// pigzpp - CLI tool for parallel gzip compression
// Drop-in replacement for pigz with compatible CLI interface.

#include "pigzpp.h"
#include "platform.h"

#include <algorithm>
#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <iostream>
#include <signal.h>
#include <string>
#include <string_view>
#if !defined(_WIN32)
#include <sys/time.h>
#endif
#include <vector>

namespace fs = std::filesystem;

using namespace pigzpp;

#ifndef PIGZPP_VERSION
#define PIGZPP_VERSION "unknown"
#endif
static const char* VERSION_STR = "pigzpp " PIGZPP_VERSION " (based on pigz 2.8)";
static std::string g_outf; // for signal handler cleanup
static int g_outd = -1;

static void cut_short(int) {
    if (g_outd != -1 && g_outd != 1) {
        platform::unlink(g_outf.c_str());
        g_outd = -1;
    }
    std::_Exit(EINTR);
}

static void show_help() {
    std::cerr << R"(Usage: pigzpp [options] [files ...]
  will compress files in place, adding the suffix '.gz'. If no files are
  specified, stdin will be compressed to stdout. pigzpp does what gzip does,
  but spreads the work over multiple processors and cores when compressing.

Options:
  -0 to -9, -11        Compression level (level 11, zopfli, is much slower)
  --fast, --best        Compression levels 1 and 9 respectively
  -b, --blocksize mmm   Set compression block size to mmmK (default 128K)
  -c, --stdout          Write all processed output to stdout (won't delete)
  -C, --comment ccc     Put comment ccc in the gzip or zip header
  -d, --decompress      Decompress the compressed input
  -f, --force           Force overwrite, compress .gz, links, and to terminal
  -F  --first           Do iterations first, before block split for -11
  -h, --help            Display a help screen and quit
  -H, --huffman         Use only Huffman coding for compression
  -i, --independent     Compress blocks independently for damage recovery
  -I, --iterations n    Number of iterations for -11 optimization
  -J, --maxsplits n     Maximum number of split blocks for -11
  -k, --keep            Do not delete original file after processing
  -K, --zip             Compress to PKWare zip (.zip) single entry format
  -l, --list            List the contents of the compressed input
  -L, --license         Display the pigzpp license and quit
  -m, --no-time         Do not store or restore mod time
  -M, --time            Store or restore mod time
  -n, --no-name         Do not store or restore file name or mod time
  -N, --name            Store or restore file name and mod time
  -O  --oneblock        Do not split into smaller blocks for -11
  -p, --processes n     Allow up to n compression threads
  -E, --engine e        DEFLATE backend: auto (default), zlib, or isal
  -q, --quiet           Print no messages, even on error
  -r, --recursive       Process the contents of all subdirectories
  -R, --rsyncable       Input-determined block locations for rsync
  -S, --suffix .sss     Use suffix .sss instead of .gz (for compression)
  -t, --test            Test the integrity of the compressed input
  -U, --rle             Use run-length encoding for compression
  -v, --verbose         Provide more verbose output
  -V  --version         Show the version of pigzpp
  -Y  --synchronous     Force output file write to permanent storage
  -z, --zlib            Compress to zlib (.zz) instead of gzip format
  --                    All arguments after "--" are treated as files
)";
    std::exit(0);
}

// Map long options to short option characters.
static char long_to_short(std::string_view arg) {
    struct { const char* lng; char shrt; } map[] = {
        {"best", '9'}, {"fast", '1'}, {"blocksize", 'b'}, {"decompress", 'd'},
        {"force", 'f'}, {"comment", 'C'}, {"first", 'F'}, {"help", 'h'},
        {"huffman", 'H'}, {"independent", 'i'}, {"iterations", 'I'},
        {"maxsplits", 'J'}, {"keep", 'k'}, {"license", 'L'}, {"list", 'l'},
        {"name", 'N'}, {"no-name", 'n'}, {"no-time", 'm'}, {"time", 'M'},
        {"oneblock", 'O'}, {"processes", 'p'}, {"quiet", 'q'},
        {"recursive", 'r'}, {"rsyncable", 'R'}, {"stdout", 'c'},
        {"to-stdout", 'c'}, {"suffix", 'S'}, {"synchronous", 'Y'},
        {"test", 't'}, {"uncompress", 'd'}, {"verbose", 'v'},
        {"version", 'V'}, {"zip", 'K'}, {"zlib", 'z'}, {"rle", 'U'},
        {"silent", 'q'}, {"engine", 'E'},
    };
    for (auto& [l, s] : map)
        if (arg == l) return s;
    return 0;
}

