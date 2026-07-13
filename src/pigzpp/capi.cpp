// C ABI wrapper implementation. See capi.h.

#include "capi.h"

#include "compress.h"
#include "config.h"
#include "decompress.h"
#include "png.h"

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
                                   int level, int threads) {
    try {
        pigzpp::Config cfg;
        cfg.form = pigzpp::Format::Gzip;
        cfg.mode = pigzpp::Mode::Compress;
        cfg.level = level;
        cfg.procs = resolve_threads(threads);
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

} // extern "C"
