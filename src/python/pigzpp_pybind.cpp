// pigzpp Python bindings via pybind11
// API mirrors Python's gzip.open() with context manager support.

#include <pybind11/pybind11.h>
#include <pybind11/numpy.h>
#include <pybind11/stl.h>

#include "compress.h"
#include "config.h"
#include "png.h"
#include "zip.h"

#include <zlib.h>

#include <cstdlib>
#include <cstring>
#include <ctime>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#include <unistd.h>

namespace py = pybind11;

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

    py::object read() {
        if (!gz_ || writing_)
            throw std::runtime_error("not opened for reading");

        // Read all decompressed data
        std::string result;
        char buf[262144];  // 256KB read chunks
        for (;;) {
            int n;
            {
                py::gil_scoped_release release;
                n = gzread(gz_, buf, sizeof(buf));
            }
            if (n <= 0) break;
            result.append(buf, static_cast<size_t>(n));
        }
        return py::str(result);
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
                py::gil_scoped_release release;
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
            py::gil_scoped_release release;
            if (!gzgets(gz_, buf, sizeof(buf)))
                throw py::stop_iteration();
        }
        size_t len = strlen(buf);
        if (len == 0) throw py::stop_iteration();
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
// Small inputs use zlib directly via the CPython C API, which is optimal on
// copies: the Py_buffer input is read in place and deflate writes straight
// into the result PyBytes (zero intermediate copy), with the GIL released
// around the deflate/inflate call.
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

// Helper: grow a PyBytes object in-place (double or to max).
// Returns new capacity, or -1 on error.
static Py_ssize_t
arrange_output_buffer(z_stream *zst, PyObject **buffer, Py_ssize_t length)
{
    Py_ssize_t occupied = zst->next_out - (unsigned char *)PyBytes_AS_STRING(*buffer);

    if (*buffer == NULL) {
        *buffer = PyBytes_FromStringAndSize(NULL, length);
        if (*buffer == NULL) return -1;
        occupied = 0;
    } else if (length == occupied) {
        Py_ssize_t new_length = length <= (PY_SSIZE_T_MAX >> 1) ? length << 1 : PY_SSIZE_T_MAX;
        if (_PyBytes_Resize(buffer, new_length) < 0)
            return -1;
        length = new_length;
    }

    zst->avail_out = (unsigned)Py_MIN((size_t)(length - occupied), UINT_MAX);
    zst->next_out = (unsigned char *)PyBytes_AS_STRING(*buffer) + occupied;
    return length;
}

static PyObject *
compress_bytes(const Py_buffer &buf, int level, int window_bits, int strategy)
{
    z_stream strm{};
    strm.zalloc = Z_NULL;
    strm.zfree = Z_NULL;
    strm.opaque = Z_NULL;

    int ret = deflateInit2(&strm, level == -1 ? 6 : level, Z_DEFLATED,
                           window_bits, 8, strategy);
    if (ret != Z_OK) {
        PyErr_SetString(PyExc_RuntimeError, "deflateInit2 failed");
        return NULL;
    }

    strm.next_in = static_cast<unsigned char *>(buf.buf);
    strm.avail_in = static_cast<unsigned>(buf.len);

    Py_ssize_t bound = static_cast<Py_ssize_t>(deflateBound(&strm, strm.avail_in));
    PyObject *result = PyBytes_FromStringAndSize(NULL, bound);
    if (!result) {
        deflateEnd(&strm);
        return NULL;
    }

    strm.next_out = (unsigned char *)PyBytes_AS_STRING(result);
    strm.avail_out = static_cast<unsigned>(bound);

    Py_BEGIN_ALLOW_THREADS
    ret = deflate(&strm, Z_FINISH);
    Py_END_ALLOW_THREADS

    if (ret != Z_STREAM_END) {
        deflateEnd(&strm);
        Py_DECREF(result);
        PyErr_SetString(PyExc_RuntimeError, "deflate failed");
        return NULL;
    }

    Py_ssize_t out_size = static_cast<Py_ssize_t>(strm.total_out);
    deflateEnd(&strm);

    if (_PyBytes_Resize(&result, out_size) < 0)
        return NULL;

    return result;
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
static PyObject *
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

    Py_BEGIN_ALLOW_THREADS
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
    Py_END_ALLOW_THREADS

    if (!err.empty()) {
        std::free(out);
        PyErr_SetString(PyExc_RuntimeError, err.c_str());
        return NULL;
    }

    PyObject *result = PyBytes_FromStringAndSize(
        reinterpret_cast<const char *>(out), static_cast<Py_ssize_t>(out_size));
    std::free(out);
    return result; // NULL propagates on allocation failure
}

