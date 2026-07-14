// pigzpp Python bindings via nanobind (CPython 3.12+ stable ABI).
// API mirrors Python's gzip.open() with context manager support.

#include <nanobind/nanobind.h>
#include <nanobind/ndarray.h>
#include <nanobind/stl/optional.h>
#include <nanobind/stl/string.h>
#include <nanobind/stl/vector.h>

#include "compress.h"
#include "config.h"
#include "png.h"
#include "zip.h"

#include <zlib.h>

#include <cstdlib>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <limits>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace nb = nanobind;

// GzFile: streaming gzip file API using zlib's gz* functions.
// Write mode: gzwrite() compresses and flushes incrementally — no buffering.
// Read mode: gzread() decompresses on demand — no full-file load.
class GzFile {
public:
    GzFile(std::string filename, std::string mode, int level = 6,
           int /*threads*/ = 0)
        : filename_(std::move(filename)), level_(level) {
        if (mode == "r" || mode == "rt" || mode == "rb") {
            writing_ = false;
        } else if (mode == "w" || mode == "wt" || mode == "wb") {
            writing_ = true;
        } else {
            throw std::invalid_argument("mode must be 'r'/'rt'/'rb' or 'w'/'wt'/'wb'");
        }
    }

    GzFile& enter() {
        if (gz_) throw std::runtime_error("already opened");
        std::string gzmode = writing_ ? ("wb" + std::to_string(level_ < 0 ? 6 : level_))
                                      : "rb";
        gz_ = gzopen(filename_.c_str(), gzmode.c_str());
        if (!gz_)
            throw std::runtime_error("cannot open " + filename_);
        // Set large buffer for better throughput
        gzbuffer(gz_, 1 << 18);  // 256KB internal buffer
        return *this;
    }

    void exit() {
        if (!gz_) return;
        gzclose(gz_);
        gz_ = nullptr;
    }

    nb::object read() {
        if (!gz_ || writing_)
            throw std::runtime_error("not opened for reading");

        // Read all decompressed data
        std::string result;
        char buf[262144];  // 256KB read chunks
        for (;;) {
            int n;
            {
                nb::gil_scoped_release release;
                n = gzread(gz_, buf, sizeof(buf));
            }
            if (n <= 0) break;
            result.append(buf, static_cast<size_t>(n));
        }
        return nb::str(result.data(), result.size());
    }

    void write(const std::string& data) {
        if (!gz_ || !writing_)
            throw std::runtime_error("not opened for writing");
        const char* p = data.data();
        size_t remaining = data.size();
        while (remaining > 0) {
            unsigned chunk = static_cast<unsigned>(
                remaining > 0x7FFFFFFF ? 0x7FFFFFFF : remaining);
            int written;
            {
                nb::gil_scoped_release release;
                written = gzwrite(gz_, p, chunk);
            }
            if (written <= 0)
                throw std::runtime_error("gzwrite failed");
            p += written;
            remaining -= static_cast<size_t>(written);
        }
    }

    GzFile& iter() {
        if (writing_) throw std::runtime_error("cannot iterate a write-mode file");
        return *this;
    }

    std::string next() {
        // Read one line
        char buf[65536];
        {
            nb::gil_scoped_release release;
            if (!gzgets(gz_, buf, sizeof(buf)))
                throw nb::stop_iteration();
        }
        size_t len = strlen(buf);
        if (len == 0) throw nb::stop_iteration();
        return std::string(buf, len);
    }

    ~GzFile() { exit(); }

private:
    std::string filename_;
    int level_;
    bool writing_ = false;
    gzFile gz_ = nullptr;
};


// ---- In-memory compress/decompress ----
// Small inputs use zlib directly. Input exporters are kept alive through a
// stable-ABI Py_buffer and the GIL is released around deflate/inflate.
//
// Large inputs (>= PARALLEL_MIN_BYTES) instead go through pigzpp's in-memory
// buffer API (compress_bytes_parallel), which runs the multi-threaded ISA-L
// pipeline. That costs one malloc->PyBytes copy of the (smaller) output, but
// wins ~20x on realistic data by using all cores. Raw deflate and small inputs
// stay on the single-threaded zero-copy path.
//
// Helper: read uncompressed size from gzip trailer (last 4 bytes).
// Returns 0 if input is not gzip or too short.
static uint32_t gzip_trailer_size(const unsigned char *data, size_t len) {
    if (len >= 18 && data[0] == 0x1f && data[1] == 0x8b) {
        uint32_t isize;
        memcpy(&isize, data + len - 4, 4);
        return isize;
    }
    return 0;
}

