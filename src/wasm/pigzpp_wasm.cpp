// pigzpp WebAssembly bindings (Embind).
//
// Exposes gzip compress/decompress and PNG encode/decode to JavaScript.
// Byte data crosses the boundary as Uint8Array; each returned buffer is copied
// into a JS-owned Uint8Array so it stays valid after the call returns.

#include <emscripten/bind.h>
#include <emscripten/val.h>

#include <cstdint>
#include <cstdlib>
#include <string>
#include <vector>

#include "compress.h"
#include "decompress.h"
#include "png.h"

namespace {

using emscripten::val;

// Convert an incoming JS Uint8Array (or number array) to a byte vector.
std::vector<uint8_t> to_vec(const val& array) {
    return emscripten::convertJSArrayToNumberVector<uint8_t>(array);
}

// Copy a byte vector into a fresh JS-owned Uint8Array.
val to_u8(const std::vector<uint8_t>& data) {
    val view = val(emscripten::typed_memory_view(data.size(), data.data()));
    val out = val::global("Uint8Array").new_(data.size());
    out.call<void>("set", view);
    return out;
}

// Copy a raw byte buffer into a fresh JS-owned Uint8Array.
val to_u8(const uint8_t* data, size_t size) {
    val view = val(emscripten::typed_memory_view(size, data));
    val out = val::global("Uint8Array").new_(size);
    out.call<void>("set", view);
    return out;
}

int normalize_threads(int threads) {
#ifdef PIGZPP_WASM_THREADS_ENABLED
    if (threads <= 0) return 1;
    return threads;
#else
    // Non-threaded build: always single-threaded regardless of request.
    (void)threads;
    return 1;
#endif
}

// gzip-compress raw bytes. strategy is a zlib strategy name
// ("default", "filtered", "huffman", "rle", "fixed").
val gzip_compress(const val& input, int level, const std::string& strategy, int threads) {
    std::vector<uint8_t> data = to_vec(input);

    pigzpp::Config cfg;
    cfg.mode = pigzpp::Mode::Compress;
    cfg.form = pigzpp::Format::Gzip;
    cfg.level = level;
    cfg.strategy = pigzpp::png::parse_strategy(strategy);
    cfg.procs = normalize_threads(threads);

    pigzpp::Compressor comp(cfg);
    // Single buffer API: returns a malloc()'d owned buffer we copy into a
    // JS-owned Uint8Array, then free.
    uint8_t* out = nullptr;
    size_t n = comp.compress_buffer(data.data(), data.size(), &out);
    val result = to_u8(out, n);
    std::free(out);
    return result;
}

// gzip-decompress bytes (also accepts zlib/zip streams pigzpp recognizes).
val gzip_decompress(const val& input, int threads) {
    std::vector<uint8_t> data = to_vec(input);

    pigzpp::Config cfg;
    cfg.mode = pigzpp::Mode::Decompress;
    cfg.procs = normalize_threads(threads);

    pigzpp::Decompressor dec(cfg);
    return to_u8(dec.decompress_buffer(data.data(), data.size()));
}

// Encode raw pixels (row-major, `channels` bytes per pixel) to a PNG.
val png_encode(const val& pixels, uint32_t width, uint32_t height, uint32_t channels,
               int level, const std::string& strategy, const std::string& filter) {
    std::vector<uint8_t> px = to_vec(pixels);

    pigzpp::png::EncodeOptions opts;
    opts.level = level;
    opts.strategy = pigzpp::png::parse_strategy(strategy);
    opts.filter = pigzpp::png::parse_filter_mode(filter);

    std::vector<uint8_t> out = pigzpp::png::encode_buffer(
        px.data(), px.size(), width, height,
        static_cast<uint8_t>(channels), opts);
    return to_u8(out);
}

// Decode a PNG to { width, height, channels, pixels: Uint8Array }.
val png_decode(const val& input) {
    std::vector<uint8_t> data = to_vec(input);
    pigzpp::png::Image img = pigzpp::png::decode(data.data(), data.size());

    val result = val::object();
    result.set("width", img.width);
    result.set("height", img.height);
    result.set("channels", static_cast<uint32_t>(img.channels));
    result.set("pixels", to_u8(img.pixels));
    return result;
}

std::string version() {
    return "pigzpp-wasm 1.1.0";
}

bool threads_enabled() {
#ifdef PIGZPP_WASM_THREADS_ENABLED
    return true;
#else
    return false;
#endif
}

} // namespace

EMSCRIPTEN_BINDINGS(pigzpp) {
    emscripten::function("version", &version);
    emscripten::function("threadsEnabled", &threads_enabled);
    emscripten::function("gzipCompress", &gzip_compress);
    emscripten::function("gzipDecompress", &gzip_decompress);
    emscripten::function("pngEncode", &png_encode);
    emscripten::function("pngDecode", &png_decode);
}
