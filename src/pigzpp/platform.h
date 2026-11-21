// Cross-platform wrappers for descriptor I/O used by the native core and CLI.
#pragma once

#include <algorithm>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <limits>
#include <string>

#include <fcntl.h>
#include <sys/stat.h>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <io.h>
#include <process.h>
#else
#include <sys/ioctl.h>
#include <sys/types.h>
#include <unistd.h>
#endif

namespace pigzpp::platform {

#if defined(_WIN32)
using FileStat = struct ::_stat64;
using NativePath = std::wstring;

inline bool native_path(const char* utf8, NativePath& path) {
    int size = ::MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, utf8, -1, nullptr, 0);
    if (size == 0) return false;
    path.resize(static_cast<size_t>(size));
    if (::MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, utf8, -1,
                              path.data(), size) == 0)
        return false;
    path.pop_back();
    return true;
}
#else
using FileStat = struct stat;
using NativePath = std::string;

inline bool native_path(const char* utf8, NativePath& path) {
    path = utf8;
    return true;
}
#endif

inline int open(const char* path, int flags, int mode = 0) {
#if defined(_WIN32)
    return ::_open(path, flags | _O_BINARY, mode);
#else
    return ::open(path, flags, mode);
#endif
}

#if defined(_WIN32)
inline int open(const wchar_t* path, int flags, int mode = 0) {
    return ::_wopen(path, flags | _O_BINARY, mode);
}
#endif

inline std::ptrdiff_t read(int fd, void* buffer, size_t size) {
#if defined(_WIN32)
    unsigned chunk = static_cast<unsigned>(
        std::min<size_t>(size, std::numeric_limits<unsigned>::max()));
    return static_cast<std::ptrdiff_t>(::_read(fd, buffer, chunk));
#else
    return static_cast<std::ptrdiff_t>(::read(fd, buffer, size));
#endif
}

inline std::ptrdiff_t write(int fd, const void* buffer, size_t size) {
#if defined(_WIN32)
    unsigned chunk = static_cast<unsigned>(
        std::min<size_t>(size, std::numeric_limits<unsigned>::max()));
    return static_cast<std::ptrdiff_t>(::_write(fd, buffer, chunk));
#else
    return static_cast<std::ptrdiff_t>(::write(fd, buffer, size));
#endif
}

inline int close(int fd) {
#if defined(_WIN32)
    return ::_close(fd);
#else
    return ::close(fd);
#endif
}

inline int64_t seek(int fd, int64_t offset, int origin) {
#if defined(_WIN32)
    return static_cast<int64_t>(::_lseeki64(fd, offset, origin));
#else
    return static_cast<int64_t>(::lseek(fd, static_cast<off_t>(offset), origin));
#endif
}

inline int truncate(int fd, int64_t size) {
#if defined(_WIN32)
    return ::_chsize_s(fd, static_cast<__int64>(size)) == 0 ? 0 : -1;
#else
    return ::ftruncate(fd, static_cast<off_t>(size));
#endif
}

inline int fstat(int fd, FileStat* result) {
#if defined(_WIN32)
    return ::_fstat64(fd, result);
#else
    return ::fstat(fd, result);
#endif
}

inline int same_file(int lhs_fd, int rhs_fd) {
#if defined(_WIN32)
    HANDLE lhs = reinterpret_cast<HANDLE>(::_get_osfhandle(lhs_fd));
    HANDLE rhs = reinterpret_cast<HANDLE>(::_get_osfhandle(rhs_fd));
    if (lhs == INVALID_HANDLE_VALUE || rhs == INVALID_HANDLE_VALUE)
        return -1;
#if defined(_WIN32_WINNT) && _WIN32_WINNT >= 0x0602
    FILE_ID_INFO lhs_id{}, rhs_id{};
    if (::GetFileInformationByHandleEx(lhs, FileIdInfo, &lhs_id, sizeof(lhs_id)) &&
        ::GetFileInformationByHandleEx(rhs, FileIdInfo, &rhs_id, sizeof(rhs_id))) {
        return lhs_id.VolumeSerialNumber == rhs_id.VolumeSerialNumber &&
                       std::memcmp(&lhs_id.FileId, &rhs_id.FileId, sizeof(lhs_id.FileId)) == 0
                   ? 1
                   : 0;
    }
#endif
    BY_HANDLE_FILE_INFORMATION lhs_info{}, rhs_info{};
    if (!::GetFileInformationByHandle(lhs, &lhs_info) ||
        !::GetFileInformationByHandle(rhs, &rhs_info))
        return -1;
    return lhs_info.dwVolumeSerialNumber == rhs_info.dwVolumeSerialNumber &&
                   lhs_info.nFileIndexHigh == rhs_info.nFileIndexHigh &&
                   lhs_info.nFileIndexLow == rhs_info.nFileIndexLow
               ? 1
               : 0;
#else
    FileStat lhs{}, rhs{};
    if (::fstat(lhs_fd, &lhs) != 0 || ::fstat(rhs_fd, &rhs) != 0)
        return -1;
    return lhs.st_dev == rhs.st_dev && lhs.st_ino == rhs.st_ino ? 1 : 0;
#endif
}

