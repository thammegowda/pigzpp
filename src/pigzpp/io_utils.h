// I/O utilities: full read/write with EINTR retry.

#pragma once

#include "config.h"

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

namespace pigzpp {

// Formats compression/decompression progress (e.g. " 50.0% 1.0GiB/2.0GiB -> 512.0MiB 400.0MiB/s").
// Defaults to stderr (never stdout): redraws in place on terminals when the line fits,
// or writes a single summary line when redirected. Applications can pass another FILE*
// or a custom Sink callback to redirect progress anywhere.
class Progress {
public:
    using Sink = std::function<void(std::string_view)>;

    explicit Progress(uint64_t total_bytes, FILE* stream = stderr);
    Progress(uint64_t total_bytes, Sink sink, std::function<int()> width_fn = {});
    Progress(const Progress&) = delete;
    Progress& operator=(const Progress&) = delete;
    ~Progress();

    static std::string format(uint64_t in, uint64_t total, uint64_t out,
                              double elapsed_secs, int width = 0);

    // Wire `cfg.progress` and `cfg.progress_interval` to this Progress instance
    // (must outlive operations using `cfg`). Call finish() after completion.
    void attach(Config& cfg,
                std::chrono::milliseconds interval = std::chrono::milliseconds(500));
    void update(uint64_t in, uint64_t out);
    void finish();

    bool is_tty() const noexcept { return static_cast<bool>(width_fn_); }
    bool active() const noexcept { return drawn_.load(std::memory_order_relaxed) != 0; }

private:
    void print(bool final = false);

    uint64_t total_;
    Sink sink_;
    std::function<int()> width_fn_;
    std::chrono::steady_clock::time_point start_ = std::chrono::steady_clock::now();
    uint64_t in_ = 0;
    uint64_t out_ = 0;
    std::atomic<size_t> drawn_{0};
    bool finished_ = false;
};

// Forwards to cfg.progress at most once per cfg.progress_interval; final
// reports always go through. Skips clock queries when progress or throttling
// is disabled. Used by one thread at a time.
class ProgressReporter {
public:
    explicit ProgressReporter(const Config& cfg) : cfg_(cfg) {}
    void operator()(uint64_t in, uint64_t out, bool final = false) {
        if (!cfg_.progress) return;
        if (final || cfg_.progress_interval.count() <= 0 ||
            std::chrono::steady_clock::now() - shown_ >= cfg_.progress_interval) {
            cfg_.progress(in, out);
            if (!final && cfg_.progress_interval.count() > 0)
                shown_ = std::chrono::steady_clock::now();
        }
    }

private:
    const Config& cfg_;
    std::chrono::steady_clock::time_point shown_ =
        cfg_.progress && cfg_.progress_interval.count() > 0
            ? std::chrono::steady_clock::now()
            : std::chrono::steady_clock::time_point{};
};

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
