// Gzip/zlib/zip header and trailer encoding/decoding.

#pragma once

#include "config.h"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <thread>
#include <vector>

namespace pigzpp {

// Time conversion for zip timestamps.
uint32_t time2dos(int64_t t);
int64_t dos2time(uint32_t dos);

// Write a gzip, zlib, or zip header to fd using config settings.
// Returns the number of header bytes written.
size_t put_header(int fd, const Config& cfg);

// Build the header bytes (fd-free) for in-memory sinks. Same encoding as
// put_header for all formats.
std::vector<unsigned char> build_header(const Config& cfg);

// Build the gzip/zlib trailer bytes (fd-free). Only valid for Gzip/Zlib;
// zip trailers require offset bookkeeping and must use put_trailer.
std::vector<unsigned char> build_trailer_simple(const Config& cfg,
                                                uint64_t ulen,
                                                unsigned long check);

// Write a gzip, zlib, or zip trailer to fd.
void put_trailer(int fd, const Config& cfg,
                 uint64_t ulen, uint64_t clen,
                 unsigned long check, size_t head);

// Header info returned from parsing.
struct HeaderInfo {
    int method = -1;      // Compression method (8=deflate, 257=lzw, negative=error)
    Format form = Format::Gzip;
    int64_t stamp = 0;
    std::string hname;
    std::string hcomm;
    unsigned long zip_crc = 0;
    uint64_t zip_clen = 0;
    uint64_t zip_ulen = 0;
    bool zip64 = false;
    bool has_data_descriptor = false; // zip form==3
};

// Buffered input reader for decompression.
class InputReader {
public:
    explicit InputReader(int fd, int procs = 1);
    ~InputReader();

    // Load more data. Returns bytes available.
    size_t load();

    // Get next byte (0 at EOF).
    int get();

    // Get 2-byte LE value.
    unsigned get2();

    // Get 4-byte LE value.
    unsigned long get4();

    // Skip n bytes. Returns false if premature EOF.
    bool skip(size_t n);

    // Read a gzip/zip/zlib header.
    HeaderInfo get_header(bool save);

    // Access to internal state for inflateBack.
    unsigned char* next() const { return in_next_; }
    size_t left() const { return in_left_; }
    bool eof() const { return in_eof_; }
    uint64_t total() const { return in_tot_; }

    // Adjust state after inflateBack returns unused input.
    void restore(unsigned char* next, size_t avail) {
        in_left_ = avail;
        in_next_ = next;
    }

    // Consume len bytes from the buffer (advance next, reduce left).
    void consume(size_t len) {
        in_next_ += len;
        in_left_ -= len;
    }

    // Reset for new stream.
    void reset();

    // Close and release.
    void close();

    int fd() const { return fd_; }

private:
    static constexpr size_t BUF = 262144; // 256 KB — fewer read syscalls at multi-GB/s speeds
    static constexpr size_t CEN = 42;
    static constexpr size_t EXT = BUF + CEN;

    int fd_;
    int procs_ = 1;
    // Heap-allocated to avoid ~512 KB on the stack with 256 KB BUF.
    std::unique_ptr<unsigned char[]> in_buf_{new unsigned char[EXT]{}};
    std::unique_ptr<unsigned char[]> in_buf2_{new unsigned char[EXT]{}}; // double-buffer for read-ahead
    unsigned char* in_next_ = in_buf_.get();
    size_t in_left_ = 0;
    bool in_eof_ = false;
    bool in_short_ = false;
    uint64_t in_tot_ = 0;

    // Read-ahead thread state (procs > 1)
    int in_which_ = -1; // -1=not started, 0=in_buf2, 1=in_buf
    std::atomic<int> load_state_{0}; // 0=done, 1=read, 2=exit
    size_t in_len_ = 0;
    std::jthread load_thread_;

    void load_read_worker();
    void load_wait();

    int read_extra(unsigned len, bool save, HeaderInfo& info);
};

} // namespace pigzpp