inline int stat(const char* path, FileStat* result) {
#if defined(_WIN32)
    return ::_stat64(path, result);
#else
    return ::stat(path, result);
#endif
}

inline int lstat(const char* path, FileStat* result) {
#if defined(_WIN32)
    return ::_stat64(path, result);
#else
    return ::lstat(path, result);
#endif
}

inline bool is_regular(const FileStat& value) {
#if defined(_WIN32)
    return (value.st_mode & _S_IFMT) == _S_IFREG;
#else
    return S_ISREG(value.st_mode);
#endif
}

inline bool is_directory(const FileStat& value) {
#if defined(_WIN32)
    return (value.st_mode & _S_IFMT) == _S_IFDIR;
#else
    return S_ISDIR(value.st_mode);
#endif
}

inline bool is_fifo(const FileStat& value) {
#if defined(_WIN32)
    return false;
#else
    return S_ISFIFO(value.st_mode);
#endif
}

inline bool is_symlink(const FileStat& value) {
#if defined(_WIN32)
    (void) value;
    return false;
#else
    return S_ISLNK(value.st_mode);
#endif
}

inline std::ptrdiff_t pread(int fd, void* buffer, size_t size, uint64_t offset) {
#if defined(_WIN32)
    // The CRT has no pread(). ReadFile with an explicit OVERLAPPED offset does
    // not touch the descriptor's shared file position, so it stays correct when
    // several threads read the same archive concurrently (matching POSIX
    // pread()). The handle must be opened for overlapped-free synchronous I/O,
    // which is the case for descriptors from _open().
    HANDLE handle = reinterpret_cast<HANDLE>(_get_osfhandle(fd));
    if (handle == INVALID_HANDLE_VALUE)
        return -1;
    DWORD to_read = static_cast<DWORD>(
        std::min<size_t>(size, static_cast<size_t>(0xFFFFFFFFu)));
    OVERLAPPED overlapped{};
    overlapped.Offset = static_cast<DWORD>(offset & 0xFFFFFFFFu);
    overlapped.OffsetHigh = static_cast<DWORD>(offset >> 32);
    DWORD read_bytes = 0;
    if (!ReadFile(handle, buffer, to_read, &read_bytes, &overlapped)) {
        if (GetLastError() == ERROR_HANDLE_EOF)
            return 0;
        return -1;
    }
    return static_cast<std::ptrdiff_t>(read_bytes);
#else
    return static_cast<std::ptrdiff_t>(
        ::pread(fd, buffer, size, static_cast<off_t>(offset)));
#endif
}

inline int unlink(const char* path) {
#if defined(_WIN32)
    return ::_unlink(path);
#else
    return ::unlink(path);
#endif
}

#if defined(_WIN32)
inline int unlink(const wchar_t* path) {
    return ::_wunlink(path);
}
#endif

inline int sync(int fd) {
#if defined(_WIN32)
    return ::_commit(fd);
#else
    return ::fsync(fd);
#endif
}

inline int isatty(int fd) {
#if defined(_WIN32)
    return ::_isatty(fd);
#else
    return ::isatty(fd);
#endif
}

// Zero means the descriptor is not a terminal or its width is unavailable.
inline int terminal_width(int fd) {
#if defined(_WIN32)
    CONSOLE_SCREEN_BUFFER_INFO info{};
    HANDLE handle = reinterpret_cast<HANDLE>(::_get_osfhandle(fd));
    return GetConsoleScreenBufferInfo(handle, &info)
        ? info.srWindow.Right - info.srWindow.Left + 1 : 0;
#elif defined(TIOCGWINSZ)
    struct winsize size{};
    return ::ioctl(fd, TIOCGWINSZ, &size) == 0 ? size.ws_col : 0;
#else
    (void) fd;
    return 0;
#endif
}

inline int fileno(FILE* file) {
#if defined(_WIN32)
    return ::_fileno(file);
#else
    return ::fileno(file);
#endif
}

inline void set_binary(int fd) {
#if defined(_WIN32)
    (void) ::_setmode(fd, _O_BINARY);
#else
    (void) fd;
#endif
}

inline bool localtime(std::time_t value, std::tm& result) {
#if defined(_WIN32)
    return ::localtime_s(&result, &value) == 0;
#else
    return ::localtime_r(&value, &result) != nullptr;
#endif
}

} // namespace pigzpp::platform
