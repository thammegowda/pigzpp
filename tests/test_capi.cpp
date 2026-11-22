#include <gtest/gtest.h>
#include "capi.h"
#include "platform.h"
#include "test_support.h"
#include "zip.h"

#include <zlib.h>

#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <system_error>
#include <vector>

namespace {

namespace fs = std::filesystem;

struct ProgressSample {
    uint64_t total, read, written, elapsed_ms;
};
std::vector<ProgressSample> g_samples;

void record_progress(uint64_t total, uint64_t read, uint64_t written, uint64_t elapsed_ms) {
    g_samples.push_back({total, read, written, elapsed_ms});
}

class CapiFileTest : public ::testing::Test {
protected:
    std::string dir;
    void SetUp() override { dir = test_support::make_temp_dir("pigzpp_capi_"); g_samples.clear(); }
    void TearDown() override { fs::remove_all(dir); }

    static void write_bytes(const fs::path& p, const std::string& s) {
        std::ofstream f(p, std::ios::binary);
        f.write(s.data(), static_cast<std::streamsize>(s.size()));
    }
    static std::vector<uint8_t> read_bytes(const fs::path& p) {
        std::ifstream f(p, std::ios::binary);
        return {std::istreambuf_iterator<char>(f), {}};
    }
};

TEST_F(CapiFileTest, FormatsAndProgressRoundTrip) {
    const auto in = (fs::path(dir) / "input.txt").string();
    const auto out = (fs::path(dir) / "input.out").string();
    for (int format : {PIGZPP_FORMAT_GZIP, PIGZPP_FORMAT_ZLIB, PIGZPP_FORMAT_ZIP, PIGZPP_FORMAT_RAW}) {
        for (int threads : {1, 2}) {
            for (const std::string& data : {std::string(), std::string(300 * 1024, 'x'),
                                            std::string("binary\0payload", 14)}) {
                g_samples.clear();
                write_bytes(in, data);
                const char* err = pigzpp_compress_file(
                    in.c_str(), out.c_str(), 1, 6, threads, format, record_progress);
                ASSERT_EQ(err, nullptr) << (err ? err : "");
                auto compressed = read_bytes(out);

                ASSERT_FALSE(g_samples.empty());
                EXPECT_EQ(g_samples.back().total, data.size());
                EXPECT_EQ(g_samples.back().read, data.size());
                EXPECT_EQ(g_samples.back().written, compressed.size());

                if (format == PIGZPP_FORMAT_ZIP) {
                    pigzpp::zip::ZipReader zr(compressed);
                    auto restored = zr.read("input.txt");
                    EXPECT_EQ(std::string(restored.begin(), restored.end()), data);
                } else if (format == PIGZPP_FORMAT_RAW) {
                    z_stream zs{};
                    ASSERT_EQ(inflateInit2(&zs, -MAX_WBITS), Z_OK);
                    std::vector<uint8_t> restored(data.size() + 1);
                    zs.next_in = compressed.data();
                    zs.avail_in = static_cast<uInt>(compressed.size());
                    zs.next_out = restored.data();
                    zs.avail_out = static_cast<uInt>(restored.size());
                    EXPECT_EQ(inflate(&zs, Z_FINISH), Z_STREAM_END);
                    EXPECT_EQ(std::string(restored.begin(), restored.begin() + zs.total_out), data);
                    inflateEnd(&zs);
                } else {
                    auto dec = pigzpp_gzip_decompress(compressed.data(), compressed.size(), 1);
                    ASSERT_EQ(dec.error, nullptr);
                    EXPECT_EQ(std::string(reinterpret_cast<const char*>(dec.data), dec.size), data);
                    pigzpp_free(dec);

                    std::string dst(data.size(), '\0');
                    auto* bytes = dst.empty() ? nullptr : reinterpret_cast<uint8_t*>(dst.data());
                    size_t written = 0;
                    ASSERT_EQ(pigzpp_gzip_decompress_into(compressed.data(), compressed.size(),
                        bytes, dst.size(), &written), PIGZPP_DECOMPRESS_OK);
                    EXPECT_EQ(written, data.size());
                    EXPECT_EQ(dst, data);
                }
            }
        }
    }
}

TEST(CapiBufferTest, DecompressIntoReportsCapacityAndInvalidBuffers) {
    const uint8_t input[] = {'a', 'b', 'c'};
    auto gz = pigzpp_gzip_compress(input, sizeof(input), 6, 1, PIGZPP_ENGINE_AUTO);
    ASSERT_EQ(gz.error, nullptr);
    uint8_t dst[2]{};
    size_t written = 99;
    EXPECT_EQ(pigzpp_gzip_decompress_into(gz.data, gz.size, dst, sizeof(dst), &written),
              PIGZPP_DECOMPRESS_OUTPUT_TOO_SMALL);
    EXPECT_EQ(pigzpp_gzip_decompress_into(gz.data, gz.size, nullptr, sizeof(dst), &written),
              PIGZPP_DECOMPRESS_ERROR);
    EXPECT_EQ(pigzpp_gzip_decompress_into(gz.data, gz.size, gz.data, gz.size, &written),
              PIGZPP_DECOMPRESS_ERROR);
    EXPECT_EQ(written, 99u);
    pigzpp_free(gz);
}

TEST_F(CapiFileTest, ValidationAndOverwriteSafety) {
    const auto in = fs::path(dir) / "input.txt", out = fs::path(dir) / "output.gz";
    write_bytes(in, "preserve this input");
    write_bytes(out, "preserved");

    // Buffer + file level/format validation (must fail fast, never hang).
    for (int bad_lvl : {-2, 10, 99}) {
        auto buf = pigzpp_gzip_compress(reinterpret_cast<const uint8_t*>("abc"), 3, bad_lvl, 4,
                                        PIGZPP_ENGINE_ZLIB);
        EXPECT_EQ(buf.data, nullptr);
        EXPECT_NE(buf.error, nullptr);
        EXPECT_STREQ(pigzpp_compress_file(in.string().c_str(), out.string().c_str(), 1, bad_lvl, 2,
                                          PIGZPP_FORMAT_GZIP, nullptr),
                     "pigzpp: invalid compression level");
    }
    EXPECT_STREQ(pigzpp_compress_file(nullptr, out.string().c_str(), 1, 6, 1, PIGZPP_FORMAT_GZIP, nullptr),
                 "pigzpp: null filename");
    EXPECT_STREQ(pigzpp_compress_file(in.string().c_str(), out.string().c_str(), 1, 6, 1, -1, nullptr),
                 "pigzpp: invalid compression format");
    auto kept = read_bytes(out);
    EXPECT_EQ(std::string(kept.begin(), kept.end()), "preserved");

    // Existing file rejected when overwrite=0, replaced when overwrite=1.
    EXPECT_NE(pigzpp_compress_file(in.string().c_str(), out.string().c_str(), 0, 6, 1,
                                   PIGZPP_FORMAT_GZIP, nullptr), nullptr);
    EXPECT_EQ(pigzpp_compress_file(in.string().c_str(), out.string().c_str(), 1, 6, 1,
                                   PIGZPP_FORMAT_GZIP, nullptr), nullptr);

    // Same file, hard link, and symlink to input must all be rejected without clobbering input.
    EXPECT_STREQ(pigzpp_compress_file(in.string().c_str(), in.string().c_str(), 1, 6, 1,
                                      PIGZPP_FORMAT_GZIP, nullptr),
                 "pigzpp: input and output are the same file");
    const auto hlink = fs::path(dir) / "hardlink.gz";
    fs::create_hard_link(in, hlink);
    EXPECT_STREQ(pigzpp_compress_file(in.string().c_str(), hlink.string().c_str(), 1, 6, 1,
                                      PIGZPP_FORMAT_GZIP, nullptr),
                 "pigzpp: input and output are the same file");
    std::error_code ec;
    const auto slink = fs::path(dir) / "symlink.gz";
    fs::create_symlink(in.filename(), slink, ec);
    if (!ec) {
        EXPECT_STREQ(pigzpp_compress_file(in.string().c_str(), slink.string().c_str(), 1, 6, 1,
                                          PIGZPP_FORMAT_GZIP, nullptr),
                     "pigzpp: input and output are the same file");
    }
    auto intact = read_bytes(in);
    EXPECT_EQ(std::string(intact.begin(), intact.end()), "preserve this input");
}

TEST_F(CapiFileTest, Utf8PathsAndZipEntryName) {
    const auto udir = fs::path(dir) / fs::path(u8"\u76ee\u5f55");
    fs::create_directory(udir);
    const auto in = udir / fs::path(u8"\u6587\u4ef6-\U0001f600.txt");
    const auto zip_out = udir / fs::path(u8"\u538b\u7f29.zip");
    auto to_utf8 = [](const fs::path& p) {
        auto u8 = p.u8string();
        return std::string(u8.begin(), u8.end());
    };
    write_bytes(in, "unicode payload");

    ASSERT_EQ(pigzpp_compress_file(to_utf8(in).c_str(), to_utf8(zip_out).c_str(),
                                   0, 6, 1, PIGZPP_FORMAT_ZIP, nullptr), nullptr);
    auto zip_bytes = read_bytes(zip_out);
    ASSERT_GE(zip_bytes.size(), 8u);
    EXPECT_NE(static_cast<uint16_t>(zip_bytes[6] | (zip_bytes[7] << 8)) & 0x0800u, 0u);
    pigzpp::zip::ZipReader zr(zip_bytes);
    const std::string expected_name = to_utf8(in.filename());
    ASSERT_EQ(zr.entries().size(), 1u);
    EXPECT_EQ(zr.entries()[0].name, expected_name);

#if defined(_WIN32)
    EXPECT_STREQ(pigzpp_compress_file("\xc3\x28", to_utf8(zip_out).c_str(), 1, 6, 1,
                                      PIGZPP_FORMAT_GZIP, nullptr),
                 "pigzpp: invalid UTF-8 filename");
#else
    // On POSIX, backslash is a filename character, not a directory separator.
    const auto bs_in = fs::path(dir) / "a\\b.txt", bs_zip = fs::path(dir) / "bs.zip";
    write_bytes(bs_in, "backslash");
    ASSERT_EQ(pigzpp_compress_file(bs_in.string().c_str(), bs_zip.string().c_str(),
                                   0, 6, 1, PIGZPP_FORMAT_ZIP, nullptr), nullptr);
    EXPECT_EQ(pigzpp::zip::ZipReader(read_bytes(bs_zip)).entries()[0].name, "a\\b.txt");
#endif
}

} // namespace
