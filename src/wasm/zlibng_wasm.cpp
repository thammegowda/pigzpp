// Thin zlib-ng WASM wrapper — a direct in-memory gzip baseline.
//
// This exists purely to benchmark pigzpp-wasm against a minimal binding of the
// SAME underlying engine (zlib-ng). It calls deflate() straight on in-memory
// buffers: no temp-fd facade, no threading, no PNG/multi-format machinery.

#include <emscripten/bind.h>
#include <emscripten/val.h>

#include <cstdint>
#include <stdexcept>
#include <string>
#include <vector>

#include <zlib.h>

namespace {

using emscripten::val;

std::vector<uint8_t> to_vec(const val& array) {
    return emscripten::convertJSArrayToNumberVector<uint8_t>(array);
}

val to_u8(const std::vector<uint8_t>& data) {
    val view = val(emscripten::typed_memory_view(data.size(), data.data()));
    val out = val::global("Uint8Array").new_(data.size());
    out.call<void>("set", view);
    return out;
}

// gzip-compress raw bytes with zlib-ng's deflate (windowBits 15+16 = gzip).
val gzip_compress(const val& input, int level) {
    std::vector<uint8_t> data = to_vec(input);

    z_stream zs{};
    if (deflateInit2(&zs, level, Z_DEFLATED, 15 + 16, 8, Z_DEFAULT_STRATEGY) != Z_OK)
        throw std::runtime_error("deflateInit2 failed");

    std::vector<uint8_t> out(deflateBound(&zs, data.size()));
    zs.next_in = data.data();
    zs.avail_in = static_cast<uInt>(data.size());
    zs.next_out = out.data();
    zs.avail_out = static_cast<uInt>(out.size());

    int ret = deflate(&zs, Z_FINISH);
    if (ret != Z_STREAM_END) {
        deflateEnd(&zs);
        throw std::runtime_error("deflate did not finish");
    }
    out.resize(zs.total_out);
    deflateEnd(&zs);
    return to_u8(out);
}

val gzip_decompress(const val& input) {
    std::vector<uint8_t> data = to_vec(input);

    z_stream zs{};
    if (inflateInit2(&zs, 15 + 16) != Z_OK)
        throw std::runtime_error("inflateInit2 failed");

    std::vector<uint8_t> out;
    out.resize(data.size() * 4 + 1024);
    zs.next_in = data.data();
    zs.avail_in = static_cast<uInt>(data.size());
    zs.next_out = out.data();
    zs.avail_out = static_cast<uInt>(out.size());

    for (;;) {
        int ret = inflate(&zs, Z_NO_FLUSH);
        if (ret == Z_STREAM_END) break;
        if (ret != Z_OK && ret != Z_BUF_ERROR) {
            inflateEnd(&zs);
            throw std::runtime_error("inflate failed");
        }
        if (zs.avail_out == 0) {
            size_t used = out.size();
            out.resize(out.size() * 2);
            zs.next_out = out.data() + used;
            zs.avail_out = static_cast<uInt>(out.size() - used);
        } else if (ret == Z_BUF_ERROR) {
            // Output space remains but inflate made no progress: the input
            // ended before Z_STREAM_END, i.e. the stream is truncated/corrupt.
            inflateEnd(&zs);
            throw std::runtime_error("inflate failed: truncated input");
        }
    }
    out.resize(zs.total_out);
    inflateEnd(&zs);
    return to_u8(out);
}

std::string version() { return std::string("zlib-ng ") + zlibVersion(); }

} // namespace

EMSCRIPTEN_BINDINGS(zlibng) {
    emscripten::function("version", &version);
    emscripten::function("gzipCompress", &gzip_compress);
    emscripten::function("gzipDecompress", &gzip_decompress);
}
