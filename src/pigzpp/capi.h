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

// Compress `data` to gzip. `level` is 1-9 (or -1 for default); `threads` is
// the number of compression threads (<= 0 means use all available cores).
pigzpp_buffer pigzpp_gzip_compress(const uint8_t* data, size_t size,
                                   int level, int threads);

// Decompress a gzip/zlib stream. `threads` controls decompression workers
// (<= 0 means use all available cores).
pigzpp_buffer pigzpp_gzip_decompress(const uint8_t* data, size_t size,
                                     int threads);

// Release a buffer returned by the functions above.
void pigzpp_free(pigzpp_buffer buf);

#ifdef __cplusplus
} // extern "C"
#endif