class PyBuffer {
public:
    explicit PyBuffer(nb::handle object, int flags = PyBUF_FULL_RO) {
        if (PyObject_GetBuffer(object.ptr(), &view_, flags) < 0)
            throw nb::python_error();
    }

    PyBuffer(const PyBuffer&) = delete;
    PyBuffer& operator=(const PyBuffer&) = delete;

    ~PyBuffer() { PyBuffer_Release(&view_); }

    const Py_buffer& view() const { return view_; }

    void require_c_contiguous(const char* what) const {
        if (!PyBuffer_IsContiguous(&view_, 'C'))
            throw std::invalid_argument(std::string(what) + " must be C-contiguous");
    }

private:
    Py_buffer view_{};
};

static nb::bytes
compress_bytes(const Py_buffer &buf, int level, int window_bits, int strategy)
{
    z_stream strm{};
    strm.zalloc = Z_NULL;
    strm.zfree = Z_NULL;
    strm.opaque = Z_NULL;

    int ret = deflateInit2(&strm, level == -1 ? 6 : level, Z_DEFLATED,
                           window_bits, 8, strategy);
    if (ret != Z_OK)
        throw std::runtime_error("deflateInit2 failed");

    auto* input = static_cast<unsigned char *>(buf.buf);
    size_t input_len = static_cast<size_t>(buf.len);
    size_t input_offset = 0;

    // deflateBound() takes a 32-bit uLong, so its estimate is only a starting
    // capacity; the loop below grows the buffer and feeds the input in
    // UINT_MAX-sized chunks so inputs larger than 4 GiB compress correctly.
    size_t bound = static_cast<size_t>(
        deflateBound(&strm, static_cast<uLong>(std::min<size_t>(input_len, UINT_MAX))));
    std::vector<uint8_t> result(std::max<size_t>(bound, 64));

    std::string error;
    {
        nb::gil_scoped_release release;
        for (;;) {
            if (strm.avail_in == 0 && input_offset < input_len) {
                size_t chunk = std::min<size_t>(input_len - input_offset, UINT_MAX);
                strm.next_in = input + input_offset;
                strm.avail_in = static_cast<unsigned>(chunk);
                input_offset += chunk;
            }
            if (strm.avail_out == 0) {
                size_t used = static_cast<size_t>(strm.total_out);
                if (used == result.size()) {
                    if (result.size() > std::numeric_limits<size_t>::max() / 2) {
                        error = "deflate output is too large";
                        break;
                    }
                    result.resize(std::max<size_t>(result.size() * 2, 262144));
                }
                size_t available = std::min<size_t>(result.size() - used, UINT_MAX);
                strm.next_out = result.data() + used;
                strm.avail_out = static_cast<unsigned>(available);
            }

            int flush = (input_offset == input_len) ? Z_FINISH : Z_NO_FLUSH;
            ret = deflate(&strm, flush);
            if (ret == Z_STREAM_END)
                break;
            if (ret != Z_OK && ret != Z_BUF_ERROR) {
                error = "deflate failed";
                break;
            }
        }
    }

    size_t out_size = static_cast<size_t>(strm.total_out);
    deflateEnd(&strm);
    if (!error.empty())
        throw std::runtime_error(error);
    return nb::bytes(result.data(), out_size);
}

// Threshold below which the single-threaded zero-copy path beats the parallel
// pipeline (thread spawn + coordination overhead dominates for small inputs).
static constexpr Py_ssize_t PARALLEL_MIN_BYTES = 1 << 20; // 1 MB

// Parse an engine name ("auto"/"zlib"/"isal") to the backend enum.
static pigzpp::Engine parse_engine(const char *s) {
    if (!s || !std::strcmp(s, "auto")) return pigzpp::Engine::Auto;
    if (!std::strcmp(s, "zlib") || !std::strcmp(s, "zlib-ng") ||
        !std::strcmp(s, "zlibng")) return pigzpp::Engine::Zlib;
    if (!std::strcmp(s, "isal") || !std::strcmp(s, "isa-l"))
        return pigzpp::Engine::Isal;
    return pigzpp::Engine::Auto;
}

