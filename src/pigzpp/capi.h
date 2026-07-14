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

// Compress `data` to gzip. `level` is 1-9 (or -1 for default); `threads` is
// the number of compression threads (<= 0 means use all available cores);
// `engine` selects the DEFLATE backend (see pigzpp_engine).
pigzpp_buffer pigzpp_gzip_compress(const uint8_t* data, size_t size,
                                   int level, int threads, int engine);

// Decompress a gzip/zlib stream. `threads` controls decompression workers
// (<= 0 means use all available cores).
pigzpp_buffer pigzpp_gzip_decompress(const uint8_t* data, size_t size,
                                     int threads);

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

// ---- ZIP archives ----

// Compression method for a ZIP member.
typedef enum {
    PIGZPP_ZIP_STORED = 0,
    PIGZPP_ZIP_DEFLATED = 8,
} pigzpp_zip_method;

// Opaque archive handles.
typedef struct pigzpp_zip_writer pigzpp_zip_writer;
typedef struct pigzpp_zip_reader pigzpp_zip_reader;

// -- Writer (builds an archive in memory) --

// Create an in-memory ZIP writer. Returns NULL on allocation failure.
pigzpp_zip_writer* pigzpp_zip_writer_new(void);

// Add a member. `method` is a pigzpp_zip_method; `level` is 0-9; `threads` <= 0
// uses all cores; `engine` is a pigzpp_engine. Returns NULL on success or a
// static error string on failure.
const char* pigzpp_zip_writer_add(pigzpp_zip_writer* w, const char* name,
                                  const uint8_t* data, size_t size,
                                  int method, int level, int threads, int engine);

// Set the archive-level comment.
void pigzpp_zip_writer_set_comment(pigzpp_zip_writer* w, const char* comment);

// Finalize: returns the complete archive bytes (release with pigzpp_free) and
// consumes the writer (do not use or free it afterwards). On failure `data` is
// NULL and `error` is set; the writer is still consumed.
pigzpp_buffer pigzpp_zip_writer_finish(pigzpp_zip_writer* w);

// Discard a writer without finalizing.
void pigzpp_zip_writer_free(pigzpp_zip_writer* w);

// -- Reader (opens an in-memory archive; the bytes are copied) --

// Open an archive. On failure returns NULL and sets *error (if non-NULL).
pigzpp_zip_reader* pigzpp_zip_reader_open(const uint8_t* data, size_t size,
                                          const char** error);

// Number of members.
size_t pigzpp_zip_reader_count(const pigzpp_zip_reader* r);

// Member metadata. `name` points into reader-owned storage (valid until the
// reader is freed).
typedef struct {
    const char* name;
    uint64_t compressed_size;
    uint64_t uncompressed_size;
    uint32_t crc32;
    int method;
    int is_dir;
} pigzpp_zip_entry;

// Fill `out` with the entry at `index`. Returns 1 on success, 0 if out of range.
int pigzpp_zip_reader_entry(const pigzpp_zip_reader* r, size_t index,
                            pigzpp_zip_entry* out);

// Read + decompress a member by name / index. Returns an owned buffer (release
// with pigzpp_free); on failure `data` is NULL and `error` is set.
pigzpp_buffer pigzpp_zip_reader_read(const pigzpp_zip_reader* r, const char* name);
pigzpp_buffer pigzpp_zip_reader_read_index(const pigzpp_zip_reader* r, size_t index);

// Archive-level comment (reader-owned, valid until the reader is freed).
const char* pigzpp_zip_reader_comment(const pigzpp_zip_reader* r);

// Verify all member CRCs. Returns the name of the first corrupt member
// (reader-owned string, valid until the next testzip call or free) or NULL if
// the whole archive is intact.
const char* pigzpp_zip_reader_testzip(pigzpp_zip_reader* r);

// Release a reader.
void pigzpp_zip_reader_free(pigzpp_zip_reader* r);

#ifdef __cplusplus
} // extern "C"
#endif
