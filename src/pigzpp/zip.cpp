// Native ZIP archive support — see zip.h.

#include "zip.h"

#include "compress.h"
#include "format.h"
#include "io.h"

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <ctime>
#include <stdexcept>
#include <thread>
#include <unordered_map>

#include <fcntl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include <zlib.h>

namespace pigzpp::zip {

namespace {

// ---- Little-endian helpers ------------------------------------------------

void put_le(std::vector<uint8_t>& v, uint64_t val, int bytes) {
    for (int i = 0; i < bytes; ++i) v.push_back(static_cast<uint8_t>((val >> (8 * i)) & 0xFF));
}

uint64_t get_le(const uint8_t* p, int bytes) {
    uint64_t v = 0;
    for (int i = 0; i < bytes; ++i) v |= static_cast<uint64_t>(p[i]) << (8 * i);
    return v;
}

// ---- ZIP constants --------------------------------------------------------

constexpr uint32_t SIG_LOCAL   = 0x04034b50;
constexpr uint32_t SIG_CENTRAL = 0x02014b50;
constexpr uint32_t SIG_EOCD    = 0x06054b50;
constexpr uint32_t SIG_Z64_EOCD    = 0x06064b50;
constexpr uint32_t SIG_Z64_LOCATOR = 0x07064b50;

constexpr uint16_t ZIP64_EXTRA_ID  = 0x0001;
constexpr uint16_t FLAG_UTF8       = 0x0800;
constexpr uint16_t VER_DEFAULT     = 20;
constexpr uint16_t VER_ZIP64       = 45;
constexpr uint16_t VER_MADE_BY     = (3 << 8) | 45;   // unix host, spec 4.5
constexpr uint64_t LOW32           = 0xFFFFFFFFULL;

int64_t now_seconds() { return static_cast<int64_t>(::time(nullptr)); }

int auto_threads(int threads) {
    if (threads > 0) return threads;
    unsigned hc = std::thread::hardware_concurrency();
    return hc == 0 ? 1 : static_cast<int>(hc);
}

// ---- DEFLATE / INFLATE for members ---------------------------------------

// Raw DEFLATE a buffer using pigzpp's (parallel) compressor.
std::vector<uint8_t> raw_deflate(const uint8_t* data, size_t size,
                                 int level, int threads, Engine engine) {
    Config cfg;
    cfg.form = Format::Raw;
    cfg.mode = Mode::Compress;
    cfg.level = level;
    cfg.engine = engine;
    cfg.procs = auto_threads(threads);
    Compressor comp(cfg);
    uint8_t* out = nullptr;
    size_t n = comp.compress_buffer(data, size, &out);
    std::vector<uint8_t> v(out, out + n);
    std::free(out);
    return v;
}

// Raw INFLATE a member into a buffer of known uncompressed size.
std::vector<uint8_t> raw_inflate(const uint8_t* data, size_t clen, uint64_t ulen) {
    std::vector<uint8_t> out(static_cast<size_t>(ulen));
    if (ulen == 0) return out;

    z_stream s{};
    if (inflateInit2(&s, -15) != Z_OK)
        throw std::runtime_error("zip: inflateInit2 failed");

    const auto* in_ptr = reinterpret_cast<const Bytef*>(data);
    size_t in_left = clen;
    auto* out_ptr = reinterpret_cast<Bytef*>(out.data());
    size_t out_left = out.size();

    int ret;
    do {
        if (s.avail_in == 0 && in_left > 0) {
            const uInt c = in_left > UINT_MAX ? UINT_MAX : static_cast<uInt>(in_left);
            s.next_in = const_cast<Bytef*>(in_ptr);
            s.avail_in = c;
            in_ptr += c;
            in_left -= c;
        }
        if (s.avail_out == 0 && out_left > 0) {
            const uInt c = out_left > UINT_MAX ? UINT_MAX : static_cast<uInt>(out_left);
            s.next_out = out_ptr;
            s.avail_out = c;
            out_ptr += c;
            out_left -= c;
        }
        ret = inflate(&s, Z_NO_FLUSH);
    } while (ret == Z_OK && (in_left > 0 || out_left > 0 || s.avail_out == 0));

    inflateEnd(&s);
    if (ret != Z_STREAM_END && ret != Z_OK)
        throw std::runtime_error("zip: inflate failed (" + std::to_string(ret) + ")");
    if (s.total_out != ulen)
        throw std::runtime_error("zip: decompressed size mismatch");
    return out;
}

uint32_t crc_of(const uint8_t* data, size_t size) {
    uLong c = ::crc32(0L, Z_NULL, 0);
    const auto* p = reinterpret_cast<const Bytef*>(data);
    while (size > 0) {
        uInt chunk = size > UINT_MAX ? UINT_MAX : static_cast<uInt>(size);
        c = ::crc32(c, p, chunk);
        p += chunk;
        size -= chunk;
    }
    return static_cast<uint32_t>(c);
}

// ---- Name normalization / path safety ------------------------------------

std::string normalize_name(std::string name) {
    for (auto& c : name)
        if (c == '\\') c = '/';
    while (!name.empty() && name.front() == '/') name.erase(name.begin());
    return name;
}

// Join dest_dir with an archive name, rejecting absolute paths and traversal.
std::string safe_join(const std::string& dest_dir, const std::string& arcname) {
    std::string name = normalize_name(arcname);
    std::string result = dest_dir;
    if (!result.empty() && result.back() != '/') result += '/';

    size_t start = 0;
    while (start <= name.size()) {
        size_t slash = name.find('/', start);
        std::string comp = name.substr(start, slash == std::string::npos ? std::string::npos
                                                                          : slash - start);
        if (comp == "..")
            throw std::runtime_error("zip: unsafe path component in '" + arcname + "'");
        if (!comp.empty() && comp != ".") {
            if (result.back() != '/') result += '/';
            result += comp;
        }
        if (slash == std::string::npos) break;
        start = slash + 1;
    }
    return result;
}

void make_dirs(const std::string& path) {
    if (path.empty()) return;
    std::string cur;
    size_t i = 0;
    if (path[0] == '/') { cur = "/"; i = 1; }
    while (i <= path.size()) {
        size_t slash = path.find('/', i);
        std::string comp = path.substr(i, slash == std::string::npos ? std::string::npos
                                                                     : slash - i);
        if (!comp.empty()) {
            if (!cur.empty() && cur.back() != '/') cur += '/';
            cur += comp;
            if (::mkdir(cur.c_str(), 0777) != 0 && errno != EEXIST)
                throw std::runtime_error("zip: mkdir '" + cur + "' failed: " + std::strerror(errno));
        }
        if (slash == std::string::npos) break;
        i = slash + 1;
    }
}

// ---- Output sink (fd or growable buffer) ----------------------------------

struct Sink {
    int fd = -1;
    std::vector<uint8_t>* buf = nullptr;
    uint64_t offset = 0;