// Parse a --engine value ("auto"/"zlib"/"isal") to the backend enum.
static Engine parse_engine_name(std::string_view s) {
    if (s == "auto") return Engine::Auto;
    if (s == "zlib" || s == "zlib-ng" || s == "zlibng") return Engine::Zlib;
    if (s == "isal" || s == "isa-l") return Engine::Isal;
    throw std::runtime_error("invalid --engine (use auto|zlib|isal)");
}

// Find compressed suffix length.
static size_t compressed_suffix(const std::string& name) {
    if (name.size() > 4) {
        auto s = name.substr(name.size() - 4);
        if (s == ".zip" || s == ".ZIP" || s == ".tgz") return 4;
    }
    if (name.size() > 3) {
        auto s = name.substr(name.size() - 3);
        if (s == ".gz" || s == "-gz" || s == ".zz" || s == "-zz") return 3;
    }
    if (name.size() > 2) {
        auto s = name.substr(name.size() - 2);
        if (s == ".z" || s == "-z" || s == "_z" || s == ".Z") return 2;
    }
    return 0;
}

// Get just the filename from a path.
static std::string justname(const std::string& path) {
    return fs::path(path).filename().string();
}

// Copy file metadata (permissions, timestamps).
static void copymeta(const std::string& from, const std::string& to) {
#if defined(_WIN32)
    std::error_code error;
    fs::permissions(to, fs::status(from, error).permissions(),
                    fs::perm_options::replace, error);
    error.clear();
    auto timestamp = fs::last_write_time(from, error);
    if (!error)
        fs::last_write_time(to, timestamp, error);
#else
    platform::FileStat st{};
    if (platform::stat(from.c_str(), &st) != 0) return;
    ::chmod(to.c_str(), st.st_mode & 07777);
    (void) ::chown(to.c_str(), st.st_uid, st.st_gid);
    struct timeval times[2] = {
        {st.st_atime, 0},
        {st.st_mtime, 0}
    };
    ::utimes(to.c_str(), times);
#endif
}

