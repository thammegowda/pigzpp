// Parallel and single-threaded gzip compression.
// Thread-safe: all state is in the Compressor object, no globals.

#pragma once

#include "config.h"
#include "crc.h"
#include "pool.h"

#include <cstddef>
#include <cstdint>
#include <vector>

namespace pigzpp {

// In-memory source/sink for the buffer-compression fast path (defined in
// compress.cpp). Holds the input view and the growable malloc-backed output
// buffer that is transferred to the caller on completion.
struct MemIO;

class Compressor {
public:
    explicit Compressor(const Config& cfg);
    ~Compressor();

    Compressor(const Compressor&) = delete;
    Compressor& operator=(const Compressor&) = delete;

    // Compress from input fd to output fd.
    void compress(int in_fd, int out_fd);

    // Compress an in-memory buffer into a malloc()'d buffer transferred to the
    // caller via *out (which must be released with free()); returns the
    // compressed size. This is the single buffer-compression entry point for
    // language bindings. The parallel gzip/zlib fast path writes deflate output
    // straight into *out (zero-copy); other paths compute once then hand off.
    size_t compress_buffer(const uint8_t* data, size_t size, uint8_t** out);

private:
    Config cfg_;
    unsigned long shift_; // Pre-computed CRC combine operator for block size.

    // In-memory-aware I/O helpers. `mem` (owned by compress_buffer for one call)
    // selects the in-memory buffer path; when null the fd path is used. These
    // don't touch member state, so they're static — the in-memory context is
    // passed explicitly rather than stored on the object.
    static size_t read_fd(MemIO* mem, int in_fd, uint8_t* buf, size_t len);
    static void write_sink(MemIO* mem, int out_fd, const uint8_t* buf, size_t len);

    // True when the multi-threaded in-memory gzip/zlib fast path applies
    // (parallel, non-zopfli, non-rsync, gzip or zlib framing).
    bool inmem_parallel_ok() const;

    // Core dispatch shared by compress() and compress_buffer(): `mem` is null
    // for fd streaming and non-null for the in-memory buffer path.
    void compress(int in_fd, int out_fd, MemIO* mem);

    // Single-threaded compression path (fd-only).
    void single_compress(int in_fd, int out_fd);

    // Multi-threaded compression path. `mem` selects in-memory I/O when set.
    void parallel_compress(int in_fd, int out_fd, MemIO* mem);

    // Direct in-memory deflate for the single-threaded gzip/zlib fast path.
    // Avoids the temp-fd round-trip used by compress_buffer's fallback.
    std::vector<uint8_t> direct_compress(const uint8_t* data, size_t size);
};

} // namespace pigzpp