    void write(const uint8_t* p, size_t n) {
        if (buf) buf->insert(buf->end(), p, p + n);
        else writen(fd, p, n);
        offset += n;
    }
    void write(const std::vector<uint8_t>& v) { write(v.data(), v.size()); }
};

// ---- Input source (fd via pread, or buffer) -------------------------------

struct Source {
    int fd = -1;
    const std::vector<uint8_t>* buf = nullptr;

    uint64_t size() const {
        if (buf) return buf->size();
        struct stat st{};
        if (::fstat(fd, &st) != 0)
            throw std::runtime_error("zip: fstat failed: " + std::string(std::strerror(errno)));
        return static_cast<uint64_t>(st.st_size);
    }

    std::vector<uint8_t> read_at(uint64_t off, size_t len) const {
        std::vector<uint8_t> out(len);
        if (buf) {
            if (off + len > buf->size())
                throw std::runtime_error("zip: read past end of archive");
            std::memcpy(out.data(), buf->data() + off, len);
            return out;
        }
        size_t got = 0;
        while (got < len) {
            ssize_t r = ::pread(fd, out.data() + got, len - got, static_cast<off_t>(off + got));
            if (r < 0) {
                if (errno == EINTR) continue;
                throw std::runtime_error("zip: pread failed: " + std::string(std::strerror(errno)));
            }
            if (r == 0) throw std::runtime_error("zip: unexpected EOF in archive");
            got += static_cast<size_t>(r);
        }
        return out;
    }
};

// ---- Parsed archive (shared by reader and append) -------------------------

struct Archive {
    std::vector<EntryInfo> entries;
    uint64_t cd_offset = 0;      // start of central directory (== append point)
    std::string comment;
};

void parse_zip64_extra(const uint8_t* extra, size_t extra_len, EntryInfo& e,
                       bool need_ulen, bool need_clen, bool need_offset) {
    size_t i = 0;
    while (i + 4 <= extra_len) {
        uint16_t id = static_cast<uint16_t>(get_le(extra + i, 2));
        uint16_t sz = static_cast<uint16_t>(get_le(extra + i + 2, 2));
        size_t field = i + 4;
        if (id == ZIP64_EXTRA_ID) {
            size_t p = field;
            if (need_ulen && p + 8 <= field + sz) { e.uncompressed_size = get_le(extra + p, 8); p += 8; }
            if (need_clen && p + 8 <= field + sz) { e.compressed_size = get_le(extra + p, 8); p += 8; }
            if (need_offset && p + 8 <= field + sz) { e.header_offset = get_le(extra + p, 8); p += 8; }
        }
        i = field + sz;
    }
}

Archive parse_archive(const Source& src) {
    const uint64_t total = src.size();
    if (total < 22) throw std::runtime_error("zip: file too small to be a ZIP archive");

    // Locate the End Of Central Directory record by scanning the tail.
    const uint64_t max_tail = std::min<uint64_t>(total, 22 + 0xFFFF);
    std::vector<uint8_t> tail = src.read_at(total - max_tail, static_cast<size_t>(max_tail));
    ssize_t eocd = -1;
    for (ssize_t i = static_cast<ssize_t>(tail.size()) - 22; i >= 0; --i) {
        if (get_le(tail.data() + i, 4) == SIG_EOCD) { eocd = i; break; }
    }
    if (eocd < 0) throw std::runtime_error("zip: end-of-central-directory not found");

    const uint8_t* e = tail.data() + eocd;
    uint64_t entry_count = get_le(e + 10, 2);
    uint64_t cd_size = get_le(e + 12, 4);
    uint64_t cd_offset = get_le(e + 16, 4);
    uint16_t comment_len = static_cast<uint16_t>(get_le(e + 20, 2));

    Archive arc;
    if (comment_len > 0 && eocd + 22 + comment_len <= static_cast<ssize_t>(tail.size()))
        arc.comment.assign(reinterpret_cast<const char*>(e + 22), comment_len);

    // Zip64: resolve real values via the Zip64 EOCD locator if present.
    if (entry_count == 0xFFFF || cd_size == LOW32 || cd_offset == LOW32) {
        if (eocd >= 20 && get_le(tail.data() + eocd - 20, 4) == SIG_Z64_LOCATOR) {
            uint64_t z64_off = get_le(tail.data() + eocd - 20 + 8, 8);
            std::vector<uint8_t> z = src.read_at(z64_off, 56);
            if (get_le(z.data(), 4) != SIG_Z64_EOCD)
                throw std::runtime_error("zip: bad Zip64 EOCD signature");
            entry_count = get_le(z.data() + 32, 8);
            cd_size = get_le(z.data() + 40, 8);
            cd_offset = get_le(z.data() + 48, 8);
        }
    }
    arc.cd_offset = cd_offset;

    // Read and parse the central directory.
    std::vector<uint8_t> cd = src.read_at(cd_offset, static_cast<size_t>(cd_size));
    size_t pos = 0;
    for (uint64_t n = 0; n < entry_count; ++n) {
        if (pos + 46 > cd.size() || get_le(cd.data() + pos, 4) != SIG_CENTRAL)
            throw std::runtime_error("zip: corrupt central directory entry");
        const uint8_t* c = cd.data() + pos;
        EntryInfo info;
        uint16_t method = static_cast<uint16_t>(get_le(c + 10, 2));
        info.method = static_cast<Method>(method);
        uint32_t dostime = static_cast<uint32_t>(get_le(c + 12, 4));
        info.mtime = dos2time(dostime);
        info.crc32 = static_cast<uint32_t>(get_le(c + 16, 4));
        info.compressed_size = get_le(c + 20, 4);
        info.uncompressed_size = get_le(c + 24, 4);
        uint16_t name_len = static_cast<uint16_t>(get_le(c + 28, 2));
        uint16_t extra_len = static_cast<uint16_t>(get_le(c + 30, 2));
        uint16_t cmt_len = static_cast<uint16_t>(get_le(c + 32, 2));
        info.external_attr = static_cast<uint32_t>(get_le(c + 38, 4));
        info.header_offset = get_le(c + 42, 4);

        size_t name_off = pos + 46;
        if (name_off + name_len + extra_len + cmt_len > cd.size())
            throw std::runtime_error("zip: central directory entry overruns");
        info.name.assign(reinterpret_cast<const char*>(cd.data() + name_off), name_len);

        bool need_ulen = info.uncompressed_size == LOW32;
        bool need_clen = info.compressed_size == LOW32;
        bool need_offset = info.header_offset == LOW32;
        if (need_ulen || need_clen || need_offset)
            parse_zip64_extra(cd.data() + name_off + name_len, extra_len, info,
                              need_ulen, need_clen, need_offset);

        if (cmt_len > 0)
            info.comment.assign(reinterpret_cast<const char*>(cd.data() + name_off + name_len + extra_len),
                                cmt_len);

        info.is_dir = (!info.name.empty() && info.name.back() == '/');
        arc.entries.push_back(std::move(info));
        pos = name_off + name_len + extra_len + cmt_len;
    }
    return arc;
}

// Compute the offset of a member's compressed data by reading its local header.
uint64_t member_data_offset(const Source& src, const EntryInfo& e) {
    std::vector<uint8_t> lh = src.read_at(e.header_offset, 30);
    if (get_le(lh.data(), 4) != SIG_LOCAL)
        throw std::runtime_error("zip: bad local file header for '" + e.name + "'");
    uint16_t name_len = static_cast<uint16_t>(get_le(lh.data() + 26, 2));
    uint16_t extra_len = static_cast<uint16_t>(get_le(lh.data() + 28, 2));
    return e.header_offset + 30 + name_len + extra_len;
}

// ---- Local header / central directory serialization -----------------------

void build_zip64_extra(std::vector<uint8_t>& extra, bool ulen, uint64_t u,
                       bool clen, uint64_t c, bool offset, uint64_t o) {
    int count = (ulen ? 1 : 0) + (clen ? 1 : 0) + (offset ? 1 : 0);
    if (count == 0) return;
    put_le(extra, ZIP64_EXTRA_ID, 2);
    put_le(extra, count * 8, 2);
    if (ulen) put_le(extra, u, 8);
    if (clen) put_le(extra, c, 8);
    if (offset) put_le(extra, o, 8);
}

std::vector<uint8_t> build_local_header(const EntryInfo& e) {
    bool z64 = e.uncompressed_size >= LOW32 || e.compressed_size >= LOW32;
    std::vector<uint8_t> extra;
    if (z64) build_zip64_extra(extra, true, e.uncompressed_size, true, e.compressed_size, false, 0);

    std::vector<uint8_t> h;
    put_le(h, SIG_LOCAL, 4);
    put_le(h, z64 ? VER_ZIP64 : VER_DEFAULT, 2);
    put_le(h, FLAG_UTF8, 2);
    put_le(h, static_cast<uint16_t>(e.method), 2);
    put_le(h, time2dos(e.mtime), 4);
    put_le(h, e.crc32, 4);
    put_le(h, z64 ? LOW32 : e.compressed_size, 4);
    put_le(h, z64 ? LOW32 : e.uncompressed_size, 4);
    put_le(h, e.name.size(), 2);
    put_le(h, extra.size(), 2);
    h.insert(h.end(), e.name.begin(), e.name.end());
    h.insert(h.end(), extra.begin(), extra.end());
    return h;
}

std::vector<uint8_t> build_central_header(const EntryInfo& e) {
    bool z64_u = e.uncompressed_size >= LOW32;
    bool z64_c = e.compressed_size >= LOW32;
    bool z64_o = e.header_offset >= LOW32;
    bool z64 = z64_u || z64_c || z64_o;
    std::vector<uint8_t> extra;
    if (z64) build_zip64_extra(extra, z64_u, e.uncompressed_size, z64_c, e.compressed_size,
                               z64_o, e.header_offset);

    std::vector<uint8_t> h;
    put_le(h, SIG_CENTRAL, 4);
    put_le(h, VER_MADE_BY, 2);
    put_le(h, z64 ? VER_ZIP64 : VER_DEFAULT, 2);
    put_le(h, FLAG_UTF8, 2);
    put_le(h, static_cast<uint16_t>(e.method), 2);
    put_le(h, time2dos(e.mtime), 4);
    put_le(h, e.crc32, 4);
    put_le(h, z64_c ? LOW32 : e.compressed_size, 4);
    put_le(h, z64_u ? LOW32 : e.uncompressed_size, 4);
    put_le(h, e.name.size(), 2);
    put_le(h, extra.size(), 2);
    put_le(h, e.comment.size(), 2);
    put_le(h, 0, 2);                 // disk number start
    put_le(h, 0, 2);                 // internal attributes
    put_le(h, e.external_attr, 4);   // external attributes
    put_le(h, z64_o ? LOW32 : e.header_offset, 4);
    h.insert(h.end(), e.name.begin(), e.name.end());
    h.insert(h.end(), extra.begin(), extra.end());
    h.insert(h.end(), e.comment.begin(), e.comment.end());
    return h;
}

} // namespace

// ==========================================================================
// ZipWriter
// ==========================================================================

struct ZipWriter::Impl {
    Sink sink;
    std::vector<uint8_t> membuf;   // used when in_memory
    int owned_fd = -1;
    bool in_memory = false;
    bool finished = false;
    std::vector<EntryInfo> entries;
    std::string comment;

