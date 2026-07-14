// C ABI wrapper implementation. See capi.h.

#include "capi.h"

#include "compress.h"
#include "config.h"
#include "decompress.h"
#include "png.h"
#include "zip.h"

#include <cstdlib>
#include <cstring>
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

// ---- ZIP archives --------------------------------------------------------

} // extern "C"

// Opaque handle definitions live outside extern "C" (they hold C++ objects).
struct pigzpp_zip_writer {
    pigzpp::zip::ZipWriter w;   // in-memory
};

struct pigzpp_zip_reader {
    pigzpp::zip::ZipReader r;
    std::string testzip_result;
    explicit pigzpp_zip_reader(std::vector<uint8_t> bytes) : r(std::move(bytes)) {}
};

extern "C" {

pigzpp_zip_writer* pigzpp_zip_writer_new(void) {
    try {
        return new pigzpp_zip_writer();
    } catch (...) {
        return nullptr;
    }
}

const char* pigzpp_zip_writer_add(pigzpp_zip_writer* w, const char* name,
                                  const uint8_t* data, size_t size,
                                  int method, int level, int threads, int engine) {
    if (!w || !name) return "pigzpp: null argument";
    try {
        pigzpp::zip::WriteOptions opts;
        opts.method = static_cast<pigzpp::zip::Method>(method);
        opts.level = level;
        opts.threads = threads;
        opts.engine = static_cast<pigzpp::Engine>(engine);
        w->w.write_bytes(name, data, size, opts);
        return nullptr;
    } catch (const std::exception&) {
        return "pigzpp: zip add failed";
    } catch (...) {
        return "pigzpp: zip add failed (unknown)";
    }
}

void pigzpp_zip_writer_set_comment(pigzpp_zip_writer* w, const char* comment) {
    if (w && comment) {
        try { w->w.set_comment(comment); } catch (...) {}
    }
}

pigzpp_buffer pigzpp_zip_writer_finish(pigzpp_zip_writer* w) {
    if (!w) return pigzpp_buffer{nullptr, 0, "pigzpp: null writer"};
    pigzpp_buffer out{nullptr, 0, nullptr};
    try {
        std::vector<uint8_t> archive = w->w.finish();
        out = to_c_buffer(archive);
    } catch (const std::exception&) {
        out = pigzpp_buffer{nullptr, 0, "pigzpp: zip finish failed"};
    } catch (...) {
        out = pigzpp_buffer{nullptr, 0, "pigzpp: zip finish failed (unknown)"};
    }
    delete w;
    return out;
}

void pigzpp_zip_writer_free(pigzpp_zip_writer* w) {
    delete w;
}

pigzpp_zip_reader* pigzpp_zip_reader_open(const uint8_t* data, size_t size,
                                          const char** error) {
    try {
        std::vector<uint8_t> bytes(data, data + size);
        return new pigzpp_zip_reader(std::move(bytes));
    } catch (const std::exception&) {
        if (error) *error = "pigzpp: cannot open zip archive";
        return nullptr;
    } catch (...) {
        if (error) *error = "pigzpp: cannot open zip archive (unknown)";
        return nullptr;
    }
}

size_t pigzpp_zip_reader_count(const pigzpp_zip_reader* r) {
    return r ? r->r.entries().size() : 0;
}

int pigzpp_zip_reader_entry(const pigzpp_zip_reader* r, size_t index,
                            pigzpp_zip_entry* out) {
    if (!r || !out || index >= r->r.entries().size()) return 0;
    const pigzpp::zip::EntryInfo& e = r->r.entries()[index];
    out->name = e.name.c_str();
    out->compressed_size = e.compressed_size;
    out->uncompressed_size = e.uncompressed_size;
    out->crc32 = e.crc32;
    out->method = static_cast<int>(e.method);
    out->is_dir = e.is_dir ? 1 : 0;
    return 1;
}

pigzpp_buffer pigzpp_zip_reader_read(const pigzpp_zip_reader* r, const char* name) {
    if (!r || !name) return pigzpp_buffer{nullptr, 0, "pigzpp: null argument"};
    try {
        return to_c_buffer(r->r.read(name));
    } catch (const std::exception&) {
        return pigzpp_buffer{nullptr, 0, "pigzpp: zip read failed"};
    } catch (...) {
        return pigzpp_buffer{nullptr, 0, "pigzpp: zip read failed (unknown)"};
    }
}

pigzpp_buffer pigzpp_zip_reader_read_index(const pigzpp_zip_reader* r, size_t index) {
    if (!r) return pigzpp_buffer{nullptr, 0, "pigzpp: null reader"};
    try {
        return to_c_buffer(r->r.read(index));
    } catch (const std::exception&) {
        return pigzpp_buffer{nullptr, 0, "pigzpp: zip read failed"};
    } catch (...) {
        return pigzpp_buffer{nullptr, 0, "pigzpp: zip read failed (unknown)"};
    }
}

const char* pigzpp_zip_reader_comment(const pigzpp_zip_reader* r) {
    return r ? r->r.comment().c_str() : nullptr;
}

const char* pigzpp_zip_reader_testzip(pigzpp_zip_reader* r) {
    if (!r) return nullptr;
    try {
        r->testzip_result = r->r.testzip();
    } catch (...) {
        r->testzip_result = "";
    }
    return r->testzip_result.empty() ? nullptr : r->testzip_result.c_str();
}

void pigzpp_zip_reader_free(pigzpp_zip_reader* r) {
    delete r;
}

} // extern "C"
