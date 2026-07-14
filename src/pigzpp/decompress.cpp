// Decompression of gzip, zlib, and zip streams.
// Parallel write and check threads for performance parity with pigz.
//
// When compiled with PIGZPP_USE_ISAL, uses Intel ISA-L for DEFLATE decompression.

#include "decompress.h"
#include "crc.h"
#include "format.h"
#include "io.h"

#include <atomic>
#include <algorithm>
#include <cassert>
#include <climits>
#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <vector>

#include <unistd.h>
#include <zlib.h>

#ifdef PIGZPP_USE_ISAL
#include <isa-l/igzip_lib.h>
#endif

namespace pigzpp {

static constexpr unsigned OUTSIZE = 262144U; // 256 KB — reduce push() calls & memcpy overhead
static constexpr uint64_t LOW32 = 0xffffffff;

// --- Parallel output handler (write + check threads) ---
// Mirrors pigz's outb_write / outb_check / outb pattern.
// Uses atomics with spin-yield for minimal synchronization overhead,
// since the operations are fast and thread wakeup latency dominates.

struct ParallelOutput {
    // Buffer: inflate output is copied here for worker threads.
    // Heap-allocated to avoid large stack frames with 256 KB OUTSIZE.
    std::unique_ptr<unsigned char[]> out_copy{new unsigned char[OUTSIZE]};
    std::atomic<size_t> out_len{0};

    // State: 0=idle, 1=work, 2=done(sentinel)
    std::atomic<int> write_state{0};
    std::atomic<int> check_state{0};

    // Results
    int out_fd = -1;
    bool testing = false;
    Format form = Format::Gzip;
    unsigned long out_check = 0;
    uint64_t out_tot = 0;

    std::jthread write_thread;
    std::jthread check_thread;
    bool started = false;

    void start(int fd, bool test, Format f) {
        out_fd = fd;
        testing = test;
        form = f;
        out_check = check(f, 0L, Z_NULL, 0);
        out_tot = 0;
        write_state.store(0, std::memory_order_relaxed);
        check_state.store(0, std::memory_order_relaxed);

        write_thread = std::jthread([this] { write_worker(); });
        check_thread = std::jthread([this] { check_worker(); });
        started = true;
    }

    // Called from inflate's outb callback.
    void push(unsigned char* buf, unsigned len) {
        if (!started) return;

        // Wait for previous operations to complete (spin-yield)
        while (check_state.load(std::memory_order_acquire) != 0)
            std::this_thread::yield();
        while (write_state.load(std::memory_order_acquire) != 0)
            std::this_thread::yield();

        // Copy output and signal workers
        out_len.store(len, std::memory_order_relaxed);
        if (len) {
            out_tot += len;
            std::memcpy(out_copy.get(), buf, len);
        }

        // Release to workers (write fence ensures memcpy is visible)
        write_state.store(1, std::memory_order_release);
        check_state.store(1, std::memory_order_release);
    }

    void finish() {
        if (!started) return;
        push(nullptr, 0); // sentinel
        write_thread.join();
        check_thread.join();
        started = false;
    }

private:
    void write_worker() {
        for (;;) {
            while (write_state.load(std::memory_order_acquire) == 0)
                std::this_thread::yield();
            size_t len = out_len.load(std::memory_order_relaxed);
            if (len && !testing)
                writen(out_fd, out_copy.get(), len);
            write_state.store(0, std::memory_order_release);
            if (len == 0) break;
        }
    }