// Process a single file (or stdin if path is empty).
static void process(const std::string& path, Config& cfg) {
    std::string inf;
    int ind = -1;
    platform::FileStat st{};
    bool is_stdin = path.empty();

    if (is_stdin) {
        inf = "<stdin>";
        ind = 0;
        cfg.name.clear();
        if ((cfg.headis & 2) && platform::fstat(0, &st) == 0 && platform::is_regular(st))
            cfg.mtime = st.st_mtime;
        else
            cfg.mtime = 0;
    } else {
        inf = path;
        static const char* sufs[] = {".z", "-z", "_z", ".Z", ".gz", "-gz", ".zz", "-zz",
                                     ".zip", ".ZIP", ".tgz", nullptr};

        if (platform::lstat(inf.c_str(), &st) != 0) {
            if (errno == ENOENT && (cfg.mode == Mode::List || cfg.mode == Mode::Decompress || cfg.mode == Mode::Test)) {
                for (auto* s = sufs; *s; ++s) {
                    std::string try_name = path + *s;
                    if (platform::lstat(try_name.c_str(), &st) == 0) {
                        inf = try_name;
                        break;
                    }
                }
            }
            if (platform::lstat(inf.c_str(), &st) != 0) {
                if (cfg.verbosity > 0)
                    std::cerr << "pigzpp: skipping: " << inf << " does not exist\n";
                return;
            }
        }

        // Type checks
        if (!platform::is_regular(st) && !platform::is_fifo(st) &&
            !platform::is_symlink(st) && !platform::is_directory(st)) {
            if (cfg.verbosity > 0)
                std::cerr << "pigzpp: skipping: " << inf << " is a special file\n";
            return;
        }
        if (platform::is_symlink(st) && !cfg.force && !cfg.pipeout) {
            if (cfg.verbosity > 0)
                std::cerr << "pigzpp: skipping: " << inf << " is a symbolic link\n";
            return;
        }
        if (platform::is_directory(st)) {
            if (!cfg.recurse) {
                if (cfg.verbosity > 0)
                    std::cerr << "pigzpp: skipping: " << inf << " is a directory\n";
                return;
            }
            // Recurse
            for (const auto& entry : fs::directory_iterator(inf)) {
                process(entry.path().string(), cfg);
            }
            return;
        }

        // Skip already-compressed files
        if (cfg.mode == Mode::Compress && !cfg.force &&
            inf.size() >= cfg.sufx.size() &&
            inf.substr(inf.size() - cfg.sufx.size()) == cfg.sufx) {
            if (cfg.verbosity > 0)
                std::cerr << "pigzpp: skipping: " << inf << " ends with " << cfg.sufx << "\n";
            return;
        }

        // Check for compressed suffix when decompressing
        size_t suf_len = compressed_suffix(inf);
        if (cfg.mode == Mode::Decompress && !cfg.pipeout && suf_len == 0) {
            if (cfg.verbosity > 0)
                std::cerr << "pigzpp: skipping: " << inf << " does not have compressed suffix\n";
            return;
        }

        ind = platform::open(inf.c_str(), O_RDONLY);
        if (ind < 0) {
            if (cfg.verbosity > 0)
                std::cerr << "pigzpp: " << inf << ": " << strerror(errno) << "\n";
            return;
        }

        cfg.name = (cfg.headis & 1) ? justname(inf) : "";
        cfg.mtime = (cfg.headis & 2) ? st.st_mtime : 0;
    }

    // Determine output
    std::string outf;
    int outd = -1;

    if (is_stdin || cfg.pipeout) {
        outf = "<stdout>";
        outd = 1;
        if (cfg.mode == Mode::Compress && !cfg.force && platform::isatty(1)) {
            std::cerr << "pigzpp: refusing to write compressed data to terminal (use -f)\n";
            if (ind > 0) platform::close(ind);
            return;
        }
    } else if (cfg.mode == Mode::List || cfg.mode == Mode::Test) {
        outf = "";
        outd = -1;
    } else if (cfg.mode == Mode::Decompress) {
        size_t suf = compressed_suffix(inf);
        outf = inf.substr(0, inf.size() - suf);
        if (inf.substr(inf.size() - suf) == ".tgz")
            outf += ".tar";
        outd = platform::open(outf.c_str(), O_CREAT | O_TRUNC | O_WRONLY | (cfg.force ? 0 : O_EXCL), 0600);
        if (outd < 0 && errno == EEXIST) {
            if (cfg.verbosity > 0)
                std::cerr << "pigzpp: skipping: " << outf << " exists (use -f to overwrite)\n";
            if (ind > 0) platform::close(ind);
            return;
        }
        if (outd < 0) {
            std::cerr << "pigzpp: " << outf << ": " << strerror(errno) << "\n";
            if (ind > 0) platform::close(ind);
            return;
        }
    } else {
        // Compress
        outf = inf + cfg.sufx;
        outd = platform::open(outf.c_str(), O_CREAT | O_TRUNC | O_WRONLY | (cfg.force ? 0 : O_EXCL), 0600);
        if (outd < 0 && errno == EEXIST) {
            if (cfg.verbosity > 0)
                std::cerr << "pigzpp: skipping: " << outf << " exists (use -f to overwrite)\n";
            if (ind > 0) platform::close(ind);
            return;
        }
        if (outd < 0) {
            std::cerr << "pigzpp: " << outf << ": " << strerror(errno) << "\n";
            if (ind > 0) platform::close(ind);
            return;
        }
    }

    // Register for signal cleanup
    g_outf = outf;
    g_outd = outd;

    try {
        if (cfg.mode == Mode::Compress) {
            Compressor comp(cfg);
            comp.compress(ind, outd);
        } else if (cfg.mode == Mode::Decompress || cfg.mode == Mode::Test) {
            Decompressor decomp(cfg);
            decomp.decompress(ind, outd);
        } else if (cfg.mode == Mode::List) {
            Decompressor decomp(cfg);
            decomp.list(ind);
        }
    } catch (const std::exception& e) {
        std::cerr << "pigzpp: " << inf << ": " << e.what() << "\n";
        if (outd > 1) {
            platform::close(outd);
            platform::unlink(outf.c_str());
        }
        if (ind > 0) platform::close(ind);
        g_outd = -1;
        return;
    }

    // Clean up
    if (outd > 1) {
        if (cfg.sync) platform::sync(outd);
        platform::close(outd);
    }
    g_outd = -1;

    if (ind > 0) {
        platform::close(ind);
        if (!is_stdin && outd > 1) {
            copymeta(inf, outf);
            if (!cfg.keep)
                platform::unlink(inf.c_str());
        }
    }
}

