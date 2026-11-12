// C ABI wrapper implementation. See capi.h.

#include "capi.h"

#include "compress.h"
#include "config.h"
#include "decompress.h"
#include "platform.h"
#include "png.h"

#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <mutex>
#include <new>
#include <string>
#include <thread>
#include <vector>

namespace {

// Copy a std::vector into a malloc'd buffer for the C ABI. Returns a failure
// buffer (data == nullptr) if allocation fails or the vector is unexpectedly
// unusable.
pigzpp_buffer to_c_buffer(const std::vector<uint8_t>& v) {
    pigzpp_buffer out{nullptr, 0, nullptr};
    if (v.empty()) {
        // Represent empty output as a valid, zero-length, non-null allocation
        // so callers can distinguish success from failure via `error`.
        out.data = static_cast<uint8_t*>(std::malloc(1));
        if (!out.data) {
            out.error = "pigzpp: allocation failed";
            return out;
        }
        out.size = 0;
        return out;
    }
    out.data = static_cast<uint8_t*>(std::malloc(v.size()));
    if (!out.data) {
        out.error = "pigzpp: allocation failed";
        return out;
    }
    std::memcpy(out.data, v.data(), v.size());
    out.size = v.size();
    return out;
}

int resolve_threads(int threads) {
    if (threads > 0) return threads;
    int hw = static_cast<int>(std::thread::hardware_concurrency());
    return hw > 0 ? hw : 1;
}

class FileCompressProgress {
public:
    FileCompressProgress(int ind, int outd, uint64_t total_size,
                         pigzpp_compress_file_progress_callback callback);
    ~FileCompressProgress();
    bool finish();

private:
    void run();
    void report();

    int ind_, outd_;
    uint64_t total_size_;
    pigzpp_compress_file_progress_callback callback_;
    std::chrono::steady_clock::time_point start_;
    std::mutex mutex_;
    std::condition_variable cv_;
    bool stop_ = false;
    bool failed_ = false;
    std::thread thread_;
};

FileCompressProgress::FileCompressProgress(
    int ind, int outd, uint64_t total_size, pigzpp_compress_file_progress_callback callback)
    : ind_(ind), outd_(outd), total_size_(total_size), callback_(callback),
      start_(std::chrono::steady_clock::now()),
      thread_([this] { run(); }) {}

FileCompressProgress::~FileCompressProgress() {
    finish();
}

bool FileCompressProgress::finish() {
    if (!thread_.joinable()) return !failed_;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        stop_ = true;
    }
    cv_.notify_one();
    thread_.join();
    report();
    return !failed_;
}

void FileCompressProgress::run() {
    std::unique_lock<std::mutex> lock(mutex_);
    while (!cv_.wait_for(lock, std::chrono::milliseconds(200), [this] { return stop_; })) {
        lock.unlock();
        report();
        lock.lock();
        if (failed_) break;
    }
}

void FileCompressProgress::report() {
    int64_t read = pigzpp::platform::seek(ind_, 0, SEEK_CUR);
    int64_t written = pigzpp::platform::seek(outd_, 0, SEEK_CUR);
    if (read < 0 || written < 0) {
        failed_ = true;
        return;
    }
    auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - start_).count();
    callback_(total_size_, static_cast<uint64_t>(read), static_cast<uint64_t>(written),
              static_cast<uint64_t>(elapsed));
}

} // namespace