// Parallel gzip/zlib compression via pigzpp's in-memory buffer API. Runs the
// multi-threaded pipeline (ISA-L or zlib-ng per `engine`) and copies the owned
// output into a PyBytes (one malloc->PyBytes copy). Not valid for raw deflate.
static nb::bytes
compress_bytes_parallel(const Py_buffer &buf, int level,
                        pigzpp::Format form, int strategy, int threads,
                        pigzpp::Engine engine)
{
    pigzpp::Config cfg;
    cfg.form = form;
    cfg.mode = pigzpp::Mode::Compress;
    cfg.level = level;
    cfg.strategy = static_cast<pigzpp::Strategy>(strategy);
    cfg.engine = engine;
    cfg.procs = threads > 0 ? threads
                            : static_cast<int>(std::thread::hardware_concurrency());
    if (cfg.procs < 1) cfg.procs = 1;

    uint8_t *out = nullptr;
    size_t out_size = 0;
    std::string err;

    {
        nb::gil_scoped_release release;
        try {
            pigzpp::Compressor comp(cfg);
            out_size = comp.compress_buffer(
                static_cast<const uint8_t *>(buf.buf),
                static_cast<size_t>(buf.len), &out);
        } catch (const std::exception &e) {
            err = e.what();
        } catch (...) {
            err = "pigzpp: compression failed";
        }
    }

    if (!err.empty()) {
        std::free(out);
        throw std::runtime_error(err);
    }

    nb::bytes result(out, out_size);
    std::free(out);
    return result;
}

static nb::bytes
pigzpp_compress(nb::object data, int level, const std::string& engine_name)
{
    PyBuffer buffer(data);
    buffer.require_c_contiguous("data");
    const Py_buffer& buf = buffer.view();
    pigzpp::Engine engine = parse_engine(engine_name.c_str());
    // Explicit backend, or large input -> parallel pipeline; small auto -> the
    // single-threaded zlib path.
    if (engine != pigzpp::Engine::Auto || buf.len >= PARALLEL_MIN_BYTES)
        return compress_bytes_parallel(buf, level, pigzpp::Format::Gzip,
                                       Z_DEFAULT_STRATEGY, /*threads=*/0, engine);
    return compress_bytes(buf, level, 15 + 16, Z_DEFAULT_STRATEGY);
}

static nb::bytes
pigzpp_compress_zlib(nb::object data, int level, int strategy,
                     const std::string& engine_name)
{
    PyBuffer buffer(data);
    buffer.require_c_contiguous("data");
    const Py_buffer& buf = buffer.view();
    pigzpp::Engine engine = parse_engine(engine_name.c_str());
    if (engine != pigzpp::Engine::Auto || buf.len >= PARALLEL_MIN_BYTES)
        return compress_bytes_parallel(buf, level, pigzpp::Format::Zlib,
                                       strategy, /*threads=*/0, engine);
    return compress_bytes(buf, level, 15, strategy);
}

static nb::bytes
pigzpp_compress_raw(nb::object data, int level, int strategy)
{
    PyBuffer buffer(data);
    buffer.require_c_contiguous("data");
    return compress_bytes(buffer.view(), level, -15, strategy);
}

static nb::bytes
pigzpp_decompress(nb::object data, Py_ssize_t bufsize)
{
    PyBuffer buffer(data);
    buffer.require_c_contiguous("data");
    const Py_buffer& buf = buffer.view();
    auto* ibuf = static_cast<unsigned char *>(buf.buf);
    size_t ibuflen = static_cast<size_t>(buf.len);

    z_stream strm{};
    strm.zalloc = Z_NULL;
    strm.zfree = Z_NULL;
    strm.opaque = Z_NULL;
    int ret = inflateInit2(&strm, 15 + 32);  // auto-detect gzip/zlib
    if (ret != Z_OK)
        throw std::runtime_error("inflateInit2 failed");

    if (bufsize <= 0) {
        uint32_t hint = gzip_trailer_size(ibuf, ibuflen);
        if (hint > 0) {
            bufsize = static_cast<Py_ssize_t>(hint);
            if (static_cast<size_t>(bufsize) < ibuflen)
                bufsize = static_cast<Py_ssize_t>(ibuflen * 4);
            bufsize += 256;
        } else {
            bufsize = static_cast<Py_ssize_t>(std::max<size_t>(ibuflen * 8, 262144));
        }
    }

    std::vector<uint8_t> result(static_cast<size_t>(bufsize));
    size_t input_offset = 0;
    std::string error;
    {
        nb::gil_scoped_release release;
        for (;;) {
            if (strm.avail_in == 0 && input_offset < ibuflen) {
                size_t chunk = std::min<size_t>(ibuflen - input_offset, UINT_MAX);
                strm.next_in = ibuf + input_offset;
                strm.avail_in = static_cast<unsigned>(chunk);
                input_offset += chunk;
            }
            if (strm.avail_out == 0) {
                size_t used = static_cast<size_t>(strm.total_out);
                if (used == result.size()) {
                    if (result.size() > std::numeric_limits<size_t>::max() / 2) {
                        error = "inflate output is too large";
                        break;
                    }
                    result.resize(std::max<size_t>(result.size() * 2, 262144));
                }
                size_t available = std::min<size_t>(result.size() - used, UINT_MAX);
                strm.next_out = result.data() + used;
                strm.avail_out = static_cast<unsigned>(available);
            }

            int flush = (input_offset == ibuflen && strm.avail_in == 0)
                            ? Z_FINISH : Z_NO_FLUSH;
            ret = inflate(&strm, flush);
            if (ret == Z_STREAM_END)
                break;
            if (ret != Z_OK && ret != Z_BUF_ERROR) {
                error = strm.msg ? strm.msg : "unknown error";
                break;
            }
            if (ret == Z_BUF_ERROR && flush == Z_FINISH && strm.avail_out > 0) {
                error = "unexpected end of compressed data";
                break;
            }
        }
    }

    size_t out_size = static_cast<size_t>(strm.total_out);
    inflateEnd(&strm);
    if (!error.empty())
        throw std::runtime_error("inflate failed: " + error);
    return nb::bytes(result.data(), out_size);
}