static PyObject *
pigzpp_compress(PyObject * /*module*/, PyObject *args, PyObject *kwargs)
{
    static const char *kwlist[] = {"data", "level", "engine", NULL};
    Py_buffer buf;
    int level = 6;
    const char *engine_s = "auto";

    if (!PyArg_ParseTupleAndKeywords(args, kwargs, "y*|is",
                                     const_cast<char**>(kwlist),
                                     &buf, &level, &engine_s))
        return NULL;

    pigzpp::Engine engine = parse_engine(engine_s);
    PyObject *result;
    // Explicit backend, or large input -> parallel pipeline; small auto -> the
    // single-threaded zero-copy zlib path.
    if (engine != pigzpp::Engine::Auto || buf.len >= PARALLEL_MIN_BYTES)
        result = compress_bytes_parallel(buf, level, pigzpp::Format::Gzip,
                                         Z_DEFAULT_STRATEGY, /*threads=*/0, engine);
    else
        result = compress_bytes(buf, level, 15 + 16, Z_DEFAULT_STRATEGY);
    PyBuffer_Release(&buf);
    return result;
}

static PyObject *
pigzpp_compress_zlib(PyObject * /*module*/, PyObject *args, PyObject *kwargs)
{
    static const char *kwlist[] = {"data", "level", "strategy", "engine", NULL};
    Py_buffer buf;
    int level = 6;
    int strategy = Z_DEFAULT_STRATEGY;
    const char *engine_s = "auto";

    if (!PyArg_ParseTupleAndKeywords(args, kwargs, "y*|iis",
                                     const_cast<char**>(kwlist),
                                     &buf, &level, &strategy, &engine_s))
        return NULL;

    pigzpp::Engine engine = parse_engine(engine_s);
    PyObject *result;
    if (engine != pigzpp::Engine::Auto || buf.len >= PARALLEL_MIN_BYTES)
        result = compress_bytes_parallel(buf, level, pigzpp::Format::Zlib,
                                         strategy, /*threads=*/0, engine);
    else
        result = compress_bytes(buf, level, 15, strategy);
    PyBuffer_Release(&buf);
    return result;
}

static PyObject *
pigzpp_compress_raw(PyObject * /*module*/, PyObject *args, PyObject *kwargs)
{
    static const char *kwlist[] = {"data", "level", "strategy", NULL};
    Py_buffer buf;
    int level = 6;
    int strategy = Z_DEFAULT_STRATEGY;

    if (!PyArg_ParseTupleAndKeywords(args, kwargs, "y*|ii",
                                     const_cast<char**>(kwlist),
                                     &buf, &level, &strategy))
        return NULL;

    PyObject *result = compress_bytes(buf, level, -15, strategy);
    PyBuffer_Release(&buf);
    return result;
}