    void check_worker() {
        for (;;) {
            while (check_state.load(std::memory_order_acquire) == 0)
                std::this_thread::yield();
            size_t len = out_len.load(std::memory_order_relaxed);
            if (len)
                out_check = check(form, out_check, out_copy.get(), len);
            check_state.store(0, std::memory_order_release);
            if (len == 0) break;
        }
    }
};

// --- Inflate context (passed through callbacks) ---

struct InflateContext {
    InputReader* reader;
    ParallelOutput* par_out; // non-null when procs > 1
    int out_fd;
    Format form;
    bool testing;
    unsigned long out_check;
    uint64_t out_tot;
    int procs;
};

// Callback: provide input for inflateBack.
static unsigned inb_cb(void* desc, unsigned char** buf) {
    auto* ctx = static_cast<InflateContext*>(desc);
    auto* r = ctx->reader;
    if (r->left() == 0) {
        size_t loaded = r->load();
        if (loaded == 0) {
            *buf = r->next();
            return 0;
        }
    }
    *buf = r->next();
    unsigned len = r->left() > UINT_MAX ? UINT_MAX : static_cast<unsigned>(r->left());
    r->consume(len);
    return len;
}

// Callback: consume output from inflateBack.
static int outb_cb(void* desc, unsigned char* buf, unsigned len) {
    auto* ctx = static_cast<InflateContext*>(desc);

    if (ctx->procs > 1 && ctx->par_out) {
        // Parallel path: hand off to write+check threads
        ctx->par_out->push(buf, len);
        return 0;
    }

    // Sequential path
    if (len) {
        if (!ctx->testing)
            writen(ctx->out_fd, buf, len);
        ctx->out_check = check(ctx->form, ctx->out_check, buf, len);
        ctx->out_tot += len;
    }
    return 0;
}

Decompressor::Decompressor(const Config& cfg) : cfg_(cfg) {}
Decompressor::~Decompressor() = default;

void Decompressor::decompress(int in_fd, int out_fd) {
#ifdef POSIX_FADV_SEQUENTIAL
    posix_fadvise(in_fd, 0, 0, POSIX_FADV_SEQUENTIAL);
#endif
    infchk(in_fd, out_fd);
}

void Decompressor::list(int in_fd) {
    // For listing, decompress to /dev/null effectively
    infchk(in_fd, -1);
}

std::vector<uint8_t> Decompressor::decompress_buffer(const uint8_t* data, size_t size) {
    // Fast path: gzip/zlib streams inflate directly in memory (auto-detect via
    // windowBits 15+32), skipping the temp-fd round-trip. Zip framing and other
    // cases fall through to the fd pipeline below.
    const bool looks_gzip = size >= 2 && data[0] == 0x1f && data[1] == 0x8b;
    const bool looks_zlib = size >= 2 && (data[0] & 0x0f) == 0x08 &&
                            ((static_cast<unsigned>(data[0]) << 8 | data[1]) % 31) == 0;
    if (looks_gzip || looks_zlib)
        return direct_decompress(data, size);

    // Fallback (zip framing, etc.): run the fd pipeline over tmpfs temp fds.
    return run_via_temp_fds(data, size,
                            [this](int in_fd, int out_fd) { decompress(in_fd, out_fd); });
}

std::vector<uint8_t> Decompressor::direct_decompress(const uint8_t* data, size_t size) {
    z_stream zs{};
    // windowBits 15+32: auto-detect gzip or zlib wrapper.
    if (inflateInit2(&zs, 15 + 32) != Z_OK)
        throw std::runtime_error("decompress_buffer: inflateInit2 failed");

    std::vector<uint8_t> out(size ? size * 4 + 1024 : 1024);
    const auto* in_ptr = reinterpret_cast<const Bytef*>(data);
    size_t in_left = size;

    int ret;
    for (;;) {
        if (zs.avail_in == 0 && in_left > 0) {
            const uInt c = in_left > UINT_MAX ? UINT_MAX : static_cast<uInt>(in_left);
            zs.next_in = const_cast<Bytef*>(in_ptr);
            zs.avail_in = c;
            in_ptr += c;
            in_left -= c;
        }
        // Track the write position via total_out so a resize (realloc) is safe.
        if (zs.total_out == out.size())
            out.resize(out.size() + (out.size() >> 1) + 4096);
        zs.next_out = out.data() + zs.total_out;
        zs.avail_out = static_cast<uInt>(
            std::min<size_t>(out.size() - zs.total_out, UINT_MAX));

        ret = inflate(&zs, Z_NO_FLUSH);
        if (ret == Z_STREAM_END) break;
        if (ret != Z_OK) { // Z_BUF_ERROR here means truncated input.
            inflateEnd(&zs);
            throw std::runtime_error("decompress_buffer: inflate failed");
        }
    }
    out.resize(zs.total_out);
    inflateEnd(&zs);
    return out;
}

void Decompressor::infchk(int in_fd, int out_fd) {
    InputReader reader(in_fd, cfg_.procs);
    reader.reset();

    auto hdr = reader.get_header(true);
    if (hdr.method < 0)
        throw std::runtime_error("unrecognized compressed format");
    if (hdr.method != 8)
        throw std::runtime_error("unsupported compression method");

    Format form = hdr.form;
    bool is_test = (cfg_.mode == Mode::Test);
    bool is_list = (cfg_.mode == Mode::List);
    bool use_parallel = cfg_.procs > 1 && !is_list;

    int cont = 0;
    bool more_entries = false;

    // Set up parallel output handler (reused across gzip members)
    ParallelOutput par_out;

    do {
        uint64_t in_tot_start = reader.total() - reader.left();

        InflateContext ctx{};
        ctx.reader = &reader;
        ctx.par_out = use_parallel ? &par_out : nullptr;
        ctx.out_fd = (is_test || is_list) ? -1 : out_fd;
        ctx.form = form;
        ctx.testing = is_test || is_list;
        ctx.out_check = check(form, 0L, Z_NULL, 0);
        ctx.out_tot = 0;
        ctx.procs = cfg_.procs;

        // Start parallel threads for this member
        if (use_parallel)
            par_out.start(ctx.out_fd, ctx.testing, form);

#ifdef PIGZPP_USE_ISAL
        {
            // ISA-L decompression: streaming isal_inflate loop.
            // Raw deflate — headers/trailers are already parsed by InputReader.
            struct inflate_state istate;
            isal_inflate_init(&istate);
            istate.crc_flag = ISAL_DEFLATE; // raw deflate, pigzpp handles headers/checksums
            istate.hist_bits = 15;

            unsigned char outbuf[OUTSIZE];
            int iret = ISAL_DECOMP_OK;

            istate.avail_in = 0;
            istate.next_in = nullptr;

            auto emit = [&](unsigned char* buf, unsigned len) {
                if (len == 0) return;
                if (use_parallel && ctx.par_out) {
                    ctx.par_out->push(buf, len);
                } else {
                    if (!ctx.testing)
                        writen(ctx.out_fd, buf, len);
                    ctx.out_check = check(ctx.form, ctx.out_check, buf, len);
                    ctx.out_tot += len;
                }
            };

            for (;;) {
                // Refill input when exhausted
                if (istate.avail_in == 0) {
                    auto* r = ctx.reader;
                    if (r->left() == 0) {
                        if (r->load() == 0) {
                            // No more input — drain any remaining output
                            do {
                                istate.next_out = outbuf;
                                istate.avail_out = OUTSIZE;
                                iret = isal_inflate(&istate);
                                emit(outbuf, OUTSIZE - istate.avail_out);
                            } while (istate.avail_out == 0);
                            break;
                        }
                    }
                    istate.next_in = r->next();
                    unsigned avail = r->left() > UINT_MAX ? UINT_MAX
                                       : static_cast<unsigned>(r->left());
                    istate.avail_in = avail;
                    r->consume(avail);
                }

                // Decompress
                istate.next_out = outbuf;
                istate.avail_out = OUTSIZE;
                iret = isal_inflate(&istate);
                emit(outbuf, OUTSIZE - istate.avail_out);

                if (istate.block_state == ISAL_BLOCK_FINISH) {
                    // Drain: ISA-L may still have buffered output
                    while (istate.avail_out == 0) {
                        istate.next_out = outbuf;
                        istate.avail_out = OUTSIZE;
                        iret = isal_inflate(&istate);
                        emit(outbuf, OUTSIZE - istate.avail_out);
                    }
                    break;
                }

                if (iret != ISAL_DECOMP_OK) {
                    if (iret == ISAL_END_INPUT)
                        break;
                    if (iret == ISAL_INVALID_BLOCK || iret == ISAL_INVALID_SYMBOL)
                        throw std::runtime_error("corrupted data: invalid deflate block");
                    if (iret == ISAL_INVALID_LOOKBACK)
                        throw std::runtime_error("corrupted data: invalid lookback distance");
                    throw std::runtime_error("isal_inflate failed: " + std::to_string(iret));
                }
            }

            // Return unconsumed input to reader.
            // ISA-L uses a 64-bit bit buffer (read_in). After the DEFLATE
            // stream ends, some trailer bytes may already have been read into
            // read_in but are past the deflate data.  Back up by the number
            // of whole bytes buffered so the reader can parse the trailer.
            {
                int buffered_bytes = istate.read_in_length / 8;
                reader.restore(istate.next_in - buffered_bytes,
                               istate.avail_in + buffered_bytes);
            }
        }
#else
        {
            unsigned char window[OUTSIZE];
            z_stream strm{};
            strm.zalloc = Z_NULL;
            strm.zfree = Z_NULL;
            strm.opaque = Z_NULL;
            int ret = inflateBackInit(&strm, 15, window);
            if (ret != Z_OK)
                throw std::runtime_error("inflateBackInit failed");

            strm.avail_in = 0;
            strm.next_in = Z_NULL;
            ret = inflateBack(&strm, inb_cb, &ctx, outb_cb, &ctx);
            inflateBackEnd(&strm);
            reader.restore(strm.next_in, strm.avail_in);

            if (ret == Z_DATA_ERROR)
                throw std::runtime_error(std::string("corrupted data: ") + (strm.msg ? strm.msg : ""));
            if (ret == Z_BUF_ERROR)
                throw std::runtime_error("corrupted: incomplete deflate data");
            if (ret != Z_STREAM_END)
                throw std::runtime_error("internal inflate error");
        }
#endif

        // Finish parallel threads, collect totals
        if (use_parallel) {
            par_out.finish();
            ctx.out_check = par_out.out_check;
            ctx.out_tot = par_out.out_tot;
        }

        uint64_t clen = (reader.total() - reader.left()) - in_tot_start;

        // Read and verify trailer
        if (form == Format::Zip) {
            if (hdr.has_data_descriptor) {
                hdr.zip_crc = reader.get4();
                hdr.zip_clen = reader.get4();
                hdr.zip_ulen = reader.get4();

                // Detect optional signature
                static constexpr unsigned long SIG = 0x08074b50;
                if (hdr.zip_crc == SIG && ctx.out_check != SIG) {
                    hdr.zip_crc = static_cast<unsigned long>(hdr.zip_clen);
                    hdr.zip_clen = hdr.zip_ulen;
                    hdr.zip_ulen = reader.get4();
                }
                if (hdr.zip64) {
                    hdr.zip_ulen = reader.get4();
                    reader.get4(); // skip high word
                }
            }
            if (hdr.zip_crc != ctx.out_check)
                throw std::runtime_error("corrupted: crc32 mismatch");
            if ((hdr.zip_clen & LOW32) != (clen & LOW32) ||
                (hdr.zip_ulen & LOW32) != (ctx.out_tot & LOW32))
                throw std::runtime_error("corrupted: length mismatch");
        }
        else if (form == Format::Zlib) {
            unsigned long check = 
                (static_cast<unsigned long>(reader.get()) << 24) +
                (static_cast<unsigned long>(reader.get()) << 16) +
                (static_cast<unsigned>(reader.get()) << 8) +
                reader.get();
            if (reader.eof())
                throw std::runtime_error("corrupted: missing trailer");
            if (check != ctx.out_check)
                throw std::runtime_error("corrupted: adler32 mismatch");
        }
        else { // Gzip
            unsigned long check = reader.get4();
            unsigned long len = reader.get4();
            if (reader.eof())
                throw std::runtime_error("corrupted: missing trailer");
            if (check != ctx.out_check)
                throw std::runtime_error("corrupted: crc32 mismatch");
            if (len != (ctx.out_tot & LOW32))
                throw std::runtime_error("corrupted: length mismatch");
        }

        if (is_list) {
            // Print listing info
            double red = ctx.out_tot > 0
                ? 100.0 * (ctx.out_tot - static_cast<double>(clen)) / ctx.out_tot
                : 0.0;
            if (cfg_.verbosity > 0 && cont == 0)
                printf("compressed   original reduced  name\n");
            if (cfg_.verbosity > 0)
                printf("%10lu %10lu %6.1f%%  %s\n",
                       static_cast<unsigned long>(clen),
                       static_cast<unsigned long>(ctx.out_tot),
                       red,
                       hdr.hname.empty() ? "<stdin>" : hdr.hname.c_str());
            cont = cont ? 2 : 1;
        }

        // Check for another gzip member
        more_entries = false;
        if (form == Format::Gzip) {
            auto next_hdr = reader.get_header(false);
            if (next_hdr.method == 8) {
                hdr = next_hdr;
                more_entries = true;
            }
        }
    } while (more_entries);
}

} // namespace pigzpp