static bool is_c_contiguous_image(const Py_buffer& info) {
    if (!info.shape || !info.strides)
        return false;
    if (info.ndim == 2)
        return info.strides[1] == 1 && info.strides[0] == info.shape[1];
    if (info.ndim != 3)
        return false;
    Py_ssize_t channels = info.shape[2];
    Py_ssize_t width = info.shape[1];
    return info.strides[2] == 1 &&
           info.strides[1] == channels &&
           info.strides[0] == width * channels;
}

struct PngInput {
    const uint8_t* pixels = nullptr;
    size_t size = 0;
    uint32_t width = 0;
    uint32_t height = 0;
    uint8_t channels = 0;
};

static pigzpp::png::EncodeOptions resolve_png_options(
    const std::string& preset,
    const std::optional<int>& level,
    const std::optional<std::string>& strategy,
    const std::optional<std::string>& filter
) {
    pigzpp::png::EncodeOptions options = pigzpp::png::preset_options(preset);
    if (level) options.level = *level;
    if (strategy) options.strategy = pigzpp::png::parse_strategy(*strategy);
    if (filter) options.filter = pigzpp::png::parse_filter_mode(*filter);
    return options;
}

static std::string path_to_string(const nb::object& path) {
    return nb::cast<std::string>(nb::module_::import_("os").attr("fspath")(path));
}

static PngInput resolve_png_input(
    const Py_buffer& info,
    const std::optional<uint32_t>& width,
    const std::optional<uint32_t>& height,
    const std::optional<uint8_t>& channels
) {
    if (info.itemsize != 1)
        throw std::invalid_argument("PNG input must be a uint8/bytes buffer");

    PngInput input;
    input.pixels = static_cast<const uint8_t*>(info.buf);
    input.size = static_cast<size_t>(info.len);
    input.width = width.value_or(0);
    input.height = height.value_or(0);
    input.channels = channels.value_or(0);

    if (info.ndim == 2) {
        if (!is_c_contiguous_image(info))
            throw std::invalid_argument("PNG grayscale image input must be C-contiguous HxW uint8 data");
        input.height = height.value_or(static_cast<uint32_t>(info.shape[0]));
        input.width = width.value_or(static_cast<uint32_t>(info.shape[1]));
        input.channels = channels.value_or(1);
    } else if (info.ndim == 3) {
        if (!is_c_contiguous_image(info))
            throw std::invalid_argument("PNG image input must be C-contiguous HxWxC uint8 data");
        input.height = height.value_or(static_cast<uint32_t>(info.shape[0]));
        input.width = width.value_or(static_cast<uint32_t>(info.shape[1]));
        input.channels = channels.value_or(static_cast<uint8_t>(info.shape[2]));
    } else if (info.ndim == 1) {
        if (!PyBuffer_IsContiguous(&info, 'C'))
            throw std::invalid_argument("PNG byte input must be C-contiguous");
    } else {
        throw std::invalid_argument("PNG input must be raw bytes, HxW grayscale, or HxWxC uint8 image data");
    }
    return input;
}

static nb::bytes png_compress(
    nb::object data,
    std::optional<uint32_t> width,
    std::optional<uint32_t> height,
    std::optional<uint8_t> channels,
    const std::optional<int>& level,
    const std::optional<std::string>& strategy,
    const std::optional<std::string>& filter,
    const std::string& preset,
    const std::optional<size_t>& idat_chunk_size
) {
    PyBuffer buffer(data);
    const Py_buffer& info = buffer.view();
    PngInput input = resolve_png_input(info, width, height, channels);
    pigzpp::png::EncodeOptions options = resolve_png_options(preset, level, strategy, filter);
    if (idat_chunk_size) options.idat_chunk_size = *idat_chunk_size;
    std::vector<uint8_t> encoded;
    {
        nb::gil_scoped_release release;
        encoded = pigzpp::png::encode_buffer(
            input.pixels,
            input.size,
            input.width,
            input.height,
            input.channels,
            options
        );
    }
    return nb::bytes(encoded.data(), encoded.size());
}