extern "C" {

pigzpp_buffer pigzpp_gzip_compress(const uint8_t* data, size_t size,
                                   int level, int threads, int engine) {
    try {
        pigzpp::Config cfg;
        cfg.form = pigzpp::Format::Gzip;
        cfg.mode = pigzpp::Mode::Compress;
        cfg.level = level;
        cfg.procs = resolve_threads(threads);
        cfg.engine = static_cast<pigzpp::Engine>(engine);
        pigzpp::Compressor comp(cfg);
        // Zero-copy handoff: the pipeline writes directly into a malloc()'d
        // buffer we return, so there is no std::vector -> malloc copy here.
        uint8_t* out = nullptr;
        size_t n = comp.compress_buffer(data, size, &out);
        return pigzpp_buffer{out, n, nullptr};
    } catch (const std::exception&) {
        return pigzpp_buffer{nullptr, 0, "pigzpp: compression failed"};
    } catch (...) {
        return pigzpp_buffer{nullptr, 0, "pigzpp: compression failed (unknown)"};
    }
}

pigzpp_buffer pigzpp_gzip_decompress(const uint8_t* data, size_t size,
                                     int threads) {
    try {
        pigzpp::Config cfg;
        cfg.form = pigzpp::Format::Gzip;
        cfg.mode = pigzpp::Mode::Decompress;
        cfg.procs = resolve_threads(threads);
        pigzpp::Decompressor dec(cfg);
        std::vector<uint8_t> out = dec.decompress_buffer(data, size);
        return to_c_buffer(out);
    } catch (const std::exception&) {
        return pigzpp_buffer{nullptr, 0, "pigzpp: decompression failed"};
    } catch (...) {
        return pigzpp_buffer{nullptr, 0, "pigzpp: decompression failed (unknown)"};
    }
}

const char* pigzpp_compress_file(const char* uncompressed_filename,
                                 const char* compressed_filename,
                                 int overwrite_existing,
                                 int level,
                                 int threads,
                                 int format,
                                 pigzpp_compress_file_progress_callback progress_callback) {
    if (!uncompressed_filename || !compressed_filename)
        return "pigzpp: null filename";
    pigzpp::Format output_format;
    switch (format) {
    case PIGZPP_FORMAT_GZIP: output_format = pigzpp::Format::Gzip; break;
    case PIGZPP_FORMAT_ZLIB: output_format = pigzpp::Format::Zlib; break;
    case PIGZPP_FORMAT_ZIP: output_format = pigzpp::Format::Zip; break;
    case PIGZPP_FORMAT_RAW: output_format = pigzpp::Format::Raw; break;
    default: return "pigzpp: invalid compression format";
    }
    int in_fd = -1, out_fd = -1;
    const char* error = nullptr;
    bool out_created = false;
    try {
        pigzpp::platform::FileStat st{};
        in_fd = pigzpp::platform::open(uncompressed_filename, O_RDONLY);
        if (in_fd < 0) return "pigzpp: cannot open input file";
        if (pigzpp::platform::fstat(in_fd, &st) != 0 || !pigzpp::platform::is_regular(st)) {
            pigzpp::platform::close(in_fd);
            return "pigzpp: input is not a regular file";
        }
        out_fd = pigzpp::platform::open(
            compressed_filename,
            O_CREAT | O_TRUNC | O_WRONLY | (overwrite_existing ? 0 : O_EXCL), 0600);
        if (out_fd < 0) {
            pigzpp::platform::close(in_fd);
            return "pigzpp: cannot create output file (exists or not writable)";
        }
        out_created = true;

        pigzpp::Config cfg;
        cfg.form = output_format;
        cfg.mode = pigzpp::Mode::Compress;
        cfg.level = level;
        cfg.procs = resolve_threads(threads);
        cfg.verbosity = 0;
        cfg.mtime = static_cast<int64_t>(st.st_mtime);
        std::string name = uncompressed_filename;
        size_t slash = name.find_last_of("/\\");
        cfg.name = slash == std::string::npos ? name : name.substr(slash + 1);

        std::unique_ptr<FileCompressProgress> progress;
        if (progress_callback)
            progress = std::make_unique<FileCompressProgress>(
                in_fd, out_fd, static_cast<uint64_t>(st.st_size), progress_callback);

        pigzpp::Compressor comp(cfg);
        comp.compress(in_fd, out_fd);
        if (progress && !progress->finish())
            error = "pigzpp: failed to read compression progress";
        progress.reset();
    } catch (const std::exception&) {
        error = "pigzpp: compression failed";
    } catch (...) {
        error = "pigzpp: compression failed (unknown)";
    }
    if (in_fd >= 0) pigzpp::platform::close(in_fd);
    if (out_fd >= 0 && pigzpp::platform::close(out_fd) != 0 && !error)
        error = "pigzpp: failed to close output file";
    if (error && out_created) pigzpp::platform::unlink(compressed_filename);
    return error;
}

void pigzpp_free(pigzpp_buffer buf) {
    std::free(buf.data);
}

pigzpp_buffer pigzpp_png_encode(const uint8_t* pixels, size_t pixel_size,
                                uint32_t width, uint32_t height, uint8_t channels,
                                int level, const char* strategy, const char* filter) {
    try {
        std::vector<uint8_t> out = pigzpp::png::encode_buffer(
            pixels, pixel_size, width, height, channels,
            level,
            strategy ? std::string(strategy) : std::string("rle"),
            filter ? std::string(filter) : std::string("up"));
        return to_c_buffer(out);
    } catch (const std::exception&) {
        return pigzpp_buffer{nullptr, 0, "pigzpp: png encode failed"};
    } catch (...) {
        return pigzpp_buffer{nullptr, 0, "pigzpp: png encode failed (unknown)"};
    }
}

pigzpp_image pigzpp_png_decode(const uint8_t* data, size_t size) {
    try {
        pigzpp::png::Image img = pigzpp::png::decode(data, size);
        pigzpp_image out{};
        out.width = img.width;
        out.height = img.height;
        out.channels = img.channels;
        out.pixel_size = img.pixels.size();
        out.pixels = static_cast<uint8_t*>(
            std::malloc(img.pixels.empty() ? 1 : img.pixels.size()));
        if (!out.pixels)
            return pigzpp_image{nullptr, 0, 0, 0, 0, "pigzpp: allocation failed"};
        if (!img.pixels.empty())
            std::memcpy(out.pixels, img.pixels.data(), img.pixels.size());
        out.error = nullptr;
        return out;
    } catch (const std::exception&) {
        return pigzpp_image{nullptr, 0, 0, 0, 0, "pigzpp: png decode failed"};
    } catch (...) {
        return pigzpp_image{nullptr, 0, 0, 0, 0, "pigzpp: png decode failed (unknown)"};
    }
}

void pigzpp_image_free(pigzpp_image img) {
    std::free(img.pixels);
}

} // extern "C"
