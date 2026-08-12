// pigzpp configuration and options
// Replaces the global struct g from pigz.c with a thread-safe value type.

#pragma once

#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <string>
#include <thread>

extern "C" {
#include "zopfli.h"
}

namespace pigzpp {

// Compression format.
enum class Format : int {
    Gzip = 0,
    Zlib = 1,
    Zip = 2,
    Raw = 3,   // Bare DEFLATE stream, no header/trailer (used for ZIP members).
};

// Operation mode.
enum class Mode : int {
    Compress = 0,
    Decompress = 1,
    Test = 2,
    List = 3,
};

// Compression strategy (mirrors zlib).
enum class Strategy : int {
    Default = 0,      // Z_DEFAULT_STRATEGY
    Filtered = 1,     // Z_FILTERED
    HuffmanOnly = 2,  // Z_HUFFMAN_ONLY
    Rle = 3,          // Z_RLE
    Fixed = 4,        // Z_FIXED
};

// DEFLATE backend engine selection.
enum class Engine : int {
    Auto = 0,  // Pick the best available: ISA-L if built in, else zlib-ng.
    Zlib = 1,  // Force zlib-ng (higher ratio, slower per core).
    Isal = 2,  // Force ISA-L (faster, lower ratio); falls back to zlib-ng
               // when not compiled in.
};

// Sliding dictionary size for deflate.
inline constexpr size_t DICT_SIZE = 32768U;

// Default block size (128K).
inline constexpr size_t DEFAULT_BLOCK_SIZE = 131072UL;

// Rsyncable constants.
inline constexpr unsigned RSYNCBITS = 12;
inline constexpr unsigned RSYNCMASK = (1U << RSYNCBITS) - 1;
inline constexpr unsigned RSYNCHIT = RSYNCMASK >> 1;

// Input buffer sizes.
inline constexpr size_t BUF_SIZE = 32768;

// Initial pool count as a function of the number of processors.
inline constexpr int inbufs(int procs) { return (procs << 1) + 3; }

// Initial output pool size.
inline constexpr size_t outpool(size_t block) { return block + (block >> 4) + DICT_SIZE; }

// Configuration for compression/decompression operations.
// This is a value type — no global state. Pass by const-ref.
struct Config {
    // Compression level: 0-9, or 11 for zopfli.
    int level = -1; // Z_DEFAULT_COMPRESSION

    // Compression strategy.
    Strategy strategy = Strategy::Default;

    // DEFLATE backend engine (Auto picks ISA-L when compiled in).
    Engine engine = Engine::Auto;

    // Output format.
    Format form = Format::Gzip;

    // Operation mode.
    Mode mode = Mode::Compress;

    // Number of compression threads (>= 1).
    int procs = static_cast<int>(std::thread::hardware_concurrency());

    // Uncompressed input block size per thread (>= 32K).
    size_t block = DEFAULT_BLOCK_SIZE;

    // Use dictionary between blocks (default true; false = independent blocks).
    bool setdict = true;

    // Rsyncable block boundaries.
    bool rsync = false;

    // Keep original file after processing.
    bool keep = false;

    // Force overwrite, compress links, output to terminal.
    bool force = false;

    // Write output to stdout.
    bool pipeout = false;

    // Flush output to permanent storage.
    bool sync = false;

    // Recurse into directories.
    bool recurse = false;

    // Verbosity level: 0=quiet, 1=normal, 2=verbose.
    int verbosity = 1;

    // Header information control (bitmask: 1=name, 2=time).
    int headis = 3;

    // Suffix for compressed files.
    std::string sufx = ".gz";

    // Name to store in header.
    std::string name;

    // Comment to store in header.
    std::string comment;

    // Alias name for stdin in zip headers.
    std::string alias = "-";

    // Modification time to store in header (0 = not set).
    int64_t mtime = 0;

    // Zopfli options.
    ZopfliOptions zopts{};

    Config() {
        // Check env vars for thread limit: PIGZPP_THREADS, PIGZ_THREADS
        for (const char* var : {"PIGZPP_THREADS", "PIGZ_THREADS"}) {
            const char* val = std::getenv(var);
            if (val && val[0]) {
                int n = std::atoi(val);
                if (n > 0) { procs = n; break; }
            }
        }
        if (procs < 1) procs = 1;
        ZopfliInitOptions(&zopts);
    }
};

} // namespace pigzpp