    void add(const std::string& name, const uint8_t* data, size_t size,
             const WriteOptions& opts, bool is_dir) {
        if (finished) throw std::runtime_error("zip: writer already finished");

        EntryInfo e;
        e.name = normalize_name(name);
        e.mtime = opts.mtime != 0 ? opts.mtime : now_seconds();
        e.header_offset = sink.offset;
        e.uncompressed_size = size;
        e.crc32 = crc_of(data, size);
        e.is_dir = is_dir;
        e.comment = opts.comment;
        e.external_attr = is_dir ? ((040755u << 16) | 0x10u) : (0100644u << 16);

        std::vector<uint8_t> payload;
        if (is_dir || size == 0 || opts.method == Method::Store) {
            e.method = Method::Store;
            e.compressed_size = size;
            if (size) payload.assign(data, data + size);
        } else {
            std::vector<uint8_t> deflated = raw_deflate(data, size, opts.level,
                                                        opts.threads, opts.engine);
            // Fall back to STORED if DEFLATE did not help.
            if (deflated.size() >= size) {
                e.method = Method::Store;
                e.compressed_size = size;
                payload.assign(data, data + size);
            } else {
                e.method = Method::Deflate;
                e.compressed_size = deflated.size();
                payload = std::move(deflated);
            }
        }

        std::vector<uint8_t> lh = build_local_header(e);
        sink.write(lh);
        if (!payload.empty()) sink.write(payload);
        entries.push_back(std::move(e));
    }

