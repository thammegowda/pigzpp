// Decompression of gzip, zlib, and zip streams.
// Thread-safe: all state is in the Decompressor object, no globals.

#pragma once

#include "config.h"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace pigzpp {

// Inflate gzip/zlib into a non-overlapping destination. Returns the byte count,
// nullopt if the output doesn't fit, or throws on invalid compressed data.
std::optional<size_t> decompress_buffer_into(const uint8_t* data, size_t size,
                                             uint8_t* dst, size_t dst_cap);

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

    // Decompress an in-memory buffer into a malloc()'d buffer transferred to the
    // caller via `*out` (release with free()); returns the decompressed size.
    size_t decompress_buffer(const uint8_t* data, size_t size, uint8_t** out);

    // Decompress an in-memory buffer, returning the decompressed bytes.
    std::vector<uint8_t> decompress_buffer(const uint8_t* data, size_t size);

private:
    Config cfg_;

    // The actual inflate/check/write loop.
    void infchk(int in_fd, int out_fd);
};

} // namespace pigzpp
