// Parallel and single-threaded gzip compression.
// Thread-safe: all state is in the Compressor object, no globals.

#pragma once

#include "config.h"
#include "crc.h"
#include "pool.h"

#include <cstdint>

namespace pigzpp {

class Compressor {
public:
    explicit Compressor(const Config& cfg);
    ~Compressor();

    Compressor(const Compressor&) = delete;
    Compressor& operator=(const Compressor&) = delete;

    // Compress from input fd to output fd.
    void compress(int in_fd, int out_fd);

private:
    Config cfg_;
    unsigned long shift_; // Pre-computed CRC combine operator for block size.

    // Single-threaded compression path.
    void single_compress(int in_fd, int out_fd);

    // Multi-threaded compression path.
    void parallel_compress(int in_fd, int out_fd);
};

} // namespace pigzpp