    std::vector<uint8_t> write_central() {
        uint64_t cd_offset = sink.offset;
        std::vector<uint8_t> cd;
        for (const auto& e : entries) {
            std::vector<uint8_t> ch = build_central_header(e);
            cd.insert(cd.end(), ch.begin(), ch.end());
        }
        sink.write(cd);
        uint64_t cd_size = cd.size();
        uint64_t count = entries.size();

        std::vector<uint8_t> end;
        bool z64 = count > 0xFFFF || cd_size >= LOW32 || cd_offset >= LOW32;
        if (z64) {
            uint64_t z64_off = sink.offset;
            put_le(end, SIG_Z64_EOCD, 4);
            put_le(end, 44, 8);              // size of remaining record
            put_le(end, VER_MADE_BY, 2);
            put_le(end, VER_ZIP64, 2);
            put_le(end, 0, 4);               // disk number
            put_le(end, 0, 4);               // disk with CD
            put_le(end, count, 8);           // entries this disk
            put_le(end, count, 8);           // total entries
            put_le(end, cd_size, 8);
            put_le(end, cd_offset, 8);

            put_le(end, SIG_Z64_LOCATOR, 4);
            put_le(end, 0, 4);               // disk with Zip64 EOCD
            put_le(end, z64_off, 8);
            put_le(end, 1, 4);               // total disks
        }
        put_le(end, SIG_EOCD, 4);
        put_le(end, 0, 2);                                  // disk number
        put_le(end, 0, 2);                                  // disk with CD
        put_le(end, count > 0xFFFF ? 0xFFFF : count, 2);    // entries this disk
        put_le(end, count > 0xFFFF ? 0xFFFF : count, 2);    // total entries
        put_le(end, cd_size >= LOW32 ? LOW32 : cd_size, 4);
        put_le(end, cd_offset >= LOW32 ? LOW32 : cd_offset, 4);
        put_le(end, comment.size(), 2);
        end.insert(end.end(), comment.begin(), comment.end());
        sink.write(end);

        finished = true;
        std::vector<uint8_t> result;
        if (in_memory) result.swap(membuf);
        if (owned_fd >= 0) { ::close(owned_fd); owned_fd = -1; }
        return result;
    }
};

ZipWriter::ZipWriter() : p_(std::make_unique<Impl>()) {
    p_->in_memory = true;
    p_->sink.buf = &p_->membuf;
}

ZipWriter::ZipWriter(const std::string& path, char mode) : p_(std::make_unique<Impl>()) {
    if (mode == 'a') {
        int fd = ::open(path.c_str(), O_RDWR);
        if (fd < 0)
            throw std::runtime_error("zip: cannot open '" + path + "' for append: "
                                     + std::strerror(errno));
        Source src;
        src.fd = fd;
        Archive arc = parse_archive(src);   // may throw on a corrupt/short file
        if (::ftruncate(fd, static_cast<off_t>(arc.cd_offset)) != 0) {
            ::close(fd);
            throw std::runtime_error("zip: ftruncate failed: " + std::string(std::strerror(errno)));
        }
        if (::lseek(fd, static_cast<off_t>(arc.cd_offset), SEEK_SET) < 0) {
            ::close(fd);
            throw std::runtime_error("zip: lseek failed: " + std::string(std::strerror(errno)));
        }
        p_->owned_fd = fd;
        p_->sink.fd = fd;
        p_->sink.offset = arc.cd_offset;
        p_->entries = std::move(arc.entries);
        p_->comment = std::move(arc.comment);
    } else {
        int fd = ::open(path.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
        if (fd < 0)
            throw std::runtime_error("zip: cannot create '" + path + "': "
                                     + std::strerror(errno));
        p_->owned_fd = fd;
        p_->sink.fd = fd;
    }
}

ZipWriter::~ZipWriter() {
    if (p_ && !p_->finished && p_->owned_fd >= 0) {
        // Best-effort finalize so a dropped file writer still yields a valid archive.
        try { p_->write_central(); } catch (...) {}
    }
    if (p_ && p_->owned_fd >= 0) ::close(p_->owned_fd);
}

void ZipWriter::write_bytes(const std::string& name, const uint8_t* data, size_t size,
                            const WriteOptions& opts) {
    p_->add(name, data, size, opts, /*is_dir=*/false);
}

void ZipWriter::write_str(const std::string& name, const std::string& data,
                          const WriteOptions& opts) {
    p_->add(name, reinterpret_cast<const uint8_t*>(data.data()), data.size(), opts, false);
}

void ZipWriter::write_file(const std::string& path, const std::string& arcname,
                           const WriteOptions& opts) {
    int fd = ::open(path.c_str(), O_RDONLY);
    if (fd < 0)
        throw std::runtime_error("zip: cannot open '" + path + "': " + std::strerror(errno));
    struct stat st{};
    if (::fstat(fd, &st) != 0) {
        ::close(fd);
        throw std::runtime_error("zip: fstat '" + path + "' failed");
    }
    std::vector<uint8_t> buf(static_cast<size_t>(st.st_size));
    size_t got = readn(fd, buf.data(), buf.size());
    ::close(fd);
    buf.resize(got);

    WriteOptions o = opts;
    if (o.mtime == 0) o.mtime = static_cast<int64_t>(st.st_mtime);
    std::string name = arcname.empty() ? path : arcname;
    p_->add(name, buf.data(), buf.size(), o, /*is_dir=*/false);
}

void ZipWriter::write_dir(const std::string& name, const WriteOptions& opts) {
    std::string n = normalize_name(name);
    if (n.empty()) throw std::runtime_error("zip: empty directory name");
    if (n.back() != '/') n += '/';
    p_->add(n, nullptr, 0, opts, /*is_dir=*/true);
}

void ZipWriter::set_comment(const std::string& comment) { p_->comment = comment; }

std::vector<uint8_t> ZipWriter::finish() { return p_->write_central(); }

void ZipWriter::close() { p_->write_central(); }

const std::vector<EntryInfo>& ZipWriter::entries() const { return p_->entries; }

// ==========================================================================
// ZipReader
// ==========================================================================

struct ZipReader::Impl {
    Source src;
    std::vector<uint8_t> owned;    // backing buffer for the in-memory variant
    int owned_fd = -1;
    Archive arc;
    std::unordered_map<std::string, size_t> index;

    void build_index() {
        for (size_t i = 0; i < arc.entries.size(); ++i)
            index[arc.entries[i].name] = i;
    }

    std::vector<uint8_t> read_entry(const EntryInfo& e) const {
        uint64_t data_off = member_data_offset(src, e);
        std::vector<uint8_t> comp = src.read_at(data_off, static_cast<size_t>(e.compressed_size));
        std::vector<uint8_t> out;
        if (e.method == Method::Store) {
            out = std::move(comp);
        } else if (e.method == Method::Deflate) {
            out = raw_inflate(comp.data(), comp.size(), e.uncompressed_size);
        } else {
            throw std::runtime_error("zip: unsupported compression method for '" + e.name + "'");
        }
        if (crc_of(out.data(), out.size()) != e.crc32)
            throw std::runtime_error("zip: CRC mismatch for '" + e.name + "'");
        return out;
    }
};

ZipReader::ZipReader(const std::string& path) : p_(std::make_unique<Impl>()) {
    int fd = ::open(path.c_str(), O_RDONLY);
    if (fd < 0)
        throw std::runtime_error("zip: cannot open '" + path + "': " + std::strerror(errno));
    p_->owned_fd = fd;
    p_->src.fd = fd;
    p_->arc = parse_archive(p_->src);
    p_->build_index();
}

ZipReader::ZipReader(std::vector<uint8_t> data) : p_(std::make_unique<Impl>()) {
    p_->owned = std::move(data);
    p_->src.buf = &p_->owned;
    p_->arc = parse_archive(p_->src);
    p_->build_index();
}

ZipReader::~ZipReader() {
    if (p_ && p_->owned_fd >= 0) ::close(p_->owned_fd);
}

const std::vector<EntryInfo>& ZipReader::entries() const { return p_->arc.entries; }

std::vector<std::string> ZipReader::namelist() const {
    std::vector<std::string> names;
    names.reserve(p_->arc.entries.size());
    for (const auto& e : p_->arc.entries) names.push_back(e.name);
    return names;
}

bool ZipReader::contains(const std::string& name) const {
    return p_->index.count(normalize_name(name)) > 0;
}

const EntryInfo* ZipReader::info(const std::string& name) const {
    auto it = p_->index.find(normalize_name(name));
    return it == p_->index.end() ? nullptr : &p_->arc.entries[it->second];
}

std::vector<uint8_t> ZipReader::read(const std::string& name) const {
    auto it = p_->index.find(normalize_name(name));
    if (it == p_->index.end())
        throw std::runtime_error("zip: no such member '" + name + "'");
    return p_->read_entry(p_->arc.entries[it->second]);
}

std::vector<uint8_t> ZipReader::read(size_t index) const {
    if (index >= p_->arc.entries.size())
        throw std::runtime_error("zip: member index out of range");
    return p_->read_entry(p_->arc.entries[index]);
}

std::string ZipReader::testzip() const {
    for (const auto& e : p_->arc.entries) {
        if (e.is_dir) continue;
        try {
            p_->read_entry(e);
        } catch (...) {
            return e.name;
        }
    }
    return {};
}

std::string ZipReader::extract(const std::string& name, const std::string& dest_dir) const {
    auto it = p_->index.find(normalize_name(name));
    if (it == p_->index.end())
        throw std::runtime_error("zip: no such member '" + name + "'");
    const EntryInfo& e = p_->arc.entries[it->second];
    std::string target = safe_join(dest_dir, e.name);

    if (e.is_dir) {
        make_dirs(target);
        return target;
    }
    size_t slash = target.find_last_of('/');
    if (slash != std::string::npos) make_dirs(target.substr(0, slash));

    std::vector<uint8_t> data = p_->read_entry(e);
    int fd = ::open(target.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0)
        throw std::runtime_error("zip: cannot write '" + target + "': " + std::strerror(errno));
    if (!data.empty()) writen(fd, data.data(), data.size());
    ::close(fd);
    return target;
}

void ZipReader::extractall(const std::string& dest_dir) const {
    for (const auto& e : p_->arc.entries) extract(e.name, dest_dir);
}

const std::string& ZipReader::comment() const { return p_->arc.comment; }

} // namespace pigzpp::zip
