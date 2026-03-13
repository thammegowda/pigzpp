// Simple compression/decompression benchmark.
// Generates synthetic data and measures throughput.

#include "compress.h"
#include "decompress.h"
#include "config.h"

#include <chrono>
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <unistd.h>

namespace fs = std::filesystem;
using namespace pigzpp;
using Clock = std::chrono::high_resolution_clock;

static std::string generate_data(size_t mb) {
    std::string data;
    data.reserve(mb * 1024 * 1024);
    for (size_t i = 0; data.size() < mb * 1024 * 1024; i++) {
        data += "Line " + std::to_string(i) + ": ";
        data += "The quick brown fox jumps over the lazy dog. ";
        data += "Pack my box with five dozen liquor jugs. ";
        data += std::to_string(i * 17 + 42) + "\n";
    }
    return data;
}

static void bench_compress(const std::string& data, int level, int procs) {
    std::filesystem::create_directories("tmp");
    char tmpl[] = "tmp/pigzpp_bench_XXXXXX";
    char* dir = mkdtemp(tmpl);
    std::string orig = std::string(dir) + "/data.txt";
    std::string gz = std::string(dir) + "/data.txt.gz";

    {
        std::ofstream f(orig, std::ios::binary);
        f.write(data.data(), data.size());
    }

    Config cfg;
    cfg.level = level;
    cfg.procs = procs;

    auto start = Clock::now();
    int in_fd = ::open(orig.c_str(), O_RDONLY);
    int out_fd = ::open(gz.c_str(), O_CREAT | O_TRUNC | O_WRONLY, 0600);
    Compressor comp(cfg);
    comp.compress(in_fd, out_fd);
    ::close(in_fd);
    ::close(out_fd);
    auto end = Clock::now();

    double secs = std::chrono::duration<double>(end - start).count();
    double mb = data.size() / (1024.0 * 1024.0);
    size_t gz_size = fs::file_size(gz);
    double ratio = 100.0 * (1.0 - static_cast<double>(gz_size) / data.size());

    printf("  compress   level=%2d procs=%d  %.1f MB in %.3fs = %.1f MB/s  ratio=%.1f%%\n",
           level, procs, mb, secs, mb / secs, ratio);

    // Decompress benchmark
    start = Clock::now();
    std::string restored = std::string(dir) + "/restored.txt";
    in_fd = ::open(gz.c_str(), O_RDONLY);
    out_fd = ::open(restored.c_str(), O_CREAT | O_TRUNC | O_WRONLY, 0600);
    Config dcfg;
    dcfg.mode = Mode::Decompress;
    Decompressor decomp(dcfg);
    decomp.decompress(in_fd, out_fd);
    ::close(in_fd);
    ::close(out_fd);
    end = Clock::now();

    secs = std::chrono::duration<double>(end - start).count();
    printf("  decompress                    %.1f MB in %.3fs = %.1f MB/s\n",
           mb, secs, mb / secs);

    fs::remove_all(dir);
}

int main() {
    printf("Generating 10MB of test data...\n");
    auto data = generate_data(10);
    printf("Data size: %.1f MB\n\n", data.size() / (1024.0 * 1024.0));

    printf("=== Single-threaded ===\n");
    bench_compress(data, 1, 1);
    bench_compress(data, 6, 1);
    bench_compress(data, 9, 1);

    printf("\n=== Parallel (2 threads) ===\n");
    bench_compress(data, 1, 2);
    bench_compress(data, 6, 2);
    bench_compress(data, 9, 2);

    int hw = std::thread::hardware_concurrency();
    if (hw > 2) {
        printf("\n=== Parallel (%d threads) ===\n", hw);
        bench_compress(data, 1, hw);
        bench_compress(data, 6, hw);
        bench_compress(data, 9, hw);
    }

    return 0;
}