static void png_save(
    nb::object path,
    nb::object data,
    std::optional<uint32_t> width,
    std::optional<uint32_t> height,
    std::optional<uint8_t> channels,
    const std::optional<int>& level,
    const std::optional<std::string>& strategy,
    const std::optional<std::string>& filter,
    const std::string& preset,
    const std::optional<size_t>& idat_chunk_size
) {
    std::string filename = path_to_string(path);
    PyBuffer buffer(data);
    const Py_buffer& info = buffer.view();
    PngInput input = resolve_png_input(info, width, height, channels);
    pigzpp::png::EncodeOptions options = resolve_png_options(preset, level, strategy, filter);
    if (idat_chunk_size) options.idat_chunk_size = *idat_chunk_size;
    {
        nb::gil_scoped_release release;
        pigzpp::png::save_buffer(
            filename,
            input.pixels,
            input.size,
            input.width,
            input.height,
            input.channels,
            options
        );
    }
}

static nb::tuple png_image_to_bytes_tuple(const pigzpp::png::Image& image) {
    nb::bytes pixels(image.pixels.data(), image.pixels.size());
    return nb::make_tuple(pixels, nb::make_tuple(image.width, image.height, image.channels));
}

static nb::object png_image_to_array(pigzpp::png::Image&& image) {
    auto* pixels = new std::vector<uint8_t>(std::move(image.pixels));
    uint8_t* data = pixels->data();
    nb::capsule owner(pixels, [](void* value) noexcept {
        delete static_cast<std::vector<uint8_t>*>(value);
    });

    size_t height = static_cast<size_t>(image.height);
    size_t width = static_cast<size_t>(image.width);
    size_t channels = static_cast<size_t>(image.channels);
    if (image.channels == 1) {
        nb::ndarray<nb::numpy, uint8_t> array(data, {height, width}, owner);
        return array.cast();
    }
    nb::ndarray<nb::numpy, uint8_t> array(data, {height, width, channels}, owner);
    return array.cast();
}

static nb::object png_image_result(pigzpp::png::Image&& image, const std::string& result) {
    if (result == "numpy" || result == "array" || result == "ndarray")
        return png_image_to_array(std::move(image));
    if (result == "bytes" || result == "tuple" || result == "raw")
        return png_image_to_bytes_tuple(image);
    throw std::invalid_argument("PNG result must be 'numpy' or 'bytes'");
}

static nb::object png_decompress(nb::object data, const std::string& result) {
    PyBuffer buffer(data);
    buffer.require_c_contiguous("PNG data");
    const Py_buffer& info = buffer.view();
    if (info.itemsize != 1)
        throw std::invalid_argument("PNG data must be a uint8/bytes buffer");
    if (info.ndim != 1)
        throw std::invalid_argument("PNG data must be a one-dimensional bytes buffer");

    pigzpp::png::Image image;
    {
        nb::gil_scoped_release release;
        image = pigzpp::png::decode(static_cast<const uint8_t*>(info.buf), static_cast<size_t>(info.len));
    }
    return png_image_result(std::move(image), result);
}

static nb::object png_decompress_array(nb::object data) {
    PyBuffer buffer(data);
    buffer.require_c_contiguous("PNG data");
    const Py_buffer& info = buffer.view();
    if (info.itemsize != 1)
        throw std::invalid_argument("PNG data must be a uint8/bytes buffer");
    if (info.ndim != 1)
        throw std::invalid_argument("PNG data must be a one-dimensional bytes buffer");

    pigzpp::png::Image image;
    {
        nb::gil_scoped_release release;
        image = pigzpp::png::decode(static_cast<const uint8_t*>(info.buf), static_cast<size_t>(info.len));
    }
    return png_image_to_array(std::move(image));
}

static nb::object png_load(nb::object path, const std::string& result) {
    std::string filename = path_to_string(path);
    pigzpp::png::Image image;
    {
        nb::gil_scoped_release release;
        image = pigzpp::png::load(filename);
    }
    return png_image_result(std::move(image), result);
}

static nb::object png_load_array(nb::object path) {
    std::string filename = path_to_string(path);
    pigzpp::png::Image image;
    {
        nb::gil_scoped_release release;
        image = pigzpp::png::load(filename);
    }
    return png_image_to_array(std::move(image));
}

// ---- ZIP archive API (mirrors Python's zipfile) --------------------------

