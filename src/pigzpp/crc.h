// CRC-32 and Adler-32 check value computation and combination.
// Delegates to zlib-ng's hardware-accelerated implementations.

#pragma once

#include "config.h"

#include <cstddef>
#include <cstdint>
#include <zlib.h>

namespace pigzpp {

// Compute check value (CRC-32 or Adler-32) depending on format.
inline unsigned long check(Format form, unsigned long c,
                          const unsigned char* buf, size_t len) {
    return form == Format::Zlib ? ::adler32(c, buf, static_cast<unsigned>(len))
                                : ::crc32(c, buf, static_cast<unsigned>(len));
}

// Combine two check values depending on format.
// When shift_op is non-zero AND len2 matches the precomputed block size,
// uses the fast crc32_combine_op path. Otherwise falls back to crc32_combine.
inline unsigned long check_combine(Format form, unsigned long c1, unsigned long c2,
                                   size_t len2, unsigned long shift_op = 0,
                                   size_t block_size = 0) {
    if (form == Format::Zlib)
        return ::adler32_combine(c1, c2, static_cast<z_off_t>(len2));
    if (shift_op && len2 == block_size)
        return ::crc32_combine_op(c1, c2, shift_op);
    return ::crc32_combine(c1, c2, static_cast<z_off_t>(len2));
}

// Pre-compute the CRC combine operator for a given block size.
// Used for the fast path when all blocks are the same size.
inline unsigned long crc_combine_gen(size_t block_size) {
    return ::crc32_combine_gen(static_cast<z_off_t>(block_size));
}

// Legacy wrapper for format.cpp header CRC (single-byte or small buffer).
inline unsigned long crc32z(unsigned long crc, const unsigned char* buf, size_t len) {
    return ::crc32(crc, buf, static_cast<unsigned>(len));
}

} // namespace pigzpp
