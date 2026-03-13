// Parallel and single-threaded gzip compression.
// Port of pigz.c parallel_compress(), single_compress(), compress_thread(),
// write_thread() — all state encapsulated.
//
// When compiled with PIGZPP_USE_ISAL, uses Intel ISA-L for DEFLATE (levels 0-9).
// Zopfli (level 11) always uses zlib.

#include "compress.h"
#include "format.h"
#include "io.h"

#include <algorithm>
#include <atomic>
#include <cassert>
#include <climits>
#include <condition_variable>
#include <cstring>
#include <deque>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <unistd.h>
#include <vector>

#include <zlib.h>

extern "C" {
#include "deflate.h"
}

#ifdef PIGZPP_USE_ISAL
#include <isa-l/igzip_lib.h>
#endif

namespace pigzpp {

// Largest power of 2 that fits in an unsigned int.
static constexpr unsigned MAXP2 = UINT_MAX - (UINT_MAX >> 1);

// ---- ISA-L helpers ----

#ifdef PIGZPP_USE_ISAL

// Map pigzpp compression level (0-9) to ISA-L level (0-3).
static int isal_level(int level) {
    if (level <= 0) return 0;
    if (level <= 1) return 0;
    if (level <= 5) return 1;
    if (level <= 8) return 2;
    return 3;  // level 9
}

// Get the recommended level_buf size for an ISA-L level.
static size_t isal_level_buf_size(int isal_lvl) {
    switch (isal_lvl) {
    case 0: return ISAL_DEF_LVL0_DEFAULT;
    case 1: return ISAL_DEF_LVL1_DEFAULT;
    case 2: return ISAL_DEF_LVL2_DEFAULT;
    case 3: return ISAL_DEF_LVL3_DEFAULT;
    default: return ISAL_DEF_LVL3_DEFAULT;
    }
}

// Returns true when ISA-L should handle this compression (levels 0-9, not zopfli).
static bool use_isal(int level) {
    return level <= 9;
}

// ISA-L deflate_engine: compress all avail_in into buf, growing it as needed.
static void isal_deflate_engine(struct isal_zstream* strm, Buffer& out, int flush) {
    strm->flush = static_cast<uint16_t>(flush);
    do {
        size_t room = out.size() - out.len;
        if (room == 0) {
            out.grow();
            room = out.size() - out.len;
        }
        strm->next_out = out.data() + out.len;
        strm->avail_out = room < UINT_MAX ? static_cast<unsigned>(room) : UINT_MAX;
        int ret = isal_deflate(strm);
        if (ret != COMP_OK)
            throw std::runtime_error("isal_deflate failed: " + std::to_string(ret));
        out.len = static_cast<size_t>(strm->next_out - out.data());
    } while (strm->avail_out == 0);
    assert(strm->avail_in == 0);
}

#endif // PIGZPP_USE_ISAL

// ---- helpers ----

// The deflate_engine equivalent: deflate all avail_in into buf, growing it.
static void deflate_engine(z_stream* strm, Buffer& out, int flush) {
    do {
        size_t room = out.size() - out.len;
        if (room == 0) {
            out.grow();
            room = out.size() - out.len;
        }
        strm->next_out = out.data() + out.len;
        strm->avail_out = room < UINT_MAX ? static_cast<unsigned>(room) : UINT_MAX;
        deflate(strm, flush);
        out.len = static_cast<size_t>(strm->next_out - out.data());
    } while (strm->avail_out == 0);
    assert(strm->avail_in == 0);
}

// Encode a hash hit to the block lengths list (matches pigz append_len).
static void append_len(std::vector<unsigned char>& lens, size_t len) {
    assert(len < 539000896UL);
    if (len < 64)
        lens.push_back(static_cast<unsigned char>(len + 128));
    else if (len < 32832U) {
        len -= 64;
        lens.push_back(static_cast<unsigned char>(len >> 8));
        lens.push_back(static_cast<unsigned char>(len));
    }
    else if (len < 2129984UL) {
        len -= 32832U;
        lens.push_back(static_cast<unsigned char>((len >> 16) + 192));
        lens.push_back(static_cast<unsigned char>(len >> 8));
        lens.push_back(static_cast<unsigned char>(len));
    }
    else {
        len -= 2129984UL;
        lens.push_back(static_cast<unsigned char>((len >> 24) + 224));
        lens.push_back(static_cast<unsigned char>(len >> 16));
        lens.push_back(static_cast<unsigned char>(len >> 8));
        lens.push_back(static_cast<unsigned char>(len));
    }
}

// Decode next block length from encoded lens buffer (matches pigz).
// If next is nullptr, return left (compress everything in one go).
static size_t decode_len(const unsigned char*& next, size_t left) {
    if (next == nullptr) return left;
    unsigned char b = *next++;
    size_t len;
    if (b < 128) {
        len = (static_cast<size_t>(b) << 8) + *next++ + 64;
    } else if (b == 128) {
        len = left; // end of list — use all remaining
    } else if (b < 192) {
        len = b & 0x3f;
    } else if (b < 224) {
        len = ((static_cast<size_t>(b) & 0x1f) << 16) +
              (static_cast<size_t>(*next++) << 8);
        len += *next++ + 32832U;
    } else {
        len = ((static_cast<size_t>(b) & 0x1f) << 24) +
              (static_cast<size_t>(*next++) << 16);
        len += static_cast<size_t>(*next++) << 8;
        len += *next++ + 2129984UL;
    }
    return len;
}

// ---- Job for parallel compression ----

struct Job {
    int64_t seq = 0;
    bool more = false;
    BufferPtr in;             // input data
    BufferPtr dict;           // dictionary (from previous block)
    BufferPtr out;            // compressed output
    std::vector<unsigned char> lens; // rsyncable block lengths
    unsigned long check = 0;
    std::atomic<int> check_done{0}; // 0=pending, 1=done (replaces promise/future)
};

// ---- Compressor implementation ----

Compressor::Compressor(const Config& cfg) : cfg_(cfg) {
#ifdef PIGZPP_USE_ISAL
    // When ISA-L is the engine, use larger blocks for parallel compression.
    // ISA-L compresses so fast (~3-5 GB/s per core) that 128KB blocks create
    // excessive per-block overhead (flush, dict set, reset).  1MB blocks
    // amortize this overhead and approach igzip's throughput.
    int lvl = cfg_.level == -1 ? 6 : cfg_.level;
    if (use_isal(lvl) && !cfg_.rsync && cfg_.block == DEFAULT_BLOCK_SIZE) {
        cfg_.block = 1024 * 1024; // 1 MB
    }
#endif
    shift_ = crc_combine_gen(cfg_.block);
}

Compressor::~Compressor() = default;

void Compressor::compress(int in_fd, int out_fd) {
#ifdef POSIX_FADV_SEQUENTIAL
    posix_fadvise(in_fd, 0, 0, POSIX_FADV_SEQUENTIAL);
#endif

#ifdef PIGZPP_USE_ISAL
    // With ISA-L's per-core speed, parallel overhead can exceed gains unless
    // there are enough blocks to keep all threads busy.  When the input is a
    // seekable file, cap procs so each thread gets at least 2 blocks.
    if (cfg_.procs > 1) {
        off_t fsize = lseek(in_fd, 0, SEEK_END);
        if (fsize > 0) {
            lseek(in_fd, 0, SEEK_SET);
            int useful_threads = static_cast<int>(
                static_cast<uint64_t>(fsize) / (cfg_.block * 2));
            if (useful_threads < 1) useful_threads = 1;
            if (useful_threads < cfg_.procs)
                cfg_.procs = useful_threads;
        }
    }
#endif

    if (cfg_.procs > 1)
        parallel_compress(in_fd, out_fd);
    else
        single_compress(in_fd, out_fd);
}

// ---- Single-threaded compression ----

void Compressor::single_compress(int in_fd, int out_fd) {
    // Write header
    size_t head = put_header(out_fd, cfg_);

    // Initialize deflate at the actual level to avoid reallocation
    int actual_level = cfg_.level == -1 ? 6 : cfg_.level;

#ifdef PIGZPP_USE_ISAL
    // ISA-L fast path: levels 0-9 without rsync (rsync needs deflatePending/deflatePrime).
    if (use_isal(actual_level) && !cfg_.rsync) {
        struct isal_zstream istrm;
        isal_deflate_init(&istrm);
        int ilvl = isal_level(actual_level);
        istrm.level = static_cast<uint32_t>(ilvl);
        size_t buf_sz = isal_level_buf_size(ilvl);
        std::vector<uint8_t> isal_lvl_buf;
        if (buf_sz > 0) isal_lvl_buf.resize(buf_sz);
        istrm.level_buf = isal_lvl_buf.empty() ? nullptr : isal_lvl_buf.data();
        istrm.level_buf_size = static_cast<uint32_t>(isal_lvl_buf.size());
        istrm.gzip_flag = IGZIP_DEFLATE; // raw deflate, pigzpp handles headers

        // Use large I/O buffers to minimize syscall overhead and let ISA-L
        // operate on big chunks without any per-block flush interruptions.
        // This matches igzip's single-thread behaviour: pure streaming with
        // NO_FLUSH until end_of_stream.
        static constexpr size_t IO_BUF = 1024 * 1024; // 1 MB
        std::vector<unsigned char> inbuf(IO_BUF);
        std::vector<unsigned char> outbuf(IO_BUF);
        size_t out_pos = 0;

        uint64_t ulen = 0, clen = 0;
        unsigned long check_val = check(cfg_.form, 0L, Z_NULL, 0);

        auto flush_output = [&]() {
            if (out_pos > 0) {
                clen += writen(out_fd, outbuf.data(), out_pos);
                out_pos = 0;
            }
        };

        bool first = true;
        for (;;) {
            size_t got = readn(in_fd, inbuf.data(), IO_BUF);
            if (got == 0 && first) {
                // Empty file: emit an empty final deflate block.
                istrm.next_in = inbuf.data();
                istrm.avail_in = 0;
                istrm.end_of_stream = 1;
                istrm.next_out = outbuf.data();
                istrm.avail_out = static_cast<uint32_t>(outbuf.size());
                int ret = isal_deflate(&istrm);
                if (ret != COMP_OK)
                    throw std::runtime_error("isal_deflate failed: " + std::to_string(ret));
                out_pos = outbuf.size() - istrm.avail_out;
                break;
            }
            first = false;
            ulen += got;
            bool eof = (got < IO_BUF);

            // CRC
            unsigned char* p = inbuf.data();
            size_t rem = got;
            while (rem > MAXP2) {
                check_val = check(cfg_.form, check_val, p, MAXP2);
                p += MAXP2; rem -= MAXP2;
            }
            check_val = check(cfg_.form, check_val, p, rem);

            istrm.next_in = inbuf.data();
            istrm.avail_in = static_cast<uint32_t>(got);

            // Pure streaming: NO_FLUSH until the very last chunk.
            // This avoids per-block SYNC_FLUSH/FULL_FLUSH overhead that
            // was costing ~30% throughput vs igzip.
            if (eof)
                istrm.end_of_stream = 1;
            // else: flush stays at NO_FLUSH (the default)

            do {
                size_t room = outbuf.size() - out_pos;
                if (room == 0) {
                    flush_output();
                    room = outbuf.size();
                }
                istrm.next_out = outbuf.data() + out_pos;
                istrm.avail_out = static_cast<uint32_t>(room);
                int ret = isal_deflate(&istrm);
                if (ret != COMP_OK)
                    throw std::runtime_error("isal_deflate failed: " + std::to_string(ret));
                out_pos = outbuf.size() - istrm.avail_out;
            } while (istrm.avail_out == 0);

            if (eof) break;
        }

        flush_output();
        put_trailer(out_fd, cfg_, ulen, clen, check_val, head);
        return;
    }
#endif // PIGZPP_USE_ISAL

    // Original zlib path (also used for rsync mode and zopfli)
    int actual_level_zlib = actual_level;
    z_stream strm{};
    strm.zalloc = Z_NULL;
    strm.zfree = Z_NULL;
    strm.opaque = Z_NULL;
    int ret = deflateInit2(&strm, actual_level_zlib, Z_DEFLATED, -15, 8,
                           static_cast<int>(cfg_.strategy));
    if (ret != Z_OK)
        throw std::runtime_error("deflateInit2 failed");

    unsigned out_size = cfg_.block > MAXP2 ? MAXP2 : static_cast<unsigned>(cfg_.block);
    std::vector<unsigned char> in(cfg_.block + DICT_SIZE);
    std::vector<unsigned char> next_buf(cfg_.block + DICT_SIZE);

    // Buffered output to reduce write syscalls
    std::vector<unsigned char> out(131072);
    size_t out_pos = 0;

    uint64_t ulen = 0, clen = 0;
    unsigned long check_val = check(cfg_.form, 0L, Z_NULL, 0);

    size_t got = 0;
    size_t more = readn(in_fd, next_buf.data(), cfg_.block);
    ulen = more;
    size_t start = 0;
    size_t hist = 0;
    size_t have = 0;
    unsigned hash = RSYNCHIT;

    auto flush_output = [&]() {
        if (out_pos > 0) {
            clen += writen(out_fd, out.data(), out_pos);
            out_pos = 0;
        }
    };

    // Buffered DEFLATE_WRITE: accumulates output, flushes when buffer full
    auto deflate_write = [&](int flush) {
        do {
            strm.avail_out = static_cast<unsigned>(out.size() - out_pos);
            strm.next_out = out.data() + out_pos;
            ::deflate(&strm, flush);
            out_pos = out.size() - strm.avail_out;
            if (out_pos >= out.size())
                flush_output();
        } while (strm.avail_out == 0);
        assert(strm.avail_in == 0);
    };

    do {
        // Get data to compress
        if (got == 0) {
            std::swap(in, next_buf);
            strm.next_in = in.data() + start;
            got = more;
            if (cfg_.level > 9) {
                size_t left = start + more - hist;
                if (left > DICT_SIZE) left = DICT_SIZE;
                std::memcpy(next_buf.data(), in.data() + ((start + more) - left), left);
                start = left;
                hist = 0;
            } else {
                start = 0;
            }
            more = readn(in_fd, next_buf.data() + start, cfg_.block);
            ulen += more;
        }

        // Rsyncable hash scanning
        size_t left = 0;
        if (cfg_.rsync && got) {
            unsigned char* scan = strm.next_in;
            left = got;
            do {
                if (left == 0) {
                    if (more == 0 || got == cfg_.block) break;
                    if (cfg_.level > 9) {
                        size_t l = static_cast<size_t>(strm.next_in - in.data()) - hist;
                        if (l > DICT_SIZE) l = DICT_SIZE;
                        left = l;
                    } else {
                        left = 0;
                    }
                    std::memmove(in.data(), strm.next_in - left, left + got);
                    hist = 0;
                    strm.next_in = in.data() + left;
                    scan = in.data() + left + got;
                    left = more > cfg_.block - got ? cfg_.block - got : more;
                    std::memcpy(scan, next_buf.data() + start, left);
                    got += left;
                    more -= left;
                    start += left;

                    if (more == 0) {
                        more = readn(in_fd, next_buf.data(), cfg_.block);
                        ulen += more;
                        start = 0;
                    }
                }
                left--;
                hash = ((hash << 1) ^ *scan++) & RSYNCMASK;
            } while (hash != RSYNCHIT);
            got -= left;
        }

        // Independent blocks
        bool fresh = false;
        if (!cfg_.setdict) {
            have += got;
            if (have > cfg_.block) {
                fresh = true;
                have = got;
            }
        }

        if (cfg_.level <= 9) {
            if (fresh) deflateReset(&strm);

            while (got > MAXP2) {
                strm.avail_in = MAXP2;
                check_val = check(cfg_.form, check_val, strm.next_in, strm.avail_in);
                deflate_write(Z_NO_FLUSH);
                got -= MAXP2;
            }

            strm.avail_in = static_cast<unsigned>(got);
            got = left;
            check_val = check(cfg_.form, check_val, strm.next_in, strm.avail_in);

            if (more || got) {
                int bits = 0;
                deflate_write(Z_BLOCK);
                deflatePending(&strm, Z_NULL, &bits);
                if ((bits & 1) || !cfg_.setdict)
                    deflate_write(Z_SYNC_FLUSH);
                else if (bits & 7) {
                    do {
                        deflatePrime(&strm, 10, 2);
                        deflatePending(&strm, Z_NULL, &bits);
                    } while (bits & 7);
                    deflate_write(Z_NO_FLUSH);
                }
                if (!cfg_.setdict)
                    deflate_write(Z_FULL_FLUSH);
            } else {
                deflate_write(Z_FINISH);
            }
        } else {
            // Zopfli compression
            size_t off = static_cast<size_t>(strm.next_in - in.data());
            if (fresh) hist = off;

            unsigned char* def = nullptr;
            size_t size = 0;
            unsigned char bits = 0;
            ZopfliDeflatePart(&cfg_.zopts, 2, !(more || left),
                              in.data() + hist, off - hist, (off - hist) + got,
                              &bits, &def, &size);
            bits &= 7;
            if (more || left) {
                writen(out_fd, def, size);
                if (bits == 0 || bits > 5) {
                    unsigned char z = 0;
                    writen(out_fd, &z, 1);
                }
                unsigned char sync[] = {0, 0, 0xff, 0xff};
                writen(out_fd, sync, 4);
                if (!cfg_.setdict) {
                    unsigned char full[] = {0, 0, 0, 0xff, 0xff};
                    writen(out_fd, full, 5);
                }
            } else {
                writen(out_fd, def, size);
            }
            free(def);
            while (got > MAXP2) {
                check_val = check(cfg_.form, check_val, strm.next_in, MAXP2);
                strm.next_in += MAXP2;
                got -= MAXP2;
            }
            check_val = check(cfg_.form, check_val, strm.next_in, static_cast<unsigned>(got));
            strm.next_in += got;
            got = left;
        }
    } while (more || got);

    flush_output();
    deflateEnd(&strm);

    // Write trailer
    put_trailer(out_fd, cfg_, ulen, clen, check_val, head);
}

// ---- Parallel compression ----

void Compressor::parallel_compress(int in_fd, int out_fd) {
    // Buffer pools
    BufferPool in_pool(cfg_.block, inbufs(cfg_.procs));
    BufferPool out_pool(outpool(cfg_.block), -1);
    BufferPool dict_pool(DICT_SIZE, -1);

    // Queues
    std::mutex compress_mu;
    std::condition_variable compress_cv;
    std::deque<std::shared_ptr<Job>> compress_queue;
    bool compress_done = false;

    std::mutex write_mu;
    std::condition_variable write_cv;
    std::deque<std::shared_ptr<Job>> write_queue;
    bool write_done = false;

    // Write header (before launching threads)
    size_t head = put_header(out_fd, cfg_);

    // Compress worker thread function
    auto compress_worker = [&]() {
        int actual_level = cfg_.level == -1 ? 6 : cfg_.level;

#ifdef PIGZPP_USE_ISAL
        // ISA-L path for levels 0-9.
        struct isal_zstream istrm;
        std::vector<uint8_t> isal_lvl_buf;
        bool using_isal = use_isal(actual_level);
        if (using_isal) {
            int ilvl = isal_level(actual_level);
            size_t buf_sz = isal_level_buf_size(ilvl);
            if (buf_sz > 0)
                isal_lvl_buf.resize(buf_sz);
            isal_deflate_init(&istrm);
            istrm.level = static_cast<uint32_t>(ilvl);
            istrm.level_buf = isal_lvl_buf.empty() ? nullptr : isal_lvl_buf.data();
            istrm.level_buf_size = static_cast<uint32_t>(isal_lvl_buf.size());
            // Raw deflate — headers/trailers handled by pigzpp format.cpp.
            istrm.gzip_flag = IGZIP_DEFLATE;
        }
#endif

        z_stream strm{};
        std::shared_ptr<Buffer> temp;

#ifdef PIGZPP_USE_ISAL
        if (!using_isal) {
#endif
        if (cfg_.level > 9) {
            temp = out_pool.get();
        } else {
            strm.zalloc = Z_NULL;
            strm.zfree = Z_NULL;
            strm.opaque = Z_NULL;
            int ret = deflateInit2(&strm, actual_level, Z_DEFLATED, -15, 8,
                                   static_cast<int>(cfg_.strategy));
            if (ret != Z_OK) return;
        }
#ifdef PIGZPP_USE_ISAL
        }
#endif

        for (;;) {
            std::shared_ptr<Job> job;
            {
                std::unique_lock lock(compress_mu);
                compress_cv.wait(lock, [&] {
                    return !compress_queue.empty() || compress_done;
                });
                if (compress_queue.empty() && compress_done) break;
                job = compress_queue.front();
                compress_queue.pop_front();
            }

            // Reset for next job (level already set at init)
#ifdef PIGZPP_USE_ISAL
            if (using_isal) {
                isal_deflate_reset(&istrm);
            } else
#endif
            if (cfg_.level <= 9) {
                deflateReset(&strm);
            } else {
                temp->len = 0;
            }

            // Set dictionary
            if (job->dict) {
                size_t dlen = job->dict->len;
                size_t left = dlen < DICT_SIZE ? dlen : DICT_SIZE;
#ifdef PIGZPP_USE_ISAL
                if (using_isal) {
                    isal_deflate_set_dict(&istrm,
                                          job->dict->data() + (dlen - left),
                                          static_cast<uint32_t>(left));
                } else
#endif
                if (cfg_.level <= 9) {
                    deflateSetDictionary(&strm, job->dict->data() + (dlen - left),
                                         static_cast<unsigned>(left));
                } else {
                    std::memcpy(temp->data(), job->dict->data() + (dlen - left), left);
                    temp->len = left;
                }
                job->dict.reset();
            }

            // Get output buffer
            job->out = out_pool.get();
#ifdef PIGZPP_USE_ISAL
            if (using_isal) {
                istrm.next_in = job->in->data();
            } else
#endif
            if (cfg_.level <= 9) {
                strm.next_in = job->in->data();
                strm.next_out = job->out->data();
            } else {
                std::memcpy(temp->data() + temp->len, job->in->data(), job->in->len);
            }

            // Compress blocks
            const unsigned char* lens_ptr = job->lens.empty() ? nullptr : job->lens.data();
            size_t left = job->in->len;
            job->out->len = 0;

            do {
                size_t len = decode_len(lens_ptr, left);
                left -= len;

#ifdef PIGZPP_USE_ISAL
                if (using_isal) {
                    // ISA-L compression path
                    while (len > MAXP2) {
                        istrm.avail_in = MAXP2;
                        isal_deflate_engine(&istrm, *job->out, NO_FLUSH);
                        len -= MAXP2;
                    }
                    istrm.avail_in = static_cast<unsigned>(len);
                    if (left || job->more) {
                        // ISA-L handles byte alignment automatically with
                        // SYNC_FLUSH / FULL_FLUSH.
                        if (!cfg_.setdict) {
                            isal_deflate_engine(&istrm, *job->out, FULL_FLUSH);
                        } else {
                            isal_deflate_engine(&istrm, *job->out, SYNC_FLUSH);
                        }
                    } else {
                        // Last block: set end_of_stream and flush.
                        istrm.end_of_stream = 1;
                        isal_deflate_engine(&istrm, *job->out, NO_FLUSH);
                        istrm.end_of_stream = 0;
                    }
                } else
#endif
                if (cfg_.level <= 9) {
                    while (len > MAXP2) {
                        strm.avail_in = MAXP2;
                        deflate_engine(&strm, *job->out, Z_NO_FLUSH);
                        len -= MAXP2;
                    }

                    strm.avail_in = static_cast<unsigned>(len);
                    if (left || job->more) {
                        int bits = 0;
                        deflate_engine(&strm, *job->out, Z_BLOCK);
                        deflatePending(&strm, Z_NULL, &bits);
                        if ((bits & 1) || !cfg_.setdict)
                            deflate_engine(&strm, *job->out, Z_SYNC_FLUSH);
                        else if (bits & 7) {
                            do {
                                deflatePrime(&strm, 10, 2);
                                deflatePending(&strm, Z_NULL, &bits);
                            } while (bits & 7);
                            deflate_engine(&strm, *job->out, Z_BLOCK);
                        }
                        if (!cfg_.setdict)
                            deflate_engine(&strm, *job->out, Z_FULL_FLUSH);
                    } else {
                        deflate_engine(&strm, *job->out, Z_FINISH);
                    }
                } else {
                    // Zopfli
                    unsigned char bits = 0;
                    unsigned char* zout = nullptr;
                    size_t outsize = 0;
                    ZopfliDeflatePart(&cfg_.zopts, 2, !(left || job->more),
                                      temp->data(), temp->len, temp->len + len,
                                      &bits, &zout, &outsize);

                    // Ensure output buffer is large enough
                    while (job->out->len + outsize + 5 > job->out->size())
                        job->out->grow();

                    std::memcpy(job->out->data() + job->out->len, zout, outsize);
                    free(zout);
                    job->out->len += outsize;

                    if (left || job->more) {
                        bits &= 7;
                        auto& ob = *job->out;
                        if ((bits & 1) || !cfg_.setdict) {
                            if (bits == 0 || bits > 5)
                                ob.buf[ob.len++] = 0;
                            ob.buf[ob.len++] = 0;
                            ob.buf[ob.len++] = 0;
                            ob.buf[ob.len++] = 0xff;
                            ob.buf[ob.len++] = 0xff;
                        } else if (bits) {
                            do {
                                ob.buf[ob.len - 1] += 2 << bits;
                                ob.buf[ob.len++] = 0;
                                bits += 2;
                            } while (bits < 8);
                        }
                        if (!cfg_.setdict) {
                            ob.buf[ob.len++] = 0;
                            ob.buf[ob.len++] = 0;
                            ob.buf[ob.len++] = 0;
                            ob.buf[ob.len++] = 0xff;
                            ob.buf[ob.len++] = 0xff;
                        }
                    }
                    temp->len += len;
                }
            } while (left);

            // Insert job into write queue FIRST so write thread can start
            // writing compressed data while we compute the check value.
            {
                std::lock_guard lock(write_mu);
                auto it = write_queue.begin();
                while (it != write_queue.end() && (*it)->seq < job->seq)
                    ++it;
                write_queue.insert(it, job);
                write_cv.notify_one();
            }

            // Calculate check value in parallel with writing
            size_t in_len = job->in->len;
            unsigned char* next = job->in->data();
            unsigned long chk = check(cfg_.form, 0L, Z_NULL, 0);
            size_t remaining = in_len;
            while (remaining > MAXP2) {
                chk = check(cfg_.form, chk, next, MAXP2);
                remaining -= MAXP2;
                next += MAXP2;
            }
            chk = check(cfg_.form, chk, next, remaining);
            job->check = chk;
            job->check_done.store(1, std::memory_order_release);
        }

        if (cfg_.level <= 9) {
#ifdef PIGZPP_USE_ISAL
            if (!using_isal)
#endif
                deflateEnd(&strm);
        }
    };

    // Write thread function
    int64_t write_seq = 0;
    uint64_t ulen = 0, clen = 0;
    unsigned long check_val = check(cfg_.form, 0L, Z_NULL, 0);

    auto write_worker = [&]() {
        for (;;) {
            std::shared_ptr<Job> job;
            {
                std::unique_lock lock(write_mu);
                write_cv.wait(lock, [&] {
                    return (!write_queue.empty() && write_queue.front()->seq == write_seq) ||
                           write_done;
                });
                if (write_queue.empty() && write_done) break;
                if (write_queue.empty() || write_queue.front()->seq != write_seq) continue;
                job = write_queue.front();
                write_queue.pop_front();
            }

            // Track lengths
            bool more = job->more;
            size_t in_len = job->in->len;
            ulen += in_len;
            clen += job->out->len;

            // Write compressed data
            writen(out_fd, job->out->data(), job->out->len);
            job->out.reset(); // returns to pool via custom deleter

            // Wait for check value (spin-yield: fast because CRC runs
            // in parallel with the write above)
            while (job->check_done.load(std::memory_order_acquire) == 0)
                std::this_thread::yield();
            unsigned long chk = job->check;
            check_val = check_combine(cfg_.form, check_val, chk, in_len,
                                      shift_, cfg_.block);

            job->in.reset();
            write_seq++;

            if (!more) break;
        }
    };

    // Launch write thread
    std::jthread write_thread(write_worker);

    // Launch compress threads
    std::vector<std::jthread> compress_threads;
    int nthreads = cfg_.procs;
    for (int i = 0; i < nthreads; i++)
        compress_threads.emplace_back(compress_worker);

    // Main thread: read input and create jobs
    int64_t seq = 0;
    auto next_buf = in_pool.get();
    next_buf->len = readn(in_fd, next_buf->data(), next_buf->size());
    BufferPtr hold;
    BufferPtr dict;

    unsigned hash = RSYNCHIT;
    unsigned char* scan = next_buf->data();
    size_t rsync_left = 0;

    bool reading = true;
    while (reading) {
        auto job = std::make_shared<Job>();

        auto curr = next_buf;
        next_buf = hold;
        hold.reset();

        if (!next_buf) {
            next_buf = in_pool.get();
            next_buf->len = readn(in_fd, next_buf->data(), next_buf->size());
        }

        // Rsyncable block boundary detection
        if (cfg_.rsync && curr->len) {
            if (rsync_left == 0) {
                unsigned char* last = curr->data();
                unsigned char* end = curr->data() + curr->len;
                while (scan < end) {
                    hash = ((hash << 1) ^ *scan++) & RSYNCMASK;
                    if (hash == RSYNCHIT) {
                        size_t slen = static_cast<size_t>(scan - last);
                        append_len(job->lens, slen);
                        last = scan;
                    }
                }
                rsync_left = static_cast<size_t>(scan - last);
                scan = next_buf->data();
            }

            unsigned char* last = next_buf->data();
            size_t len = curr->size() - curr->len;
            if (len > next_buf->len) len = next_buf->len;
            unsigned char* end = next_buf->data() + len;
            while (scan < end) {
                hash = ((hash << 1) ^ *scan++) & RSYNCMASK;
                if (hash == RSYNCHIT) {
                    size_t slen = static_cast<size_t>(scan - last) + rsync_left;
                    rsync_left = 0;
                    append_len(job->lens, slen);
                    last = scan;
                }
            }
            append_len(job->lens, 0); // end marker

            size_t copy_len = static_cast<size_t>(
                (job->lens.size() == 1 ? scan : last) - next_buf->data());
            if (copy_len) {
                // Grow curr if needed
                while (curr->len + copy_len > curr->size())
                    curr->grow();
                std::memcpy(curr->data() + curr->len, next_buf->data(), copy_len);
                curr->len += copy_len;
                std::memmove(next_buf->data(), next_buf->data() + copy_len,
                             next_buf->len - copy_len);
                next_buf->len -= copy_len;
                scan -= copy_len;
                rsync_left = 0;
            } else if (job->lens.size() != 1 && rsync_left && next_buf->len) {
                hold = next_buf;
                next_buf = in_pool.get();
                std::memcpy(next_buf->data(), curr->data() + (curr->len - rsync_left), rsync_left);
                next_buf->len = rsync_left;
                curr->len -= rsync_left;
            } else {
                rsync_left = 0;
            }
        }

        job->in = curr;
        bool has_more = next_buf->len != 0;
        job->more = has_more;

        // Dictionary setup
        job->dict = dict;
        if (has_more && cfg_.setdict) {
            if (curr->len >= DICT_SIZE || !dict) {
                dict = curr; // shared_ptr keeps it alive
            } else {
                dict = dict_pool.get();
                size_t dlen = DICT_SIZE - curr->len;
                std::memcpy(dict->data(), job->dict->data() + (job->dict->len - dlen), dlen);
                std::memcpy(dict->data() + dlen, curr->data(), curr->len);
                dict->len = DICT_SIZE;
            }
        }

        job->seq = seq++;

        // Enqueue for compression
        {
            std::lock_guard lock(compress_mu);
            compress_queue.push_back(job);
            compress_cv.notify_one();
        }

        if (!has_more) reading = false;
    }

    // Signal compress threads to finish
    {
        std::lock_guard lock(compress_mu);
        compress_done = true;
        compress_cv.notify_all();
    }

    // Wait for compress threads
    compress_threads.clear();

    // Wait for write thread
    write_thread.join();

    // Write trailer
    put_trailer(out_fd, cfg_, ulen, clen, check_val, head);
}

} // namespace pigzpp
