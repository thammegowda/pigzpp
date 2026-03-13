// I/O utilities: full read/write with EINTR retry.

#pragma once

#include <cstddef>

namespace pigzpp {

// Read exactly len bytes from fd, returning the number actually read
// (less than len only at EOF). Retries on EINTR.
size_t readn(int fd, unsigned char* buf, size_t len);

// Write exactly len bytes to fd. Throws on error. Returns len.
size_t writen(int fd, const unsigned char* buf, size_t len);

} // namespace pigzpp