namespace zipapi {

using pigzpp::zip::EntryInfo;
using pigzpp::zip::Method;
using pigzpp::zip::WriteOptions;
using pigzpp::zip::ZipReader;
using pigzpp::zip::ZipWriter;

// Convert unix seconds to a zipfile-style 6-tuple (year, month, day, h, m, s).
static nb::tuple date_time(int64_t mtime) {
    std::time_t t = static_cast<std::time_t>(mtime);
    std::tm tmv{};
#if defined(_WIN32)
    localtime_s(&tmv, &t);
#else
    localtime_r(&t, &tmv);
#endif
    return nb::make_tuple(tmv.tm_year + 1900, tmv.tm_mon + 1, tmv.tm_mday,
                          tmv.tm_hour, tmv.tm_min, tmv.tm_sec);
}

// pigzpp.ZipFile — a subset of zipfile.ZipFile.
//
//   ZipFile(file, mode='r', compression=ZIP_DEFLATED, compresslevel=6,
//           threads=0, engine='auto')
//
// Read mode ('r') exposes namelist/infolist/read/testzip/extract(all).
// Write ('w') / exclusive ('x') / append ('a') expose write/writestr/mkdir.
class PyZipFile {
public:
    PyZipFile(const std::string& file, const std::string& mode,
              int compression, int compresslevel, int threads,
              const std::string& engine)
        : method_(static_cast<Method>(compression)),
          level_(compresslevel),
          threads_(threads),
          engine_(parse_engine(engine.c_str())) {
        if (mode == "r") {
            reader_ = std::make_unique<ZipReader>(file);
        } else if (mode == "w" || mode == "a" || mode == "x") {
            if (mode == "x" && std::filesystem::exists(file))
                throw std::runtime_error("pigzpp.ZipFile: file exists: " + file);
            writer_ = std::make_unique<ZipWriter>(file, mode == "a" ? 'a' : 'w');
        } else {
            throw std::invalid_argument("mode must be 'r', 'w', 'x', or 'a'");
        }
    }

    PyZipFile& enter() { return *this; }
    void exit_() { close(); }

    void close() {
        if (writer_) { writer_->close(); writer_.reset(); }
        reader_.reset();
    }

    WriteOptions opts(std::optional<int> compress_type,
                      std::optional<int> compresslevel) const {
        WriteOptions o;
        o.method = compress_type ? static_cast<Method>(*compress_type) : method_;
        o.level = compresslevel ? *compresslevel : level_;
        o.threads = threads_;
        o.engine = engine_;
        return o;
    }

    void writestr(const std::string& name, nb::object data,
                  std::optional<int> compress_type,
                  std::optional<int> compresslevel) {
        require_writer();
        if (nb::isinstance<nb::str>(data)) {
            std::string s = nb::cast<std::string>(data);
            writer_->write_str(name, s, opts(compress_type, compresslevel));
        } else {
            PyBuffer buffer(data);
            buffer.require_c_contiguous("data");
            const Py_buffer& info = buffer.view();
            writer_->write_bytes(name, static_cast<const uint8_t*>(info.buf),
                                 static_cast<size_t>(info.len),
                                 opts(compress_type, compresslevel));
        }
    }

    void write(const std::string& filename, std::optional<std::string> arcname,
               std::optional<int> compress_type, std::optional<int> compresslevel) {
        require_writer();
        writer_->write_file(filename, arcname.value_or(std::string{}),
                            opts(compress_type, compresslevel));
    }

    void mkdir(const std::string& name) {
        require_writer();
        writer_->write_dir(name);
    }

    nb::bytes read(const std::string& name) {
        require_reader();
        std::vector<uint8_t> data;
        {
            nb::gil_scoped_release release;
            data = reader_->read(name);
        }
        return nb::bytes(data.data(), data.size());
    }

    std::vector<std::string> namelist() {
        require_reader();
        return reader_->namelist();
    }

    nb::list infolist() {
        require_reader();
        nb::list out;
        for (const auto& e : reader_->entries()) out.append(make_info(e));
        return out;
    }

    nb::object getinfo(const std::string& name) {
        require_reader();
        const EntryInfo* e = reader_->info(name);
        if (!e) {
            std::string message = "no such member: " + name;
            throw nb::key_error(message.c_str());
        }
        return make_info(*e);
    }

    nb::object testzip() {
        require_reader();
        std::string bad = reader_->testzip();
        if (bad.empty()) return nb::none();
        return nb::str(bad.data(), bad.size());
    }

    std::string extract(const std::string& name, const std::string& path) {
        require_reader();
        return reader_->extract(name, path);
    }

    void extractall(const std::string& path) {
        require_reader();
        reader_->extractall(path);
    }

