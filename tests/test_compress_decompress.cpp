#include <gtest/gtest.h>
#include "compress.h"
#include "decompress.h"
#include "config.h"

#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>
#include <fcntl.h>
#include <unistd.h>

namespace fs = std::filesystem;
using namespace pigzpp;

class CompressDecompressTest : public ::testing::Test {
protected:
    std::string tmp_dir;

    void SetUp() override {
        fs::create_directories("tmp");
        char tmpl[] = "tmp/pigzpp_test_XXXXXX";
        char* dir = mkdtemp(tmpl);
        ASSERT_NE(dir, nullptr);
        tmp_dir = dir;
    }

    void TearDown() override {
        fs::remove_all(tmp_dir);
    }

    std::string path(const std::string& name) {
        return tmp_dir + "/" + name;
    }

    void write_file(const std::string& p, const std::string& content) {
        std::ofstream f(p, std::ios::binary);
        f.write(content.data(), content.size());
    }

    std::string read_file_content(const std::string& p) {
        std::ifstream f(p, std::ios::binary);
        return {std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>()};
    }

    void roundtrip(const std::string& data, int level = 6, int procs = 1) {
        auto orig = path("orig.txt");
        auto gz = path("orig.txt.gz");
        auto restored = path("restored.txt");
        write_file(orig, data);

        // Compress
        Config cfg;
        cfg.level = level;
        cfg.procs = procs;
        int in_fd = ::open(orig.c_str(), O_RDONLY);
        int out_fd = ::open(gz.c_str(), O_CREAT | O_TRUNC | O_WRONLY, 0600);
        ASSERT_GE(in_fd, 0);
        ASSERT_GE(out_fd, 0);
        Compressor comp(cfg);
        comp.compress(in_fd, out_fd);
        ::close(in_fd);
        ::close(out_fd);

        // Decompress
        in_fd = ::open(gz.c_str(), O_RDONLY);
        out_fd = ::open(restored.c_str(), O_CREAT | O_TRUNC | O_WRONLY, 0600);
        ASSERT_GE(in_fd, 0);
        ASSERT_GE(out_fd, 0);
        Config dcfg;
        dcfg.mode = Mode::Decompress;
        Decompressor decomp(dcfg);
        decomp.decompress(in_fd, out_fd);
        ::close(in_fd);
        ::close(out_fd);

        EXPECT_EQ(read_file_content(restored), data);
    }
};

TEST_F(CompressDecompressTest, EmptyFile) {
    roundtrip("");
}

TEST_F(CompressDecompressTest, SmallData) {
    roundtrip("Hello, World!\nThis is a test.\n");
}

TEST_F(CompressDecompressTest, Level0) {
    roundtrip("Store this uncompressed data block.", 0);
}

TEST_F(CompressDecompressTest, Level1) {
    roundtrip("Quick compression test data string here.", 1);
}

TEST_F(CompressDecompressTest, Level9) {
    roundtrip("Best compression level test data string.", 9);
}

TEST_F(CompressDecompressTest, LargerData) {
    // Generate ~200KB of data
    std::string data;
    for (int i = 0; i < 5000; i++)
        data += "Line " + std::to_string(i) + ": some repetitive test data for compression.\n";
    roundtrip(data);
}

TEST_F(CompressDecompressTest, ParallelSmall) {
    roundtrip("Small data for parallel test.", 6, 2);
}

TEST_F(CompressDecompressTest, ParallelLarger) {
    std::string data;
    for (int i = 0; i < 10000; i++)
        data += "Line " + std::to_string(i) + ": parallel compression test data.\n";
    roundtrip(data, 6, 4);
}

