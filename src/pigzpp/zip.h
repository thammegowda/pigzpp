// Native ZIP archive support (multi-entry), modeled on Python's zipfile.
//
// Provides ZipWriter (create/append) and ZipReader (list/extract/test) for
// STORED and DEFLATE members, with Zip64 for large entries/archives. DEFLATE
// members reuse pigzpp's parallel compressor for high throughput on big files.
//
// Thread-safety: each ZipWriter/ZipReader instance is single-threaded from the
// caller's perspective (no shared mutable globals), but a member's DEFLATE step
// may itself use multiple worker threads internally.

#pragma once

#include "config.h"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace pigzpp::zip {

// Supported member compression methods (matches the ZIP spec values).
enum class Method : uint16_t {
    Store = 0,
    Deflate = 8,
};

// Metadata for a single archive member.
struct EntryInfo {
    std::string name;               // Archive path (uses '/' separators).
    uint64_t compressed_size = 0;
    uint64_t uncompressed_size = 0;
    uint32_t crc32 = 0;
    Method method = Method::Deflate;
    int64_t mtime = 0;              // Modification time (unix seconds).
    std::string comment;
    uint32_t external_attr = 0;     // Host attributes (unix mode in high 16 bits).
    uint64_t header_offset = 0;     // Offset of the local file header.
    bool is_dir = false;
};

// Options controlling how a member is added.
struct WriteOptions {
    Method method = Method::Deflate;
    int level = 6;                  // DEFLATE level 0-9 (ignored for Store).
    int threads = 0;                // 0 = auto (hardware concurrency).
    Engine engine = Engine::Auto;   // DEFLATE backend.
    int64_t mtime = 0;              // 0 = current time.
    std::string comment;
};

// Builds a ZIP archive incrementally. Streams local headers + member data to
// the sink as entries are added; the central directory is written on close().
class ZipWriter {
public:
    // In-memory writer: the archive is returned by finish().
    ZipWriter();

    // File writer. mode 'w' truncates/creates; mode 'a' appends to an existing
    // archive (its members are preserved and new ones added after them).
    explicit ZipWriter(const std::string& path, char mode = 'w');

    ~ZipWriter();

    ZipWriter(const ZipWriter&) = delete;
    ZipWriter& operator=(const ZipWriter&) = delete;

    // Add raw bytes / a string as a member.
    void write_bytes(const std::string& name, const uint8_t* data, size_t size,
                     const WriteOptions& opts = {});
    void write_str(const std::string& name, const std::string& data,
                   const WriteOptions& opts = {});

    // Add a file from disk (arcname defaults to path if empty).
    void write_file(const std::string& path, const std::string& arcname = {},
                    const WriteOptions& opts = {});

    // Add an explicit directory entry (name is normalized to end with '/').
    void write_dir(const std::string& name, const WriteOptions& opts = {});

    // Archive-level comment (stored in the end-of-central-directory record).
    void set_comment(const std::string& comment);

    // Finalize: write the central directory + EOCD. For the in-memory writer,
    // returns the complete archive bytes. For a file writer, flushes and closes.
    // After finish()/close() the writer must not be used again.
    std::vector<uint8_t> finish();
    void close();

    const std::vector<EntryInfo>& entries() const;

private:
    struct Impl;
    std::unique_ptr<Impl> p_;
};

// Opens a ZIP archive for reading (from a file path or an in-memory buffer).
class ZipReader {
public:
    explicit ZipReader(const std::string& path);
    explicit ZipReader(std::vector<uint8_t> data);
    ~ZipReader();

    ZipReader(const ZipReader&) = delete;
    ZipReader& operator=(const ZipReader&) = delete;

    const std::vector<EntryInfo>& entries() const;
    std::vector<std::string> namelist() const;
    bool contains(const std::string& name) const;
    const EntryInfo* info(const std::string& name) const;   // nullptr if absent

    // Extract and decompress a member to bytes.
    std::vector<uint8_t> read(const std::string& name) const;
    std::vector<uint8_t> read(size_t index) const;

    // Verify all member CRCs. Returns the name of the first corrupt entry,
    // or an empty string if the whole archive is intact.
    std::string testzip() const;

    // Extract a member / all members to a destination directory. Returns the
    // path written. Paths are sanitized to stay within dest_dir.
    std::string extract(const std::string& name, const std::string& dest_dir) const;
    void extractall(const std::string& dest_dir) const;

    // Archive-level comment.
    const std::string& comment() const;

private:
    struct Impl;
    std::unique_ptr<Impl> p_;
};

} // namespace pigzpp::zip
