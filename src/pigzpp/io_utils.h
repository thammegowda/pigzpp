// I/O utilities: full read/write with EINTR retry.

#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <vector>

namespace pigzpp {

// Read exactly len bytes from fd, returning the number actually read
// (less than len only at EOF). Retries on EINTR.
size_t readn(int fd, unsigned char* buf, size_t len);

// Write exactly len bytes to fd. Throws on error. Returns len.
size_t writen(int fd, const unsigned char* buf, size_t len);

// Run an fd-based operation on an in-memory buffer using temporary files.
// Writes `data` to a temp input fd, invokes op(in_fd, out_fd), and returns the
// bytes written to the temp output fd. Used by buffer-API fallbacks for paths
// that still require seekable fds (zip, zopfli, rsync, decompress).
// Throws std::runtime_error on I/O failure.
std::vector<uint8_t> run_via_temp_fds(
    const uint8_t* data, size_t size,
    const std::function<void(int in_fd, int out_fd)>& op);

} // namespace pigzpp