static PyObject *
pigzpp_decompress(PyObject * /*module*/, PyObject *args, PyObject *kwargs)
{
    static const char *kwlist[] = {"data", "bufsize", NULL};
    Py_buffer buf;
    Py_ssize_t bufsize = 0;

    if (!PyArg_ParseTupleAndKeywords(args, kwargs, "y*|n",
                                     const_cast<char**>(kwlist),
                                     &buf, &bufsize))
        return NULL;

    unsigned char *ibuf = static_cast<unsigned char *>(buf.buf);
    Py_ssize_t ibuflen = buf.len;

    z_stream strm{};
    strm.zalloc = Z_NULL;
    strm.zfree = Z_NULL;
    strm.opaque = Z_NULL;
    strm.next_in = ibuf;
    strm.avail_in = 0;

    int ret = inflateInit2(&strm, 15 + 32);  // auto-detect gzip/zlib
    if (ret != Z_OK) {
        PyBuffer_Release(&buf);
        PyErr_SetString(PyExc_RuntimeError, "inflateInit2 failed");
        return NULL;
    }

    if (bufsize <= 0) {
        uint32_t hint = gzip_trailer_size(ibuf, static_cast<size_t>(ibuflen));
        if (hint > 0) {
            bufsize = static_cast<Py_ssize_t>(hint);
            if (bufsize < ibuflen) bufsize = ibuflen * 4;
            bufsize += 256;
        } else {
            bufsize = Py_MAX(ibuflen * 8, 262144);
        }
    }

    // Allocate output buffer
    PyObject *result = PyBytes_FromStringAndSize(NULL, bufsize);
    if (!result) {
        inflateEnd(&strm);
        PyBuffer_Release(&buf);
        return NULL;
    }

    // Set up stream and decompress
    strm.avail_in = static_cast<unsigned>(Py_MIN((size_t)ibuflen, UINT_MAX));
    strm.next_out = (unsigned char *)PyBytes_AS_STRING(result);
    strm.avail_out = static_cast<unsigned>(Py_MIN((size_t)bufsize, UINT_MAX));

    // Fast path: single inflate call when buffer is large enough
    Py_BEGIN_ALLOW_THREADS
    ret = inflate(&strm, Z_FINISH);
    Py_END_ALLOW_THREADS

    if (ret == Z_STREAM_END) {
        PyBuffer_Release(&buf);
        inflateEnd(&strm);
        if (_PyBytes_Resize(&result, static_cast<Py_ssize_t>(strm.total_out)) < 0)
            return NULL;
        return result;
    }

    // Slow path: buffer too small or multi-chunk input — grow and retry
    if (ret == Z_OK || ret == Z_BUF_ERROR) {
        ibuflen -= strm.avail_in ? 0 : ibuflen; // track remaining input
        do {
            if (strm.avail_in == 0 && ibuflen > 0) {
                unsigned chunk = (unsigned)Py_MIN((size_t)ibuflen, UINT_MAX);
                strm.avail_in = chunk;
                ibuflen -= chunk;
            }
            if (strm.avail_out == 0) {
                bufsize = arrange_output_buffer(&strm, &result, bufsize);
                if (bufsize < 0) {
                    inflateEnd(&strm);
                    PyBuffer_Release(&buf);
                    Py_XDECREF(result);
                    return NULL;
                }
            }

            Py_BEGIN_ALLOW_THREADS
            ret = inflate(&strm, strm.avail_in == 0 ? Z_FINISH : Z_NO_FLUSH);
            Py_END_ALLOW_THREADS

            if (ret != Z_OK && ret != Z_BUF_ERROR && ret != Z_STREAM_END)
                break;
        } while (ret != Z_STREAM_END);
    }

    PyBuffer_Release(&buf);

    if (ret != Z_STREAM_END) {
        inflateEnd(&strm);
        Py_XDECREF(result);
        PyErr_Format(PyExc_RuntimeError, "inflate failed: %s",
                     strm.msg ? strm.msg : "unknown error");
        return NULL;
    }

    inflateEnd(&strm);

    if (_PyBytes_Resize(&result,
            strm.next_out - (unsigned char *)PyBytes_AS_STRING(result)) < 0)
        return NULL;

    return result;
}

