// Decompression of gzip, zlib, and zip streams.
// Thread-safe: all state is in the Decompressor object, no globals.

#pragma once

#include "config.h"

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace pigzpp {

class Decompressor {
public:
    explicit Decompressor(const Config& cfg);
    ~Decompressor();

    Decompressor(const Decompressor&) = delete;
    Decompressor& operator=(const Decompressor&) = delete;

    // Decompress from input fd to output fd.
    // If mode is Test, output is discarded (integrity check only).
    void decompress(int in_fd, int out_fd);

    // List contents of compressed file to stdout.
    void list(int in_fd);

    // Decompress an in-memory buffer, returning the decompressed bytes.
    // Convenience wrapper around decompress() using in-memory temp fds;
    // suitable for language bindings (e.g. WebAssembly) that pass byte arrays.
    std::vector<uint8_t> decompress_buffer(const uint8_t* data, size_t size);

private:
    Config cfg_;

    // The actual inflate/check/write loop.
    void infchk(int in_fd, int out_fd);

    // Direct in-memory inflate for gzip/zlib streams; avoids the temp-fd
    // round-trip used by decompress_buffer's fallback.
    std::vector<uint8_t> direct_decompress(const uint8_t* data, size_t size);
};

} // namespace pigzpp
