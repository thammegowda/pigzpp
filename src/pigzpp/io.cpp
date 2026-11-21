#include "io_utils.h"
#include "platform.h"

#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <iterator>
#include <stdexcept>
#include <string>
#include <utility>

namespace pigzpp {

static std::string format_bytes(double bytes) {
    static constexpr const char* units[] = {"B", "KiB", "MiB", "GiB", "TiB", "PiB", "EiB"};
    size_t unit = 0;
    while (bytes >= 1024.0 && unit + 1 < std::size(units)) {
        bytes /= 1024.0;
        ++unit;
    }
    char text[64];
    std::snprintf(text, sizeof text, "%.1f%s", bytes, units[unit]);
    return text;
}

Progress::Progress(uint64_t total_bytes, FILE* stream)
    : total_(total_bytes) {
    if (!stream) return;
    sink_ = [stream](std::string_view s) {
        if (!s.empty()) {
            std::fwrite(s.data(), 1, s.size(), stream);
            std::fflush(stream);
        }
    };
    int fd = platform::fileno(stream);
    if (fd >= 0 && platform::isatty(fd))
        width_fn_ = [fd] { return platform::terminal_width(fd); };
}

Progress::Progress(uint64_t total_bytes, Sink sink, std::function<int()> width_fn)
    : total_(total_bytes), sink_(std::move(sink)), width_fn_(std::move(width_fn)) {}

Progress::~Progress() {
    if (drawn_.load(std::memory_order_relaxed) != 0) {
        if (sink_) sink_("\n");
        drawn_.store(0, std::memory_order_relaxed);
    }
}

std::string Progress::format(uint64_t in, uint64_t total, uint64_t out,
                             double elapsed_secs, int width) {
    double pct = total ? std::min(100.0, 100.0 * static_cast<double>(in) / static_cast<double>(total))
                       : 100.0;
    char percent[16];
    std::snprintf(percent, sizeof percent, "%5.1f%%", pct);
    std::string rate = format_bytes(elapsed_secs > 0 ? static_cast<double>(in) / elapsed_secs : 0.0) + "/s";
    std::string text = std::string(percent) + " " + format_bytes(static_cast<double>(in)) + "/" +
        format_bytes(static_cast<double>(total)) + " -> " + format_bytes(static_cast<double>(out)) + " " + rate;
    // Leave the last column unused: some terminals wrap as soon as it is filled.
    if (width > 0 && text.size() >= static_cast<size_t>(width)) {
        text = std::string(percent) + " " + rate;
        if (text.size() >= static_cast<size_t>(width))
            text = percent;
    }
    return text;
}

void Progress::attach(Config& cfg, std::chrono::milliseconds interval) {
    cfg.progress_interval = is_tty() ? interval : std::chrono::hours(24);
    cfg.progress = [this](uint64_t in, uint64_t out) { update(in, out); };
}

void Progress::update(uint64_t in, uint64_t out) {
    in_ = in;
    out_ = out;
    if (is_tty()) print(false);
}

void Progress::finish() {
    if (finished_) return;
    finished_ = true;
    if (drawn_.load(std::memory_order_relaxed) != 0) {
        if (sink_) sink_("\n");
        drawn_.store(0, std::memory_order_relaxed);
    } else {
        print(true);
    }
}

void Progress::print(bool final) {
    if (!sink_) return;
    double secs = std::chrono::duration<double>(
        std::chrono::steady_clock::now() - start_).count();
    int width = width_fn_ ? width_fn_() : 0;
    std::string text = format(in_, total_, out_, secs, width);
    size_t prev = drawn_.load(std::memory_order_relaxed);
    if (prev && (width <= 0 || prev >= static_cast<size_t>(width))) {
        sink_("\n"); // resized: do not overwrite a possibly wrapped row
        drawn_.store(0, std::memory_order_relaxed);
        prev = 0;
    }
    if (width > 0 && text.size() < static_cast<size_t>(width)) {
        size_t padded = std::max(prev, text.size());
        std::string line = "\r" + text;
        line.append(padded - text.size(), ' ');
        if (final) line.push_back('\n');
        drawn_.store(final ? 0 : padded, std::memory_order_relaxed);
        sink_(line);
    } else if (final) {
        sink_(text + "\n");
    }
}

size_t readn(int fd, unsigned char* buf, size_t len) {
    size_t got = 0;
    while (got < len) {
        auto ret = platform::read(fd, buf + got, len - got);
        if (ret < 0) {
            if (errno == EINTR) continue;
            throw std::runtime_error("read error: " + std::string(strerror(errno)));
        }
        if (ret == 0) break; // EOF
        got += static_cast<size_t>(ret);
    }
    return got;
}

size_t writen(int fd, const unsigned char* buf, size_t len) {
    size_t written = 0;
    while (written < len) {
        auto ret = platform::write(fd, buf + written, len - written);
        if (ret < 0) {
            if (errno == EINTR) continue;
            throw std::runtime_error("write error: " + std::string(strerror(errno)));
        }
        written += static_cast<size_t>(ret);
    }
    return written;
}

std::vector<uint8_t> run_via_temp_fds(
    const uint8_t* data, size_t size,
    const std::function<void(int, int)>& op) {
    // On Linux these temp files live in tmpfs; under Emscripten in MEMFS.
    // Seekability preserves the block-size and thread-count heuristics that the
    // fd-based compress()/decompress() paths rely on.
    FILE* in = std::tmpfile();
    if (!in) throw std::runtime_error("run_via_temp_fds: cannot create temp input");
    FILE* out = std::tmpfile();
    if (!out) {
        std::fclose(in);
        throw std::runtime_error("run_via_temp_fds: cannot create temp output");
    }

    try {
        if (size > 0 && std::fwrite(data, 1, size, in) != size)
            throw std::runtime_error("run_via_temp_fds: temp input write failed");
        std::fflush(in);

        const int in_fd = platform::fileno(in);
        const int out_fd = platform::fileno(out);
        if (in_fd < 0 || out_fd < 0)
            throw std::runtime_error("run_via_temp_fds: fileno failed");
        if (platform::seek(in_fd, 0, SEEK_SET) < 0)
            throw std::runtime_error("run_via_temp_fds: lseek(input) failed");

        op(in_fd, out_fd);

        const int64_t osize = platform::seek(out_fd, 0, SEEK_END);
        if (osize < 0)
            throw std::runtime_error("run_via_temp_fds: lseek(output) failed");
        platform::seek(out_fd, 0, SEEK_SET);
        std::vector<uint8_t> result(osize > 0 ? static_cast<size_t>(osize) : 0);
        const size_t got = readn(out_fd, result.data(), result.size());
        result.resize(got);

        std::fclose(in);
        std::fclose(out);
        return result;
    } catch (...) {
        std::fclose(in);
        std::fclose(out);
        throw;
    }
}

} // namespace pigzpp