    nb::object comment() {
        if (reader_) {
            const std::string& value = reader_->comment();
            return nb::bytes(value.data(), value.size());
        }
        return nb::bytes("", 0);
    }

    void set_comment(const std::string& c) {
        require_writer();
        writer_->set_comment(c);
    }

private:
    void require_writer() const {
        if (!writer_) throw std::runtime_error("pigzpp.ZipFile: not open for writing");
    }
    void require_reader() const {
        if (!reader_) throw std::runtime_error("pigzpp.ZipFile: not open for reading");
    }

    static nb::object make_info(const EntryInfo& e) {
        nb::dict d;
        d["filename"] = e.name;
        d["file_size"] = e.uncompressed_size;
        d["compress_size"] = e.compressed_size;
        d["CRC"] = e.crc32;
        d["compress_type"] = static_cast<int>(e.method);
        d["date_time"] = date_time(e.mtime);
        d["_is_dir"] = e.is_dir;
        d["comment"] = nb::bytes(e.comment.data(), e.comment.size());
        nb::object types = nb::module_::import_("types");
        return types.attr("SimpleNamespace")(**d);
    }

    Method method_;
    int level_;
    int threads_;
    pigzpp::Engine engine_;
    std::unique_ptr<ZipReader> reader_;
    std::unique_ptr<ZipWriter> writer_;
};

} // namespace zipapi


