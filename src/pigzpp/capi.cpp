// C ABI wrapper implementation. See capi.h.

#include "capi.h"

#include "compress.h"
#include "config.h"
#include "decompress.h"
#include "platform.h"
#include "png.h"

#include <chrono>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <new>
#include <string>
#include <system_error>
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

bool valid_level(int level) {
    return level == -1 || (level >= 0 && level <= 9);
}

} // namespace

extern "C" {

pigzpp_buffer pigzpp_gzip_compress(const uint8_t* data, size_t size,
                                   int level, int threads, int engine) {
    if (!valid_level(level))
        return pigzpp_buffer{nullptr, 0, "pigzpp: invalid compression level"};
    if (engine < PIGZPP_ENGINE_AUTO || engine > PIGZPP_ENGINE_ISAL)
        return pigzpp_buffer{nullptr, 0, "pigzpp: invalid engine"};
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
    if (!data || !size)
        return {nullptr, 0, "pigzpp: decompression failed"};
    try {
        pigzpp::Config cfg;
        cfg.form = pigzpp::Format::Gzip;
        cfg.mode = pigzpp::Mode::Decompress;
        cfg.procs = resolve_threads(threads);
        pigzpp::Decompressor dec(cfg);
        uint8_t* out = nullptr;
        size_t n = dec.decompress_buffer(data, size, &out);
        return pigzpp_buffer{out, n, nullptr};
    } catch (const std::exception&) {
        return pigzpp_buffer{nullptr, 0, "pigzpp: decompression failed"};
    } catch (...) {
        return pigzpp_buffer{nullptr, 0, "pigzpp: decompression failed (unknown)"};
    }
}

int pigzpp_gzip_decompress_into(const uint8_t* data, size_t size,
                               uint8_t* dst, size_t dst_cap, size_t* written) {
    if (!data || !written || (!dst && dst_cap))
        return PIGZPP_DECOMPRESS_ERROR;
    if (size && dst_cap) {
        const auto src_addr = reinterpret_cast<uintptr_t>(data);
        const auto dst_addr = reinterpret_cast<uintptr_t>(dst);
        if (src_addr <= dst_addr ? dst_addr - src_addr < size : src_addr - dst_addr < dst_cap)
            return PIGZPP_DECOMPRESS_ERROR;
    }
    try {
        const auto n = pigzpp::decompress_buffer_into(data, size, dst, dst_cap);
        if (!n)
            return PIGZPP_DECOMPRESS_OUTPUT_TOO_SMALL;
        *written = *n;
        return PIGZPP_DECOMPRESS_OK;
    } catch (const std::exception&) {
        return PIGZPP_DECOMPRESS_ERROR;
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
    if (!valid_level(level))
        return "pigzpp: invalid compression level";
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
    pigzpp::platform::NativePath input_path, output_path;
    try {
        if (!pigzpp::platform::native_path(uncompressed_filename, input_path) ||
            !pigzpp::platform::native_path(compressed_filename, output_path))
            return "pigzpp: invalid UTF-8 filename";
        pigzpp::platform::FileStat st{};
        in_fd = pigzpp::platform::open(input_path.c_str(), O_RDONLY);
        if (in_fd < 0) return "pigzpp: cannot open input file";
        if (pigzpp::platform::fstat(in_fd, &st) != 0 || !pigzpp::platform::is_regular(st)) {
            pigzpp::platform::close(in_fd);
            return "pigzpp: input is not a regular file";
        }
        std::error_code ec;
        if (std::filesystem::equivalent(input_path, output_path, ec)) {
            pigzpp::platform::close(in_fd);
            return "pigzpp: input and output are the same file";
        }
        out_fd = pigzpp::platform::open(
            output_path.c_str(),
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
#if defined(_WIN32)
        size_t slash = name.find_last_of("/\\");
#else
        size_t slash = name.find_last_of('/');
#endif
        cfg.name = slash == std::string::npos ? name : name.substr(slash + 1);

        if (progress_callback) {
            uint64_t total_size = static_cast<uint64_t>(st.st_size);
            auto start = std::chrono::steady_clock::now();
            cfg.progress_interval = std::chrono::milliseconds(200);
            cfg.progress = [progress_callback, total_size, start](uint64_t read, uint64_t written) {
                auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                    std::chrono::steady_clock::now() - start).count();
                progress_callback(total_size, read, written, static_cast<uint64_t>(elapsed));
            };
        }

        pigzpp::Compressor(cfg).compress(in_fd, out_fd);
    } catch (const std::exception&) {
        error = "pigzpp: compression failed";
    } catch (...) {
        error = "pigzpp: compression failed (unknown)";
    }
    if (in_fd >= 0) pigzpp::platform::close(in_fd);
    if (out_fd >= 0 && pigzpp::platform::close(out_fd) != 0 && !error)
        error = "pigzpp: failed to close output file";
    if (error && out_created) pigzpp::platform::unlink(output_path.c_str());
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
