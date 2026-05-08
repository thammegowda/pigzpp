// Gzip/zlib/zip header and trailer encoding/decoding.

#include "format.h"
#include "crc.h"
#include "io.h"

#include <cstring>
#include <ctime>
#include <stdexcept>
#include <algorithm>

#include <zlib.h>

namespace pigzpp {

// Convert Unix time to MS-DOS date and time format.
uint32_t time2dos(int64_t t) {
    if (t == 0) return 0;
    time_t tt = static_cast<time_t>(t);
    struct tm tm{};
    localtime_r(&tt, &tm);
    return static_cast<uint32_t>(
        ((tm.tm_year - 80) << 25) |
        ((tm.tm_mon + 1) << 21) |
        (tm.tm_mday << 16) |
        (tm.tm_hour << 11) |
        (tm.tm_min << 5) |
        (tm.tm_sec >> 1));
}

int64_t dos2time(uint32_t dos) {
    if (dos == 0) return ::time(nullptr);
    struct tm tm{};
    tm.tm_year = static_cast<int>((dos >> 25) & 0x7f) + 80;
    tm.tm_mon  = static_cast<int>((dos >> 21) & 0xf) - 1;
    tm.tm_mday = static_cast<int>((dos >> 16) & 0x1f);
    tm.tm_hour = static_cast<int>((dos >> 11) & 0x1f);
    tm.tm_min  = static_cast<int>((dos >> 5) & 0x3f);
    tm.tm_sec  = static_cast<int>((dos << 1) & 0x3e);
    tm.tm_isdst = -1;
    return static_cast<int64_t>(mktime(&tm));
}

// Helper to write LE bytes.
static void put_le(std::vector<unsigned char>& v, uint64_t val, int bytes) {
    for (int i = 0; i < bytes; i++) {
        v.push_back(static_cast<unsigned char>(val & 0xff));
        val >>= 8;
    }
}

// Helper to write BE bytes.
static void put_be(std::vector<unsigned char>& v, uint64_t val, int bytes) {
    for (int i = bytes - 1; i >= 0; i--) {
        v.push_back(static_cast<unsigned char>((val >> (i * 8)) & 0xff));
    }
}

size_t put_header(int fd, const Config& cfg) {
    std::vector<unsigned char> hdr;

    if (cfg.form == Format::Zip) {
        const std::string& fname = cfg.name.empty() ? cfg.alias : cfg.name;

        // Local file header
        put_le(hdr, 0x04034b50, 4); // signature
        put_le(hdr, 45, 2);         // version needed
        put_le(hdr, 8, 2);          // flags: data descriptor follows
        put_le(hdr, 8, 2);          // deflate
        put_le(hdr, time2dos(cfg.mtime), 4);
        put_le(hdr, 0, 4);          // crc placeholder
        put_le(hdr, 0xffffffff, 4); // compressed length placeholder
        put_le(hdr, 0xffffffff, 4); // uncompressed length placeholder
        put_le(hdr, fname.size(), 2);
        put_le(hdr, 29, 2);         // extra field length

        // file name
        hdr.insert(hdr.end(), fname.begin(), fname.end());

        // Zip64 extended information (16 bytes)
        put_le(hdr, 0x0001, 2);
        put_le(hdr, 16, 2);
        put_le(hdr, 0, 8);  // uncompressed length placeholder
        put_le(hdr, 0, 8);  // compressed length placeholder

        // Extended timestamp (9 bytes)
        put_le(hdr, 0x5455, 2);
        put_le(hdr, 5, 2);
        put_le(hdr, 1, 1);
        put_le(hdr, static_cast<uint32_t>(cfg.mtime), 4);
    }
    else if (cfg.form == Format::Zlib) {
        unsigned head = (0x78 << 8) +
                       (cfg.level >= 9 ? 3 << 6 :
                        cfg.level == 1 ? 0 << 6 :
                        cfg.level >= 6 || cfg.level == -1 ? 1 << 6 : 2 << 6);
        head += 31 - (head % 31);
        put_be(hdr, head, 2);
    }
    else { // Gzip
        hdr.push_back(31);  // magic
        hdr.push_back(139);
        hdr.push_back(8);   // deflate
        unsigned char flags = 0;
        if (!cfg.name.empty()) flags |= 8;
        if (!cfg.comment.empty()) flags |= 16;
        hdr.push_back(flags);
        put_le(hdr, static_cast<uint32_t>(cfg.mtime), 4);
        hdr.push_back(cfg.level >= 9 ? 2 : (cfg.level == 1 ? 4 : 0)); // xfl
        hdr.push_back(3); // OS=Unix

        if (!cfg.name.empty()) {
            hdr.insert(hdr.end(), cfg.name.begin(), cfg.name.end());
            hdr.push_back(0);
        }
        if (!cfg.comment.empty()) {
            hdr.insert(hdr.end(), cfg.comment.begin(), cfg.comment.end());
            hdr.push_back(0);
        }
    }

    writen(fd, hdr.data(), hdr.size());
    return hdr.size();
}

static constexpr uint64_t LOW32 = 0xffffffff;

void put_trailer(int fd, const Config& cfg,
                 uint64_t ulen, uint64_t clen,
                 unsigned long check, size_t head) {
    std::vector<unsigned char> tlr;

    if (cfg.form == Format::Zip) {
        // Zip64 data descriptor
        put_le(tlr, 0x08074b50, 4);
        put_le(tlr, check, 4);
        put_le(tlr, clen, 8);
        put_le(tlr, ulen, 8);
        size_t desc_len = tlr.size();

        writen(fd, tlr.data(), tlr.size());
        tlr.clear();

        bool zip64 = ulen >= LOW32 || clen >= LOW32;
        const std::string& fname = cfg.name.empty() ? cfg.alias : cfg.name;

        // Central directory entry
        put_le(tlr, 0x02014b50, 4);
        put_le(tlr, 45, 1); // version made by
        put_le(tlr, 255, 1);
        put_le(tlr, 45, 2);  // version needed
        put_le(tlr, 8, 2);   // flags
        put_le(tlr, 8, 2);   // deflate
        put_le(tlr, time2dos(cfg.mtime), 4);
        put_le(tlr, check, 4);
        put_le(tlr, zip64 ? LOW32 : clen, 4);
        put_le(tlr, zip64 ? LOW32 : ulen, 4);
        put_le(tlr, fname.size(), 2);
        put_le(tlr, zip64 ? 29u : 9u, 2); // extra field length
        put_le(tlr, cfg.comment.empty() ? 0u : cfg.comment.size(), 2);
        put_le(tlr, 0, 2);  // disk number
        put_le(tlr, 0, 2);  // internal attributes
        put_le(tlr, 0, 4);  // external attributes
        put_le(tlr, 0, 4);  // offset of local header

        // file name
        tlr.insert(tlr.end(), fname.begin(), fname.end());

        // Zip64 extra field (if needed)
        if (zip64) {
            put_le(tlr, 0x0001, 2);
            put_le(tlr, 16, 2);
            put_le(tlr, ulen, 8);
            put_le(tlr, clen, 8);
        }

        // Extended timestamp
        put_le(tlr, 0x5455, 2);
        put_le(tlr, 5, 2);
        put_le(tlr, 1, 1);
        put_le(tlr, static_cast<uint32_t>(cfg.mtime), 4);

        // Comment
        if (!cfg.comment.empty())
            tlr.insert(tlr.end(), cfg.comment.begin(), cfg.comment.end());

        size_t cent_len = tlr.size();
        writen(fd, tlr.data(), tlr.size());
        tlr.clear();

        // Zip64 end of central directory (if offset doesn't fit in 32 bits)
        bool zip64_eocd = (head + clen + desc_len) >= LOW32;
        if (zip64_eocd) {
            put_le(tlr, 0x06064b50, 4);
            put_le(tlr, 44, 8);
            put_le(tlr, 45, 2);
            put_le(tlr, 45, 2);
            put_le(tlr, 0, 4);
            put_le(tlr, 0, 4);
            put_le(tlr, 1, 8);
            put_le(tlr, 1, 8);
            put_le(tlr, cent_len, 8);
            put_le(tlr, head + clen + desc_len, 8);

            put_le(tlr, 0x07064b50, 4);
            put_le(tlr, 0, 4);
            put_le(tlr, head + clen + desc_len + cent_len, 8);
            put_le(tlr, 1, 4);
        }

        // End of central directory
        put_le(tlr, 0x06054b50, 4);
        put_le(tlr, 0, 2);
        put_le(tlr, 0, 2);
        put_le(tlr, zip64_eocd ? 0xffff : 1, 2);
        put_le(tlr, zip64_eocd ? 0xffff : 1, 2);
        put_le(tlr, zip64_eocd ? LOW32 : cent_len, 4);
        put_le(tlr, zip64_eocd ? LOW32 : (head + clen + desc_len), 4);
        put_le(tlr, 0, 2);

        writen(fd, tlr.data(), tlr.size());
    }
    else if (cfg.form == Format::Zlib) {
        // Big-endian Adler-32
        put_be(tlr, check, 4);
        writen(fd, tlr.data(), tlr.size());
    }
    else { // Gzip
        put_le(tlr, check, 4);
        put_le(tlr, ulen & LOW32, 4);
        writen(fd, tlr.data(), tlr.size());
    }
}

// -- InputReader implementation --

InputReader::InputReader(int fd, int procs) : fd_(fd), procs_(procs) {}

InputReader::~InputReader() {
    // Shut down read-ahead thread if running
    if (in_which_ != -1) {
        load_wait();
        load_state_.store(2, std::memory_order_release); // exit signal
        if (load_thread_.joinable()) load_thread_.join();
        in_which_ = -1;
    }
}

void InputReader::load_read_worker() {
    for (;;) {
        // Wait for request
        while (load_state_.load(std::memory_order_acquire) == 0)
            std::this_thread::yield();
        if (load_state_.load(std::memory_order_acquire) > 1)
            break; // exit
        in_len_ = readn(fd_, in_which_ ? in_buf_.get() : in_buf2_.get(), BUF);
        load_state_.store(0, std::memory_order_release);
        if (in_len_ < BUF) break; // EOF, thread exits
    }
}

void InputReader::load_wait() {
    if (in_which_ == -1) return;
    while (load_state_.load(std::memory_order_acquire) != 0)
        std::this_thread::yield();
}

void InputReader::reset() {
    in_left_ = 0;
    in_eof_ = false;
    in_short_ = false;
    in_tot_ = 0;
    in_next_ = in_buf_.get();
}

void InputReader::close() {
    // Shut down read-ahead thread
    if (in_which_ != -1) {
        load_wait();
        load_state_.store(2, std::memory_order_release);
        if (load_thread_.joinable()) load_thread_.join();
        in_which_ = -1;
    }
    in_left_ = 0;
    in_short_ = true;
    in_eof_ = true;
    if (fd_ > 0) {
        ::close(fd_);
        fd_ = -1;
    }
}

size_t InputReader::load() {
    if (in_short_) {
        in_eof_ = true;
        in_left_ = 0;
        return 0;
    }

    if (procs_ > 1) {
        // Double-buffered read-ahead path
        if (in_which_ == -1) {
            // First time: start read thread, request first read
            in_which_ = 1;
            load_state_.store(1, std::memory_order_release);
            load_thread_ = std::jthread([this] { load_read_worker(); });
        }

        // Wait for the previously requested read to complete
        load_wait();

        // Use the buffer that was just filled
        in_next_ = in_which_ ? in_buf_.get() : in_buf2_.get();
        in_left_ = in_len_;

        if (in_len_ == BUF) {
            // Not at EOF: alternate buffer and request next read
            in_which_ = 1 - in_which_;
            load_state_.store(1, std::memory_order_release);
        } else {
            // EOF: join the read thread
            if (load_thread_.joinable()) load_thread_.join();
            in_which_ = -1;
        }
    } else {
        // Single-threaded: simple read
        in_left_ = readn(fd_, in_next_ = in_buf_.get(), BUF);
    }

    if (in_left_ < BUF) {
        in_short_ = true;
        if (in_left_ == 0) in_eof_ = true;
    }
    in_tot_ += in_left_;
    return in_left_;
}

int InputReader::get() {
    if (in_left_ == 0 && (in_eof_ || load() == 0)) return 0;
    in_left_--;
    return *in_next_++;
}

unsigned InputReader::get2() {
    unsigned v = static_cast<unsigned>(get());
    v += static_cast<unsigned>(get()) << 8;
    return v;
}

unsigned long InputReader::get4() {
    unsigned long v = get2();
    v += static_cast<unsigned long>(get2()) << 16;
    return v;
}

bool InputReader::skip(size_t n) {
    while (n > in_left_) {
        n -= in_left_;
        if (load() == 0) return false;
    }
    in_left_ -= n;
    in_next_ += n;
    return true;
}

int InputReader::read_extra(unsigned len, bool save, HeaderInfo& info) {
    while (len >= 4) {
        unsigned id = get2();
        unsigned size = get2();
        if (in_eof_) return -1;
        len -= 4;
        if (size > len) break;
        len -= size;

        if (id == 0x0001) {
            info.zip64 = true;
            if (info.zip_ulen == LOW32 && size >= 8) {
                info.zip_ulen = get4();
                skip(4);
                size -= 8;
            }
            if (info.zip_clen == LOW32 && size >= 8) {
                info.zip_clen = get4();
                skip(4);
                size -= 8;
            }
        }
        if (save) {
            if ((id == 0x000d || id == 0x5855) && size >= 8) {
                skip(4);
                info.stamp = static_cast<int64_t>(get4());
                size -= 8;
            }
            if (id == 0x5455 && size >= 5) {
                size--;
                if (get() & 1) {
                    info.stamp = static_cast<int64_t>(get4());
                    size -= 4;
                }
            }
        }
        skip(size);
    }
    skip(len);
    return 0;
}

HeaderInfo InputReader::get_header(bool save) {
    HeaderInfo info;

    if (save) {
        info.stamp = 0;
        info.hname.clear();
        info.hcomm.clear();
    }

    // Read first two magic bytes
    int magic1 = get();
    if (in_eof_) {
        info.method = -1;
        return info;
    }
    unsigned magic = static_cast<unsigned>(magic1) << 8;
    magic += static_cast<unsigned>(get());
    if (in_eof_) {
        info.method = -2;
        return info;
    }

    // Check for zlib
    if (magic % 31 == 0 && (magic & 0x8f20) == 0x0800) {
        info.form = Format::Zlib;
        info.method = 8;
        return info;
    }

    // Check for zip
    if (magic == 0x504b) {
        unsigned rest = get2();
        if (in_eof_) { info.method = -3; return info; }
        if (rest == 0x0201 || rest == 0x0806) { info.method = -5; return info; }
        if (rest != 0x0403) { info.method = -4; return info; }

        info.zip64 = false;
        skip(2);
        unsigned flags = get2();
        if (flags & 0xf7f0) { info.method = -4; return info; }
        unsigned method = get();
        if (get() != 0 || (flags & 1)) method = 256;
        if (save) info.stamp = dos2time(static_cast<uint32_t>(get4()));
        else skip(4);
        info.zip_crc = get4();
        info.zip_clen = get4();
        info.zip_ulen = get4();
        unsigned fname = get2();
        unsigned extra = get2();

        if (save) {
            if (in_eof_) { info.method = -3; return info; }
            info.hname.resize(fname);
            size_t got = 0;
            while (got < fname) {
                if (in_left_ == 0 && load() == 0) { info.method = -3; return info; }
                size_t take = std::min(static_cast<size_t>(fname - got), in_left_);
                std::memcpy(info.hname.data() + got, in_next_, take);
                in_left_ -= take;
                in_next_ += take;
                got += take;
            }
        } else {
            skip(fname);
        }
        read_extra(extra, save, info);
        info.has_data_descriptor = (flags & 8) != 0;
        info.form = info.has_data_descriptor ? Format::Zip : Format::Zip;
        info.method = in_eof_ ? -3 : static_cast<int>(method);
        return info;
    }

    // Check for gzip magic
    if (magic != 0x1f8b) {
        in_left_++;
        in_next_--;
        info.method = -2;
        return info;
    }

    // Gzip
    unsigned long crc = 0xf6e946c9; // CRC of 0x1f 0x8b

    auto getc_crc = [&]() -> int {
        int c = get();
        unsigned char b = static_cast<unsigned char>(c);
        crc = crc32z(crc, &b, 1);
        return c;
    };
    auto get2c = [&]() -> unsigned {
        unsigned v = static_cast<unsigned>(getc_crc());
        v += static_cast<unsigned>(getc_crc()) << 8;
        return v;
    };
    auto get4c = [&]() -> unsigned long {
        unsigned long v = get2c();
        v += static_cast<unsigned long>(get2c()) << 16;
        return v;
    };
    auto skipc = [&](size_t n) {
        while (n > 0) {
            if (in_left_ == 0 && load() == 0) return;
            size_t take = std::min(n, in_left_);
            crc = crc32z(crc, in_next_, take);
            in_left_ -= take;
            in_next_ += take;
            n -= take;
        }
    };

    unsigned method = static_cast<unsigned>(getc_crc());
    unsigned flags = static_cast<unsigned>(getc_crc());
    if (flags & 0xe0) { info.method = -4; return info; }

    if (save) {
        unsigned long ts = get4c();
        info.stamp = static_cast<int64_t>(ts & 0x7fffffffUL) -
                     static_cast<int64_t>(ts & 0x80000000UL);
    } else {
        skipc(4);
    }
    skipc(2); // extra flags + OS

    // Extra field
    if (flags & 4) {
        unsigned elen = get2c();
        skipc(elen);
    }

    // File name
    if (flags & 8) {
        if (save) {
            info.hname.clear();
            int c;
            while ((c = getc_crc()) != 0)
                info.hname.push_back(static_cast<char>(c));
        } else {
            while (getc_crc() != 0) {}
        }
    }

    // Comment
    if (flags & 16) {
        if (save) {
            info.hcomm.clear();
            int c;
            while ((c = getc_crc()) != 0)
                info.hcomm.push_back(static_cast<char>(c));
        } else {
            while (getc_crc() != 0) {}
        }
    }

    // Header CRC
    if ((flags & 2) && get2() != (crc & 0xffff)) {
        info.method = -6;
        return info;
    }

    info.form = Format::Gzip;
    info.method = in_eof_ ? -3 : static_cast<int>(method);
    return info;
}

} // namespace pigzpp
