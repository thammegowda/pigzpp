#include "io.h"

#include <cerrno>
#include <cstring>
#include <stdexcept>
#include <string>
#include <unistd.h>

namespace pigzpp {

size_t readn(int fd, unsigned char* buf, size_t len) {
    size_t got = 0;
    while (got < len) {
        auto ret = ::read(fd, buf + got, len - got);
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
        auto ret = ::write(fd, buf + written, len - written);
        if (ret < 0) {
            if (errno == EINTR) continue;
            throw std::runtime_error("write error: " + std::string(strerror(errno)));
        }
        written += static_cast<size_t>(ret);
    }
    return written;
}

} // namespace pigzpp
