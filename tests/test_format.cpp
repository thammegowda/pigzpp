#include <gtest/gtest.h>
#include "format.h"
#include "config.h"
#include "io_utils.h"

#include <cstring>
#include <fcntl.h>
#include <unistd.h>
#include <filesystem>

namespace fs = std::filesystem;
using namespace pigzpp;

class FormatTest : public ::testing::Test {
protected:
    std::string tmp_dir;

    void SetUp() override {
        fs::create_directories("tmp");
        char tmpl[] = "tmp/pigzpp_fmt_XXXXXX";
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
};

TEST_F(FormatTest, GzipHeaderBasic) {
    Config cfg;
    cfg.form = Format::Gzip;
    cfg.name = "test.txt";
    cfg.mtime = 1700000000;

    auto p = path("header.gz");
    int fd = ::open(p.c_str(), O_CREAT | O_TRUNC | O_WRONLY, 0600);
    ASSERT_GE(fd, 0);
    size_t hlen = put_header(fd, cfg);
    ::close(fd);

    EXPECT_GT(hlen, 10u); // Gzip header is at least 10 bytes + name

    // Read back and check magic bytes
    fd = ::open(p.c_str(), O_RDONLY);
    unsigned char buf[64];
    size_t n = readn(fd, buf, hlen);
    ::close(fd);
    EXPECT_EQ(n, hlen);
    EXPECT_EQ(buf[0], 31);
    EXPECT_EQ(buf[1], 139);
    EXPECT_EQ(buf[2], 8); // deflate
}

TEST_F(FormatTest, ZlibHeaderBasic) {
    Config cfg;
    cfg.form = Format::Zlib;
    cfg.level = 6;

    auto p = path("header.zz");
    int fd = ::open(p.c_str(), O_CREAT | O_TRUNC | O_WRONLY, 0600);
    size_t hlen = put_header(fd, cfg);
    ::close(fd);

    EXPECT_EQ(hlen, 2u);

    fd = ::open(p.c_str(), O_RDONLY);
    unsigned char buf[2];
    readn(fd, buf, 2);
    ::close(fd);

    // Check zlib header: CMF=0x78, FCHECK makes it divisible by 31
    unsigned magic = (static_cast<unsigned>(buf[0]) << 8) + buf[1];
    EXPECT_EQ(magic % 31, 0u);
}

TEST_F(FormatTest, Time2DosRoundtrip) {
    int64_t t = 1700000000; // ~2023
    uint32_t dos = time2dos(t);
    EXPECT_NE(dos, 0u);
    int64_t back = dos2time(dos);
    // Should be within 2 seconds (DOS time has 2-second resolution)
    EXPECT_LE(std::abs(back - t), 2);
}

TEST_F(FormatTest, InputReaderGetHeader) {
    // Create a minimal gzip file using zlib and read the header back
    auto p = path("test.gz");

    // Use system gzip to create a valid file
    auto orig = path("test.txt");
    {
        int fd = ::open(orig.c_str(), O_CREAT | O_TRUNC | O_WRONLY, 0600);
        const char* data = "test data\n";
        ::write(fd, data, 10);
        ::close(fd);
    }
    std::string cmd = "gzip -c " + orig + " > " + p;
    ASSERT_EQ(system(cmd.c_str()), 0);

    int fd = ::open(p.c_str(), O_RDONLY);
    ASSERT_GE(fd, 0);

    InputReader reader(fd);
    reader.reset();
    auto hdr = reader.get_header(true);
    ::close(fd);

    EXPECT_EQ(hdr.method, 8); // deflate
    EXPECT_EQ(hdr.form, Format::Gzip);
}
