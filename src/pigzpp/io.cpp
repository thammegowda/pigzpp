#include "io_utils.h"
#include "platform.h"

#include <cerrno>
#include <cstdio>
#include <cstring>
#include <stdexcept>
#include <string>

namespace pigzpp {

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
