// C ABI wrapper around pigzpp's buffer compression API.
// Enables in-process bindings from C, Go (cgo), Rust, etc. — no fork/pipe.

#pragma once

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// Result buffer. On success, `data` points to a heap allocation of `size`
// bytes that the caller must release with pigzpp_free(). On failure, `data`
// is NULL and `error` holds a static, human-readable message.
typedef struct {
    uint8_t* data;
    size_t size;
    const char* error; // NULL on success; static string on failure.
} pigzpp_buffer;

// DEFLATE backend engine selection.
typedef enum {
    PIGZPP_ENGINE_AUTO = 0, // best available (ISA-L if built in, else zlib-ng)
    PIGZPP_ENGINE_ZLIB = 1, // force zlib-ng (higher ratio, slower)
    PIGZPP_ENGINE_ISAL = 2, // force ISA-L (faster, lower ratio)
} pigzpp_engine;

// Output format for pigzpp_compress_file.
typedef enum {
    PIGZPP_FORMAT_GZIP = 0,
    PIGZPP_FORMAT_ZLIB = 1,
    PIGZPP_FORMAT_ZIP = 2, // ZIP archive containing the input file
    PIGZPP_FORMAT_RAW = 3, // raw DEFLATE stream
} pigzpp_format;

// Compress `data` to gzip. `level` is 1-9 (or -1 for default); `threads` is
// the number of compression threads (<= 0 means use all available cores);
// `engine` selects the DEFLATE backend (see pigzpp_engine).
pigzpp_buffer pigzpp_gzip_compress(const uint8_t* data, size_t size,
                                   int level, int threads, int engine);

// Decompress a gzip/zlib stream. In-memory single-stream inflate is sequential;
// `threads` is retained for API compatibility and does not create workers on
// this direct buffer path.
pigzpp_buffer pigzpp_gzip_decompress(const uint8_t* data, size_t size,
                                     int threads);

// Progress callback for pigzpp_compress_file. `total_size` is the input
// file size at the start of compression; `read_size` and `write_size` are
// cumulative bytes read and written; `elapsed_ms` is elapsed milliseconds.
// Calls are serialized: periodic calls (approximately every 200 ms) run on a
// helper thread, and the final call runs on the calling thread, including on
// compression failure, provided the file offsets can be read. The callback
// must not throw an exception.
typedef void (*pigzpp_compress_file_progress_callback)(uint64_t total_size,
                                                       uint64_t read_size,
                                                       uint64_t write_size,
                                                       uint64_t elapsed_ms);

// Compress `uncompressed_filename` to `compressed_filename` using `format`
// (see pigzpp_format). ZIP output contains one entry named after the input file.
// `overwrite_existing` (non-zero) allows replacing an existing output file;
// otherwise it is an error if it exists. `level` is 1-9 (or -1 for default);
// `threads` of 0 (or less) uses the default. `progress_callback` may be
// NULL. Returns NULL on success, or a static error message on failure (any
// partial output file is removed).
const char* pigzpp_compress_file(const char* uncompressed_filename,
                                 const char* compressed_filename,
                                 int overwrite_existing,
                                 int level,
                                 int threads,
                                 int format,
                                 pigzpp_compress_file_progress_callback progress_callback);

// Release a buffer returned by the functions above.
void pigzpp_free(pigzpp_buffer buf);

// ---- PNG ----

// Encode raw row-major pixels (`channels` bytes per pixel) to PNG file bytes.
// `level` is 1-9; `strategy` and `filter` are option names (e.g. "rle"/"up");
// pass NULL for their defaults. Returns an owned buffer (release with
// pigzpp_free); on failure `data` is NULL and `error` is set.
pigzpp_buffer pigzpp_png_encode(const uint8_t* pixels, size_t pixel_size,
                                uint32_t width, uint32_t height, uint8_t channels,
                                int level, const char* strategy, const char* filter);

// A decoded image. On success `pixels` is a heap buffer of `pixel_size` bytes
// (release with pigzpp_image_free); on failure `pixels` is NULL and `error` set.
typedef struct {
    uint8_t* pixels;
    size_t pixel_size;
    uint32_t width;
    uint32_t height;
    uint8_t channels;
    const char* error; // NULL on success; static string on failure.
} pigzpp_image;

// Decode a PNG into raw pixels.
pigzpp_image pigzpp_png_decode(const uint8_t* data, size_t size);

// Release an image returned by pigzpp_png_decode().
void pigzpp_image_free(pigzpp_image img);

#ifdef __cplusplus
} // extern "C"

#endif