int main(int argc, char** argv) {
    platform::set_binary(0);
    platform::set_binary(1);
    Config cfg;
    std::vector<std::string> files;

    signal(SIGINT, cut_short);

    // Check invoked name
    std::string prog = argv[0];
    auto slash = prog.rfind('/');
    if (slash != std::string::npos) prog = prog.substr(slash + 1);

    if (prog == "unpigzpp" || prog == "gunzip") {
        cfg.mode = Mode::Decompress;
        cfg.headis >>= 2;
    }
    if (prog.size() > 2 && prog.substr(prog.size() - 3) == "cat") {
        cfg.mode = Mode::Decompress;
        cfg.pipeout = true;
        cfg.headis >>= 2;
    }

    // Parse arguments
    bool end_opts = false;
    char need_param = 0; // expecting parameter for this option
    for (int i = 1; i < argc; i++) {
        std::string_view arg = argv[i];

        // Handle pending parameter
        if (need_param) {
            size_t n;
            switch (need_param) {
            case 'b':
                n = std::stoul(std::string(arg));
                cfg.block = n << 10;
                if (cfg.block < DICT_SIZE)
                    throw std::runtime_error("block size too small (must be >= 32K)");
                break;
            case 'p':
                cfg.procs = std::stoi(std::string(arg));
                if (cfg.procs < 1) throw std::runtime_error("invalid number of processes");
                break;
            case 'S':
                cfg.sufx = arg;
                break;
            case 'I':
                cfg.zopts.numiterations = std::stoi(std::string(arg));
                break;
            case 'J':
                cfg.zopts.blocksplittingmax = std::stoi(std::string(arg));
                break;
            case 'C':
                cfg.comment = arg;
                break;
            case 'A':
                cfg.alias = arg;
                break;
            case 'E':
                cfg.engine = parse_engine_name(arg);
                break;
            default: break;
            }
            need_param = 0;
            continue;
        }

        if (!end_opts && arg == "--") { end_opts = true; continue; }

        if (!end_opts && arg.size() > 1 && arg[0] == '-') {
            std::string_view opts = arg.substr(1);

            // Long option
            if (opts[0] == '-') {
                char c = long_to_short(opts.substr(1));
                if (c == 0) {
                    std::cerr << "pigzpp: invalid option: " << arg << "\n";
                    return 1;
                }
                opts = std::string_view(&c, 1);
            }

            for (size_t j = 0; j < opts.size(); j++) {
                // If waiting for a parameter, use remaining chars as the param
                if (need_param && j > 0) {
                    // Remaining chars in this option string are the parameter
                    std::string param_str(opts.substr(j));
                    // Simulate: set next arg to be this parameter
                    size_t n;
                    switch (need_param) {
                    case 'b':
                        n = std::stoul(param_str);
                        cfg.block = n << 10;
                        if (cfg.block < DICT_SIZE)
                            throw std::runtime_error("block size too small (must be >= 32K)");
                        break;
                    case 'p':
                        cfg.procs = std::stoi(param_str);
                        if (cfg.procs < 1) throw std::runtime_error("invalid number of processes");
                        break;
                    case 'S': cfg.sufx = param_str; break;
                    case 'I': cfg.zopts.numiterations = std::stoi(param_str); break;
                    case 'J': cfg.zopts.blocksplittingmax = std::stoi(param_str); break;
                    case 'C': cfg.comment = param_str; break;
                    case 'A': cfg.alias = param_str; break;
                    case 'E': cfg.engine = parse_engine_name(param_str); break;
                    default: break;
                    }
                    need_param = 0;
                    break; // done with this option string
                }

                char c = opts[j];
                switch (c) {
                case '0': case '1': case '2': case '3': case '4':
                case '5': case '6': case '7': case '8': case '9': {
                    int level = c - '0';
                    while (j + 1 < opts.size() && opts[j+1] >= '0' && opts[j+1] <= '9') {
                        level = level * 10 + opts[++j] - '0';
                    }
                    if (level == 10 || level > 11) {
                        std::cerr << "pigzpp: only levels 0..9 and 11 are allowed\n";
                        return 1;
                    }
                    cfg.level = level;
                    break;
                }
                case 'b': need_param = 'b'; break;
                case 'c': cfg.pipeout = true; break;
                case 'C': need_param = 'C'; break;
                case 'd': cfg.mode = Mode::Decompress; break;
                case 'E': need_param = 'E'; break;
                case 'f': cfg.force = true; break;
                case 'F': cfg.zopts.blocksplittinglast = 1; break;
                case 'h': show_help(); break;
                case 'H': cfg.strategy = Strategy::HuffmanOnly; break;
                case 'i': cfg.setdict = false; break;
                case 'I': need_param = 'I'; break;
                case 'J': need_param = 'J'; break;
                case 'k': cfg.keep = true; break;
                case 'K': cfg.form = Format::Zip; cfg.sufx = ".zip"; break;
                case 'l': cfg.mode = Mode::List; break;
                case 'L':
                    std::cout << VERSION_STR << "\n";
                    std::cout << "Based on pigz by Mark Adler. zlib license.\n";
                    return 0;
                case 'm': cfg.headis &= ~0xa; break;
                case 'M': cfg.headis |= 0xa; break;
                case 'n': cfg.headis = 0; break;
                case 'N': cfg.headis = 0xf; break;
                case 'O': cfg.zopts.blocksplitting = 0; break;
                case 'p': need_param = 'p'; break;
                case 'q': cfg.verbosity = 0; break;
                case 'r': cfg.recurse = true; break;
                case 'R': cfg.rsync = true; break;
                case 'S': need_param = 'S'; break;
                case 't': cfg.mode = Mode::Test; break;
                case 'U': cfg.strategy = Strategy::Rle; break;
                case 'v': cfg.verbosity++; break;
                case 'V':
                    std::cout << VERSION_STR << "\n";
                    return 0;
                case 'Y': cfg.sync = true; break;
                case 'z': cfg.form = Format::Zlib; cfg.sufx = ".zz"; break;
                case 'A': need_param = 'A'; break;
                default:
                    std::cerr << "pigzpp: invalid option: -" << c << "\n";
                    return 1;
                }
            }
        } else {
            if (arg == "-" && !end_opts)
                files.emplace_back();
            else
                files.emplace_back(arg);
        }
    }

    if (need_param) {
        std::cerr << "pigzpp: missing parameter\n";
        return 1;
    }

    // If no files and stdin is a terminal, show help
    if (files.empty() && isatty(cfg.mode != Mode::Compress ? 0 : 1) && argc < 2)
        show_help();

    try {
        if (files.empty()) {
            process("", cfg);
        } else {
            for (auto& f : files)
                process(f, cfg);
        }
    } catch (const std::exception& e) {
        std::cerr << "pigzpp: " << e.what() << "\n";
        return 1;
    }

    return 0;
}