static PyMethodDef pigzpp_c_methods[] = {
    {"compress", (PyCFunction)pigzpp_compress, METH_VARARGS | METH_KEYWORDS,
     "Compress bytes and return gzip-compressed bytes.\n\n"
     "Args:\n"
     "    data: bytes-like object to compress\n"
     "    level: compression level 0-9 (default 6)\n"},
    {"compress_zlib", (PyCFunction)pigzpp_compress_zlib, METH_VARARGS | METH_KEYWORDS,
     "Compress bytes and return zlib-wrapped DEFLATE bytes, suitable for PNG IDAT.\n\n"
     "Args:\n"
     "    data: bytes-like object to compress\n"
     "    level: compression level 0-9 (default 6)\n"
     "    strategy: zlib strategy integer (default Z_DEFAULT_STRATEGY)\n"},
    {"compress_raw", (PyCFunction)pigzpp_compress_raw, METH_VARARGS | METH_KEYWORDS,
     "Compress bytes and return raw DEFLATE bytes with no gzip or zlib wrapper.\n\n"
     "Args:\n"
     "    data: bytes-like object to compress\n"
     "    level: compression level 0-9 (default 6)\n"
     "    strategy: zlib strategy integer (default Z_DEFAULT_STRATEGY)\n"},
    {"decompress", (PyCFunction)pigzpp_decompress, METH_VARARGS | METH_KEYWORDS,
     "Decompress gzip-compressed bytes.\n\n"
     "Args:\n"
     "    data: bytes-like object containing gzip data\n"
     "    bufsize: initial output buffer size (default: auto). Set to expected\n"
     "        decompressed size for best performance with large data.\n"},
    {NULL, NULL, 0, NULL}
};

static bool is_c_contiguous_image(const py::buffer_info& info) {
    if (info.ndim == 2)
        return info.strides[1] == 1 && info.strides[0] == info.shape[1];
    if (info.ndim != 3)
        return false;
    ssize_t channels = info.shape[2];
    ssize_t width = info.shape[1];
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

static std::string path_to_string(const py::object& path) {
    return py::module_::import("os").attr("fspath")(path).cast<std::string>();
}

static PngInput resolve_png_input(
    const py::buffer_info& info,
    const std::optional<uint32_t>& width,
    const std::optional<uint32_t>& height,
    const std::optional<uint8_t>& channels
) {
    if (info.itemsize != 1)
        throw std::invalid_argument("PNG input must be a uint8/bytes buffer");

    PngInput input;
    input.pixels = static_cast<const uint8_t*>(info.ptr);
    input.size = static_cast<size_t>(info.size);
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
    } else if (info.ndim != 1) {
        throw std::invalid_argument("PNG input must be raw bytes, HxW grayscale, or HxWxC uint8 image data");
    }
    return input;
}

static py::bytes png_compress(
    py::buffer data,
    std::optional<uint32_t> width,
    std::optional<uint32_t> height,
    std::optional<uint8_t> channels,
    const std::optional<int>& level,
    const std::optional<std::string>& strategy,
    const std::optional<std::string>& filter,
    const std::string& preset,
    const std::optional<size_t>& idat_chunk_size
) {
    py::buffer_info info = data.request();
    PngInput input = resolve_png_input(info, width, height, channels);
    pigzpp::png::EncodeOptions options = resolve_png_options(preset, level, strategy, filter);
    if (idat_chunk_size) options.idat_chunk_size = *idat_chunk_size;
    std::vector<uint8_t> encoded;
    {
        py::gil_scoped_release release;
        encoded = pigzpp::png::encode_buffer(
            input.pixels,
            input.size,
            input.width,
            input.height,
            input.channels,
            options
        );
    }
    return py::bytes(reinterpret_cast<const char*>(encoded.data()), encoded.size());
}

