#include <gtest/gtest.h>

#include "zip.h"

#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <limits>
#include <random>
#include <string>
#include <vector>

namespace fs = std::filesystem;
using namespace pigzpp::zip;
using pigzpp::Engine;

namespace {

std::string as_str(const std::vector<uint8_t>& v) {
    return {v.begin(), v.end()};
}

// Semi-compressible payload of the requested size.
std::vector<uint8_t> make_payload(size_t n, unsigned seed = 1) {
    std::vector<uint8_t> v(n);
    std::mt19937 rng(seed);
    static const char* words = "the quick brown fox jumps over a lazy dog ";
    size_t wl = std::strlen(words);
    for (size_t i = 0; i < n; ++i)
        v[i] = static_cast<uint8_t>(words[(i + rng() % 3) % wl]);
    return v;
}

void set_le(std::vector<uint8_t>& data, size_t offset, uint64_t value, size_t width) {
    for (size_t byte_index = 0; byte_index < width; ++byte_index)
        data[offset + byte_index] = static_cast<uint8_t>(value >> (8 * byte_index));
}

} // namespace

class ZipTest : public ::testing::Test {
protected:
    std::string dir;
    void SetUp() override {
        fs::create_directories("tmp");
        char tmpl[] = "tmp/pigzpp_zip_XXXXXX";
        char* d = mkdtemp(tmpl);
        ASSERT_NE(d, nullptr);
        dir = d;
    }
    void TearDown() override { fs::remove_all(dir); }
    std::string path(const std::string& n) { return dir + "/" + n; }
};

TEST_F(ZipTest, InMemoryRoundTrip) {
    ZipWriter w;
    w.write_str("a.txt", "hello world");
    w.write_str("b/c.txt", "nested content here");
    std::vector<uint8_t> archive = w.finish();
    ASSERT_FALSE(archive.empty());

    ZipReader r(archive);
    EXPECT_EQ(r.entries().size(), 2u);
    EXPECT_TRUE(r.contains("a.txt"));
    EXPECT_TRUE(r.contains("b/c.txt"));
    EXPECT_FALSE(r.contains("missing"));
    EXPECT_EQ(as_str(r.read("a.txt")), "hello world");
    EXPECT_EQ(as_str(r.read("b/c.txt")), "nested content here");
    EXPECT_TRUE(r.testzip().empty());
}

TEST_F(ZipTest, StoreMethodKeepsBytes) {
    ZipWriter w;
    WriteOptions st;
    st.method = Method::Store;
    w.write_str("plain.txt", "no compression", st);
    auto archive = w.finish();

    ZipReader r(archive);
    const EntryInfo* info = r.info("plain.txt");
    ASSERT_NE(info, nullptr);
    EXPECT_EQ(info->method, Method::Store);
    EXPECT_EQ(info->compressed_size, info->uncompressed_size);
    EXPECT_EQ(as_str(r.read("plain.txt")), "no compression");
}

