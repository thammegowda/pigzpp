// Decompression of gzip, zlib, and zip streams.
// Thread-safe: all state is in the Decompressor object, no globals.

#pragma once

#include "config.h"

#include <cstdint>
#include <string>

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

private:
    Config cfg_;

    // The actual inflate/check/write loop.
    void infchk(int in_fd, int out_fd);
};

} // namespace pigzpp
