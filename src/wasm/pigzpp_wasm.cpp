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
#include "zip.h"

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
#ifndef PIGZPP_VERSION
#define PIGZPP_VERSION "unknown"
#endif
    return "pigzpp-wasm " PIGZPP_VERSION;
}

bool threads_enabled() {
#ifdef PIGZPP_WASM_THREADS_ENABLED
    return true;
#else
    return false;
#endif
}

// ---- ZIP archives ----

// Number of worker threads for a member's DEFLATE step. Non-threaded builds
// must stay single-threaded regardless of the requested count.
int zip_threads(int threads) {
#ifdef PIGZPP_WASM_THREADS_ENABLED
    // 0 means "auto" in the core API; clamp negatives to 1 to avoid surprises.
    return threads < 0 ? 1 : threads;
#else
    (void)threads;
    return 1;
#endif
}

// In-memory ZIP writer exposed to JS. Add members, then call finish() to get
// the archive as a Uint8Array.
class WasmZipWriter {
public:
    // Add a member. `method` is 0 (store) or 8 (deflate). `data` is a Uint8Array.
    void add(const std::string& name, const val& data, int method, int level,
             int threads) {
        if (method != 0 && method != 8)
            throw std::runtime_error("ZipWriter.add: method must be 0 (store) or 8 (deflate)");
        std::vector<uint8_t> bytes = to_vec(data);
        pigzpp::zip::WriteOptions opts;
        opts.method = static_cast<pigzpp::zip::Method>(method);
        opts.level = level;
        opts.threads = zip_threads(threads);
        writer_.write_bytes(name, bytes.data(), bytes.size(), opts);
    }

    void setComment(const std::string& comment) { writer_.set_comment(comment); }

    // Finalize and return the archive bytes.
    val finish() {
        std::vector<uint8_t> archive = writer_.finish();
        return to_u8(archive);
    }

private:
    pigzpp::zip::ZipWriter writer_;
};

// In-memory ZIP reader exposed to JS.
class WasmZipReader {
public:
    explicit WasmZipReader(const val& data) : reader_(to_vec(data)) {}

    // List of member names.
    val names() const {
        val arr = val::array();
        int i = 0;
        for (const auto& n : reader_.namelist()) arr.set(i++, n);
        return arr;
    }

    // Member metadata: { name, size, compressedSize, crc32, method, isDir }.
    val infolist() const {
        val arr = val::array();
        int i = 0;
        for (const auto& e : reader_.entries()) {
            val o = val::object();
            o.set("name", e.name);
            o.set("size", static_cast<double>(e.uncompressed_size));
            o.set("compressedSize", static_cast<double>(e.compressed_size));
            o.set("crc32", static_cast<double>(e.crc32));
            o.set("method", static_cast<int>(e.method));
            o.set("isDir", e.is_dir);
            arr.set(i++, o);
        }
        return arr;
    }

    // Read + decompress a member by name.
    val read(const std::string& name) const {
        return to_u8(reader_.read(name));
    }

    // Name of the first corrupt member, or "" if the archive is intact.
    std::string testzip() const { return reader_.testzip(); }

    std::string comment() const { return reader_.comment(); }

private:
    pigzpp::zip::ZipReader reader_;
};

} // namespace

EMSCRIPTEN_BINDINGS(pigzpp) {
    emscripten::function("version", &version);
    emscripten::function("threadsEnabled", &threads_enabled);
    emscripten::function("gzipCompress", &gzip_compress);
    emscripten::function("gzipDecompress", &gzip_decompress);
    emscripten::function("pngEncode", &png_encode);
    emscripten::function("pngDecode", &png_decode);

    emscripten::class_<WasmZipWriter>("ZipWriter")
        .constructor<>()
        .function("add", &WasmZipWriter::add)
        .function("setComment", &WasmZipWriter::setComment)
        .function("finish", &WasmZipWriter::finish);

    emscripten::class_<WasmZipReader>("ZipReader")
        .constructor<val>()
        .function("names", &WasmZipReader::names)
        .function("infolist", &WasmZipReader::infolist)
        .function("read", &WasmZipReader::read)
        .function("testzip", &WasmZipReader::testzip)
        .function("comment", &WasmZipReader::comment);
}