NB_MODULE(pigzpp, m) {
    m.doc() = "pigzpp: Fast gzip/zlib compression and PNG helpers (C++23 library with zlib-ng/ISA-L)";
    nb::class_<GzFile>(m, "open",
        "Open a gzip file for reading or writing. Use as context manager.\n"
        "\n"
        "Example:\n"
        "    with pigzpp.open('file.gz', 'wt') as f:\n"
        "        f.write('hello world')\n"
        "    with pigzpp.open('file.gz', 'rt') as f:\n"
        "        data = f.read()")
           .def(nb::init<std::string, std::string, int, int>(),
               nb::arg("filename"), nb::arg("mode") = "rt",
               nb::arg("level") = 6, nb::arg("threads") = 0)
           .def("__enter__", &GzFile::enter, nb::rv_policy::reference)
        .def("__exit__", [](GzFile& self, nb::handle, nb::handle, nb::handle) {
            self.exit();
        }, nb::arg().none(), nb::arg().none(), nb::arg().none())
        .def("close", &GzFile::exit, "Close the file and flush")
        .def("read", &GzFile::read, "Read all decompressed data as a string")
        .def("write", &GzFile::write, "Write string data to be compressed",
             nb::arg("data"))
          .def("__iter__", &GzFile::iter, nb::rv_policy::reference)
        .def("__next__", &GzFile::next);

        m.def("compress", &pigzpp_compress,
            nb::arg("data"), nb::arg("level") = 6, nb::arg("engine") = "auto",
            "Compress a bytes-like object and return gzip-compressed bytes.");
        m.def("compress_zlib", &pigzpp_compress_zlib,
            nb::arg("data"), nb::arg("level") = 6,
            nb::arg("strategy") = Z_DEFAULT_STRATEGY, nb::arg("engine") = "auto",
            "Compress a bytes-like object and return zlib-wrapped DEFLATE bytes.");
        m.def("compress_raw", &pigzpp_compress_raw,
            nb::arg("data"), nb::arg("level") = 6,
            nb::arg("strategy") = Z_DEFAULT_STRATEGY,
            "Compress a bytes-like object and return raw DEFLATE bytes.");
        m.def("decompress", &pigzpp_decompress,
            nb::arg("data"), nb::arg("bufsize") = 0,
            "Decompress a gzip or zlib bytes-like object.");

    auto png_module = m.def_submodule("png", "Fast PNG encode/decode helpers");
    png_module.def(
        "compress",
        &png_compress,
        nb::arg("data"),
        nb::arg("width") = std::nullopt,
        nb::arg("height") = std::nullopt,
        nb::arg("channels") = std::nullopt,
        nb::arg("level") = std::nullopt,
        nb::arg("strategy") = std::nullopt,
        nb::arg("filter") = std::nullopt,
        nb::arg("preset") = "fast",
        nb::arg("idat_chunk_size") = std::nullopt,
        "Compress grayscale/grayscale+alpha/RGB/RGBA uint8 image data to PNG bytes."
    );
    png_module.def(
        "decompress",
        &png_decompress,
        nb::arg("data"),
        nb::arg("result") = "bytes",
        "Decompress a supported PNG and return either result='bytes' as (pixels, (width, height, channels)) or result='numpy' as a NumPy uint8 array."
    );
    png_module.def(
        "decompress_array",
        &png_decompress_array,
        nb::arg("data"),
        "Decompress a supported PNG and return a NumPy uint8 array with shape HxW or HxWxC."
    );
    png_module.def(
        "save",
        &png_save,
        nb::arg("path"),
        nb::arg("data"),
        nb::arg("width") = std::nullopt,
        nb::arg("height") = std::nullopt,
        nb::arg("channels") = std::nullopt,
        nb::arg("level") = std::nullopt,
        nb::arg("strategy") = std::nullopt,
        nb::arg("filter") = std::nullopt,
        nb::arg("preset") = "fast",
        nb::arg("idat_chunk_size") = std::nullopt,
        "Save grayscale/grayscale+alpha/RGB/RGBA uint8 image data to a PNG file."
    );
    png_module.def(
        "load",
        &png_load,
        nb::arg("path"),
        nb::arg("result") = "numpy",
        "Load a supported PNG file and return result='numpy' as a NumPy uint8 array, or result='bytes' as (pixels, (width, height, channels))."
    );
    png_module.def(
        "load_array",
        &png_load_array,
        nb::arg("path"),
        "Load a supported PNG file and return a NumPy uint8 array with shape HxW or HxWxC."
    );

    // ZIP archive API (mirrors a subset of Python's zipfile module).
    m.attr("ZIP_STORED") = static_cast<int>(pigzpp::zip::Method::Store);
    m.attr("ZIP_DEFLATED") = static_cast<int>(pigzpp::zip::Method::Deflate);

    nb::class_<zipapi::PyZipFile>(m, "ZipFile",
        "Open a ZIP archive for reading ('r'), writing ('w'), exclusive create\n"
        "('x'), or appending ('a'). Mirrors a subset of zipfile.ZipFile.\n"
        "\n"
        "Example:\n"
        "    with pigzpp.ZipFile('out.zip', 'w') as z:\n"
        "        z.writestr('hello.txt', 'hi')\n"
        "        z.write('/path/to/file.bin')\n"
        "    with pigzpp.ZipFile('out.zip') as z:\n"
        "        print(z.namelist())\n"
        "        data = z.read('hello.txt')")
           .def(nb::init<std::string, std::string, int, int, int, std::string>(),
               nb::arg("file"), nb::arg("mode") = "r",
               nb::arg("compression") = static_cast<int>(pigzpp::zip::Method::Deflate),
               nb::arg("compresslevel") = 6, nb::arg("threads") = 0,
               nb::arg("engine") = "auto")
           .def("__enter__", &zipapi::PyZipFile::enter, nb::rv_policy::reference)
        .def("__exit__", [](zipapi::PyZipFile& self, nb::handle, nb::handle, nb::handle) {
            self.exit_();
        }, nb::arg().none(), nb::arg().none(), nb::arg().none())
        .def("close", &zipapi::PyZipFile::close, "Finalize and close the archive.")
        .def("namelist", &zipapi::PyZipFile::namelist, "List member names.")
        .def("infolist", &zipapi::PyZipFile::infolist, "List member info objects.")
        .def("getinfo", &zipapi::PyZipFile::getinfo, nb::arg("name"),
             "Return the info object for a member.")
        .def("read", &zipapi::PyZipFile::read, nb::arg("name"),
             "Read and decompress a member, returning bytes.")
        .def("writestr", &zipapi::PyZipFile::writestr,
             nb::arg("zinfo_or_arcname"), nb::arg("data"),
             nb::arg("compress_type") = std::nullopt,
             nb::arg("compresslevel") = std::nullopt,
             "Write a str/bytes payload as a member.")
           .def("write", &zipapi::PyZipFile::write, nb::arg("filename"),
               nb::arg("arcname") = std::nullopt,
               nb::arg("compress_type") = std::nullopt,
               nb::arg("compresslevel") = std::nullopt,
             "Add a file from disk to the archive.")
        .def("mkdir", &zipapi::PyZipFile::mkdir, nb::arg("name"),
             "Add a directory entry.")
        .def("testzip", &zipapi::PyZipFile::testzip,
             "Return the name of the first corrupt member, or None if all are OK.")
           .def("extract", &zipapi::PyZipFile::extract, nb::arg("member"),
               nb::arg("path") = ".", "Extract a member to path; returns the file path.")
           .def("extractall", &zipapi::PyZipFile::extractall, nb::arg("path") = ".",
             "Extract all members to path.")
        .def("setcomment", &zipapi::PyZipFile::set_comment, nb::arg("comment"),
             "Set the archive-level comment (write mode).")
        .def_prop_ro("comment", &zipapi::PyZipFile::comment,
             "The archive-level comment (bytes).");
}
