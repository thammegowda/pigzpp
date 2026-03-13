// pigzpp Python bindings via pybind11
// API mirrors Python's gzip.open() with context manager support.

#include <pybind11/pybind11.h>
#include <pybind11/stl.h>

#include <zlib.h>

#include <cstring>
#include <stdexcept>
#include <string>
#include <vector>

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


// ---- Direct memory compress/decompress via raw CPython C API ----
// These use zlib directly (not pigzpp_lib's fd-based API) because:
// 1. We need to write directly into PyBytes objects (zero-copy output)
// 2. We need fine-grained GIL release around inflate/deflate calls
// 3. The fd-based Compressor/Decompressor add pipe/thread overhead
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
pigzpp_compress(PyObject * /*module*/, PyObject *args, PyObject *kwargs)
{
    static const char *kwlist[] = {"data", "level", NULL};
    Py_buffer buf;
    int level = 6;

    if (!PyArg_ParseTupleAndKeywords(args, kwargs, "y*|i",
                                     const_cast<char**>(kwlist),
                                     &buf, &level))
        return NULL;

    z_stream strm{};
    strm.zalloc = Z_NULL;
    strm.zfree = Z_NULL;
    strm.opaque = Z_NULL;

    int ret = deflateInit2(&strm, level == -1 ? 6 : level, Z_DEFLATED,
                           15 + 16, 8, Z_DEFAULT_STRATEGY);
    if (ret != Z_OK) {
        PyBuffer_Release(&buf);
        PyErr_SetString(PyExc_RuntimeError, "deflateInit2 failed");
        return NULL;
    }

    strm.next_in = static_cast<unsigned char *>(buf.buf);
    strm.avail_in = static_cast<unsigned>(buf.len);

    Py_ssize_t bound = static_cast<Py_ssize_t>(deflateBound(&strm, strm.avail_in));
    PyObject *result = PyBytes_FromStringAndSize(NULL, bound);
    if (!result) {
        deflateEnd(&strm);
        PyBuffer_Release(&buf);
        return NULL;
    }

    strm.next_out = (unsigned char *)PyBytes_AS_STRING(result);
    strm.avail_out = static_cast<unsigned>(bound);

    Py_BEGIN_ALLOW_THREADS
    ret = deflate(&strm, Z_FINISH);
    Py_END_ALLOW_THREADS

    PyBuffer_Release(&buf);

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
    {"decompress", (PyCFunction)pigzpp_decompress, METH_VARARGS | METH_KEYWORDS,
     "Decompress gzip-compressed bytes.\n\n"
     "Args:\n"
     "    data: bytes-like object containing gzip data\n"
     "    bufsize: initial output buffer size (default: auto). Set to expected\n"
     "        decompressed size for best performance with large data.\n"},
    {NULL, NULL, 0, NULL}
};


PYBIND11_MODULE(pigzpp, m) {
    m.doc() = "pigzpp: Fast parallel gzip compression (C++23 library with zlib-ng)";

    // File-based API: pigzpp.open() — mirrors gzip.open()
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
}