TEST_F(ZipTest, DeflateActuallyCompresses) {
    auto payload = make_payload(500'000);
    ZipWriter w;
    WriteOptions opt;
    opt.method = Method::Deflate;
    opt.level = 6;
    w.write_bytes("big.bin", payload.data(), payload.size(), opt);
    auto archive = w.finish();

    ZipReader r(archive);
    const EntryInfo* info = r.info("big.bin");
    ASSERT_NE(info, nullptr);
    EXPECT_EQ(info->method, Method::Deflate);
    EXPECT_LT(info->compressed_size, info->uncompressed_size);
    EXPECT_EQ(r.read("big.bin"), payload);
}

TEST_F(ZipTest, EmptyMemberAndDirectory) {
    ZipWriter w;
    w.write_str("empty.txt", "");
    w.write_dir("subdir");
    auto archive = w.finish();

    ZipReader r(archive);
    EXPECT_EQ(as_str(r.read("empty.txt")), "");
    const EntryInfo* d = r.info("subdir/");
    ASSERT_NE(d, nullptr);
    EXPECT_TRUE(d->is_dir);
    EXPECT_EQ(d->uncompressed_size, 0u);
}

TEST_F(ZipTest, ParallelEngineRoundTrip) {
    auto payload = make_payload(4'000'000, 7);
    for (Engine eng : {Engine::Auto, Engine::Zlib, Engine::Isal}) {
        ZipWriter w;
        WriteOptions opt;
        opt.method = Method::Deflate;
        opt.threads = 4;
        opt.engine = eng;
        w.write_bytes("data.bin", payload.data(), payload.size(), opt);
        auto archive = w.finish();
        ZipReader r(archive);
        EXPECT_EQ(r.read("data.bin"), payload)
            << "engine=" << static_cast<int>(eng);
        EXPECT_TRUE(r.testzip().empty());
    }
}

TEST_F(ZipTest, FileWriteAndRead) {
    std::string zp = path("out.zip");
    {
        ZipWriter w(zp);
        w.write_str("one.txt", "first");
        w.write_str("two.txt", "second");
        w.set_comment("archive comment");
        w.close();
    }
    ZipReader r(zp);
    EXPECT_EQ(r.entries().size(), 2u);
    EXPECT_EQ(as_str(r.read("one.txt")), "first");
    EXPECT_EQ(as_str(r.read("two.txt")), "second");
    EXPECT_EQ(r.comment(), "archive comment");
}

TEST_F(ZipTest, AppendPreservesExisting) {
    std::string zp = path("app.zip");
    {
        ZipWriter w(zp);
        w.write_str("orig.txt", "original data");
        w.close();
    }
    {
        ZipWriter w(zp, 'a');
        w.write_str("new.txt", "appended data");
        w.close();
    }
    ZipReader r(zp);
    EXPECT_EQ(r.entries().size(), 2u);
    EXPECT_EQ(as_str(r.read("orig.txt")), "original data");
    EXPECT_EQ(as_str(r.read("new.txt")), "appended data");
    EXPECT_TRUE(r.testzip().empty());
}

TEST_F(ZipTest, ExtractAllToDisk) {
    ZipWriter w;
    w.write_str("top.txt", "top");
    w.write_str("sub/inner.txt", "inner");
    auto archive = w.finish();

    ZipReader r(archive);
    std::string out = path("extracted");
    r.extractall(out);
    EXPECT_TRUE(fs::exists(out + "/top.txt"));
    EXPECT_TRUE(fs::exists(out + "/sub/inner.txt"));
    std::ifstream f(out + "/sub/inner.txt");
    std::string content((std::istreambuf_iterator<char>(f)), {});
    EXPECT_EQ(content, "inner");
}

TEST_F(ZipTest, RejectsPathTraversalOnExtract) {
    ZipWriter w;
    w.write_str("../evil.txt", "nope");
    auto archive = w.finish();
    ZipReader r(archive);
    // Name is normalized on write (leading slashes stripped) but ".." stays;
    // extraction must refuse to escape the destination directory.
    EXPECT_THROW(r.extractall(path("safe")), std::runtime_error);
}

TEST_F(ZipTest, DetectsCorruption) {
    auto payload = make_payload(50'000, 3);
    ZipWriter w;
    w.write_bytes("d.bin", payload.data(), payload.size());
    auto archive = w.finish();
    // Flip a byte well inside the member's DEFLATE stream (local header is 30
    // bytes + a 5-byte name), leaving the trailing central directory intact.
    archive[100] ^= 0xFF;
    ZipReader r(archive);
    EXPECT_FALSE(r.testzip().empty());
}

TEST_F(ZipTest, RejectsOverflowingZip64Offset) {
    std::vector<uint8_t> archive(42);
    set_le(archive, 0, 0x07064b50, 4);
    set_le(archive, 8, std::numeric_limits<uint64_t>::max() - 55, 8);
    set_le(archive, 20, 0x06054b50, 4);
    set_le(archive, 30, 0xffff, 2);

    EXPECT_THROW(ZipReader reader(std::move(archive)), std::runtime_error);
}