TEST_F(CompressDecompressTest, CrossCompatGzipDecompress) {
    // Compress with pigzpp, decompress with system gzip
    auto orig = path("orig.txt");
    auto gz = path("orig.txt.gz");
    auto restored = path("restored.txt");

    std::string data = "Cross-compatibility test: compress with pigzpp, decompress with gzip.\n";
    for (int i = 0; i < 1000; i++) data += "Line " + std::to_string(i) + "\n";
    write_file(orig, data);

    Config cfg;
    cfg.level = 6;
    int in_fd = ::open(orig.c_str(), O_RDONLY);
    int out_fd = ::open(gz.c_str(), O_CREAT | O_TRUNC | O_WRONLY, 0600);
    Compressor comp(cfg);
    comp.compress(in_fd, out_fd);
    ::close(in_fd);
    ::close(out_fd);

    // Use system gzip to decompress
    std::string cmd = "gzip -d -c " + gz + " > " + restored;
    int ret = system(cmd.c_str());
    EXPECT_EQ(ret, 0);
    EXPECT_EQ(read_file_content(restored), data);
}

TEST_F(CompressDecompressTest, CrossCompatGzipCompress) {
    // Compress with system gzip, decompress with pigzpp
    auto orig = path("orig.txt");
    auto gz = path("orig.txt.gz");
    auto restored = path("restored.txt");

    std::string data = "Cross-compatibility test: compress with gzip, decompress with pigzpp.\n";
    for (int i = 0; i < 1000; i++) data += "Line " + std::to_string(i) + "\n";
    write_file(orig, data);

    std::string cmd = "gzip -c " + orig + " > " + gz;
    int ret = system(cmd.c_str());
    ASSERT_EQ(ret, 0);

    int in_fd = ::open(gz.c_str(), O_RDONLY);
    int out_fd = ::open(restored.c_str(), O_CREAT | O_TRUNC | O_WRONLY, 0600);
    Config dcfg;
    dcfg.mode = Mode::Decompress;
    Decompressor decomp(dcfg);
    decomp.decompress(in_fd, out_fd);
    ::close(in_fd);
    ::close(out_fd);

    EXPECT_EQ(read_file_content(restored), data);
}

TEST_F(CompressDecompressTest, TestMode) {
    auto orig = path("orig.txt");
    auto gz = path("orig.txt.gz");
    write_file(orig, "Test integrity check data.\n");

    Config cfg;
    int in_fd = ::open(orig.c_str(), O_RDONLY);
    int out_fd = ::open(gz.c_str(), O_CREAT | O_TRUNC | O_WRONLY, 0600);
    Compressor comp(cfg);
    comp.compress(in_fd, out_fd);
    ::close(in_fd);
    ::close(out_fd);

    in_fd = ::open(gz.c_str(), O_RDONLY);
    Config tcfg;
    tcfg.mode = Mode::Test;
    Decompressor decomp(tcfg);
    EXPECT_NO_THROW(decomp.decompress(in_fd, -1));
    ::close(in_fd);
}

// Test thread safety: multiple Compressor/Decompressor instances in parallel
TEST_F(CompressDecompressTest, ThreadSafety) {
    constexpr int N = 4;
    std::vector<std::thread> threads;

    for (int i = 0; i < N; i++) {
        threads.emplace_back([this, i] {
            std::string data = "Thread " + std::to_string(i) + " data.\n";
            for (int j = 0; j < 1000; j++)
                data += "Line " + std::to_string(j) + "\n";

            auto orig = path("thread_" + std::to_string(i) + ".txt");
            auto gz = path("thread_" + std::to_string(i) + ".txt.gz");
            auto restored = path("thread_" + std::to_string(i) + "_restored.txt");
            write_file(orig, data);

            Config cfg;
            cfg.procs = 1; // single-thread per instance for this test

            int in_fd = ::open(orig.c_str(), O_RDONLY);
            int out_fd = ::open(gz.c_str(), O_CREAT | O_TRUNC | O_WRONLY, 0600);
            Compressor comp(cfg);
            comp.compress(in_fd, out_fd);
            ::close(in_fd);
            ::close(out_fd);

            in_fd = ::open(gz.c_str(), O_RDONLY);
            out_fd = ::open(restored.c_str(), O_CREAT | O_TRUNC | O_WRONLY, 0600);
            Config dcfg;
            dcfg.mode = Mode::Decompress;
            Decompressor decomp(dcfg);
            decomp.decompress(in_fd, out_fd);
            ::close(in_fd);
            ::close(out_fd);

            EXPECT_EQ(read_file_content(restored), data);
        });
    }

    for (auto& t : threads) t.join();
}