static void png_save(
    py::object path,
    py::buffer data,
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
    py::buffer_info info = data.request();
    PngInput input = resolve_png_input(info, width, height, channels);
    pigzpp::png::EncodeOptions options = resolve_png_options(preset, level, strategy, filter);
    if (idat_chunk_size) options.idat_chunk_size = *idat_chunk_size;
    {
        py::gil_scoped_release release;
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

static py::tuple png_image_to_bytes_tuple(const pigzpp::png::Image& image) {
    py::bytes pixels(reinterpret_cast<const char*>(image.pixels.data()), image.pixels.size());
    return py::make_tuple(pixels, py::make_tuple(image.width, image.height, image.channels));
}

static py::array png_image_to_array(pigzpp::png::Image&& image) {
    auto pixels = std::make_unique<std::vector<uint8_t>>(std::move(image.pixels));
    uint8_t* data = pixels->data();
    py::capsule owner(pixels.release(), [](void* value) {
        delete static_cast<std::vector<uint8_t>*>(value);
    });

    ssize_t height = static_cast<ssize_t>(image.height);
    ssize_t width = static_cast<ssize_t>(image.width);
    ssize_t channels = static_cast<ssize_t>(image.channels);
    if (image.channels == 1) {
        return py::array(
            py::dtype::of<uint8_t>(),
            {height, width},
            {width, ssize_t{1}},
            data,
            owner
        );
    }
    return py::array(
        py::dtype::of<uint8_t>(),
        {height, width, channels},
        {width * channels, channels, ssize_t{1}},
        data,
        owner
    );
}

static py::object png_image_result(pigzpp::png::Image&& image, const std::string& result) {
    if (result == "numpy" || result == "array" || result == "ndarray")
        return png_image_to_array(std::move(image));
    if (result == "bytes" || result == "tuple" || result == "raw")
        return png_image_to_bytes_tuple(image);
    throw std::invalid_argument("PNG result must be 'numpy' or 'bytes'");
}

static py::object png_decompress(py::buffer data, const std::string& result) {
    py::buffer_info info = data.request();
    if (info.itemsize != 1)
        throw std::invalid_argument("PNG data must be a uint8/bytes buffer");
    if (info.ndim != 1)
        throw std::invalid_argument("PNG data must be a one-dimensional bytes buffer");

    pigzpp::png::Image image;
    {
        py::gil_scoped_release release;
        image = pigzpp::png::decode(static_cast<const uint8_t*>(info.ptr), static_cast<size_t>(info.size));
    }
    return png_image_result(std::move(image), result);
}

static py::array png_decompress_array(py::buffer data) {
    py::buffer_info info = data.request();
    if (info.itemsize != 1)
        throw std::invalid_argument("PNG data must be a uint8/bytes buffer");
    if (info.ndim != 1)
        throw std::invalid_argument("PNG data must be a one-dimensional bytes buffer");

    pigzpp::png::Image image;
    {
        py::gil_scoped_release release;
        image = pigzpp::png::decode(static_cast<const uint8_t*>(info.ptr), static_cast<size_t>(info.size));
    }
    return png_image_to_array(std::move(image));
}

static py::object png_load(py::object path, const std::string& result) {
    std::string filename = path_to_string(path);
    pigzpp::png::Image image;
    {
        py::gil_scoped_release release;
        image = pigzpp::png::load(filename);
    }
    return png_image_result(std::move(image), result);
}

static py::array png_load_array(py::object path) {
    std::string filename = path_to_string(path);
    pigzpp::png::Image image;
    {
        py::gil_scoped_release release;
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
static py::tuple date_time(int64_t mtime) {
    std::time_t t = static_cast<std::time_t>(mtime);
    std::tm tmv{};
    localtime_r(&t, &tmv);
    return py::make_tuple(tmv.tm_year + 1900, tmv.tm_mon + 1, tmv.tm_mday,
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
            if (mode == "x" && ::access(file.c_str(), F_OK) == 0)
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

    void writestr(const std::string& name, py::object data,
                  std::optional<int> compress_type,
                  std::optional<int> compresslevel) {
        require_writer();
        if (py::isinstance<py::str>(data)) {
            std::string s = data.cast<std::string>();
            writer_->write_str(name, s, opts(compress_type, compresslevel));
        } else {
            py::buffer_info info = data.cast<py::buffer>().request();
            writer_->write_bytes(name, static_cast<const uint8_t*>(info.ptr),
                                 static_cast<size_t>(info.size * info.itemsize),
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

    py::bytes read(const std::string& name) {
        require_reader();
        std::vector<uint8_t> data;
        {
            py::gil_scoped_release release;
            data = reader_->read(name);
        }
        return py::bytes(reinterpret_cast<const char*>(data.data()), data.size());
    }

    std::vector<std::string> namelist() {
        require_reader();
        return reader_->namelist();
    }

    py::list infolist() {
        require_reader();
        py::list out;
        for (const auto& e : reader_->entries()) out.append(make_info(e));
        return out;
    }

    py::object getinfo(const std::string& name) {
        require_reader();
        const EntryInfo* e = reader_->info(name);
        if (!e) throw py::key_error("no such member: " + name);
        return make_info(*e);
    }

    py::object testzip() {
        require_reader();
        std::string bad = reader_->testzip();
        if (bad.empty()) return py::none();
        return py::str(bad);
    }

    std::string extract(const std::string& name, const std::string& path) {
        require_reader();
        return reader_->extract(name, path);
    }

    void extractall(const std::string& path) {
        require_reader();
        reader_->extractall(path);
    }

    py::object comment() {
        if (reader_) return py::bytes(reader_->comment());
        return py::bytes();
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

    static py::object make_info(const EntryInfo& e) {
        py::dict d;
        d["filename"] = e.name;
        d["file_size"] = e.uncompressed_size;
        d["compress_size"] = e.compressed_size;
        d["CRC"] = e.crc32;
        d["compress_type"] = static_cast<int>(e.method);
        d["date_time"] = date_time(e.mtime);
        d["_is_dir"] = e.is_dir;
        d["comment"] = py::bytes(e.comment);
        py::object types = py::module_::import("types");
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


PYBIND11_MODULE(pigzpp, m) {
    m.doc() = "pigzpp: Fast gzip/zlib compression and PNG helpers (C++23 library with zlib-ng/ISA-L)";
    py::class_<GzFile>(m, "open",
        "Open a gzip file for reading or writing. Use as context manager.\n"
        "\n"
        "Example:\n"
        "    with pigzpp.open('file.gz', 'wt') as f:\n"
        "        f.write('hello world')\n"
        "    with pigzpp.open('file.gz', 'rt') as f:\n"
        "        data = f.read()")
        .def(py::init<std::string, std::string, int, int>(),
             py::arg("filename"), py::arg("mode") = "rt",
             py::arg("level") = 6, py::arg("threads") = 0)
        .def("__enter__", &GzFile::enter, py::return_value_policy::reference)
        .def("__exit__", [](GzFile& self, py::object, py::object, py::object) {
            self.exit();
        })
        .def("close", &GzFile::exit, "Close the file and flush")
        .def("read", &GzFile::read, "Read all decompressed data as a string")
        .def("write", &GzFile::write, "Write string data to be compressed",
             py::arg("data"))
        .def("__iter__", &GzFile::iter, py::return_value_policy::reference)
        .def("__next__", &GzFile::next);

    // Register raw CPython C compress/decompress (zero-copy, GIL-released)
    PyObject *mod = m.ptr();
    for (PyMethodDef *meth = pigzpp_c_methods; meth->ml_name; meth++) {
        PyObject *func = PyCFunction_NewEx(meth, mod, NULL);
        if (!func) throw py::error_already_set();
        if (PyModule_AddObject(mod, meth->ml_name, func) < 0) {
            Py_DECREF(func);
            throw py::error_already_set();
        }
    }

    auto png_module = m.def_submodule("png", "Fast PNG encode/decode helpers");
    png_module.def(
        "compress",
        &png_compress,
        py::arg("data"),
        py::arg("width") = std::nullopt,
        py::arg("height") = std::nullopt,
        py::arg("channels") = std::nullopt,
        py::arg("level") = std::nullopt,
        py::arg("strategy") = std::nullopt,
        py::arg("filter") = std::nullopt,
        py::arg("preset") = "fast",
        py::arg("idat_chunk_size") = std::nullopt,
        "Compress grayscale/grayscale+alpha/RGB/RGBA uint8 image data to PNG bytes."
    );
    png_module.def(
        "decompress",
        &png_decompress,
        py::arg("data"),
        py::arg("result") = "bytes",
        "Decompress a supported PNG and return either result='bytes' as (pixels, (width, height, channels)) or result='numpy' as a NumPy uint8 array."
    );
    png_module.def(
        "decompress_array",
        &png_decompress_array,
        py::arg("data"),
        "Decompress a supported PNG and return a NumPy uint8 array with shape HxW or HxWxC."
    );
    png_module.def(
        "save",
        &png_save,
        py::arg("path"),
        py::arg("data"),
        py::arg("width") = std::nullopt,
        py::arg("height") = std::nullopt,
        py::arg("channels") = std::nullopt,
        py::arg("level") = std::nullopt,
        py::arg("strategy") = std::nullopt,
        py::arg("filter") = std::nullopt,
        py::arg("preset") = "fast",
        py::arg("idat_chunk_size") = std::nullopt,
        "Save grayscale/grayscale+alpha/RGB/RGBA uint8 image data to a PNG file."
    );
    png_module.def(
        "load",
        &png_load,
        py::arg("path"),
        py::arg("result") = "numpy",
        "Load a supported PNG file and return result='numpy' as a NumPy uint8 array, or result='bytes' as (pixels, (width, height, channels))."
    );
    png_module.def(
        "load_array",
        &png_load_array,
        py::arg("path"),
        "Load a supported PNG file and return a NumPy uint8 array with shape HxW or HxWxC."
    );

    // ZIP archive API (mirrors a subset of Python's zipfile module).
    m.attr("ZIP_STORED") = static_cast<int>(pigzpp::zip::Method::Store);
    m.attr("ZIP_DEFLATED") = static_cast<int>(pigzpp::zip::Method::Deflate);

    py::class_<zipapi::PyZipFile>(m, "ZipFile",
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
        .def(py::init<std::string, std::string, int, int, int, std::string>(),
             py::arg("file"), py::arg("mode") = "r",
             py::arg("compression") = static_cast<int>(pigzpp::zip::Method::Deflate),
             py::arg("compresslevel") = 6, py::arg("threads") = 0,
             py::arg("engine") = "auto")
        .def("__enter__", &zipapi::PyZipFile::enter, py::return_value_policy::reference)
        .def("__exit__", [](zipapi::PyZipFile& self, py::object, py::object, py::object) {
            self.exit_();
        })
        .def("close", &zipapi::PyZipFile::close, "Finalize and close the archive.")
        .def("namelist", &zipapi::PyZipFile::namelist, "List member names.")
        .def("infolist", &zipapi::PyZipFile::infolist, "List member info objects.")
        .def("getinfo", &zipapi::PyZipFile::getinfo, py::arg("name"),
             "Return the info object for a member.")
        .def("read", &zipapi::PyZipFile::read, py::arg("name"),
             "Read and decompress a member, returning bytes.")
        .def("writestr", &zipapi::PyZipFile::writestr,
             py::arg("zinfo_or_arcname"), py::arg("data"),
             py::arg("compress_type") = std::nullopt,
             py::arg("compresslevel") = std::nullopt,
             "Write a str/bytes payload as a member.")
        .def("write", &zipapi::PyZipFile::write, py::arg("filename"),
             py::arg("arcname") = std::nullopt,
             py::arg("compress_type") = std::nullopt,
             py::arg("compresslevel") = std::nullopt,
             "Add a file from disk to the archive.")
        .def("mkdir", &zipapi::PyZipFile::mkdir, py::arg("name"),
             "Add a directory entry.")
        .def("testzip", &zipapi::PyZipFile::testzip,
             "Return the name of the first corrupt member, or None if all are OK.")
        .def("extract", &zipapi::PyZipFile::extract, py::arg("member"),
             py::arg("path") = ".", "Extract a member to path; returns the file path.")
        .def("extractall", &zipapi::PyZipFile::extractall, py::arg("path") = ".",
             "Extract all members to path.")
        .def("setcomment", &zipapi::PyZipFile::set_comment, py::arg("comment"),
             "Set the archive-level comment (write mode).")
        .def_property_readonly("comment", &zipapi::PyZipFile::comment,
             "The archive-level comment (bytes).");
}
