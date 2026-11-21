#include <gtest/gtest.h>
#include "capi.h"
#include "platform.h"
#include "test_support.h"
#include "zip.h"

#include <zlib.h>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <mutex>
#include <string>
#include <system_error>
#include <vector>

namespace {

struct ProgressSample {
    uint64_t total;
    uint64_t read;
    uint64_t written;
    uint64_t elapsed_ms;
};

std::vector<ProgressSample> samples;
std::mutex samples_mutex;

void record_progress(uint64_t total, uint64_t read, uint64_t written, uint64_t elapsed_ms) {
    {
        std::lock_guard<std::mutex> lock(samples_mutex);
        samples.push_back({total, read, written, elapsed_ms});
    }
}

class CapiFileTest : public ::testing::Test {
protected:
    std::string dir;

    void SetUp() override {
        dir = test_support::make_temp_dir("pigzpp_capi_");
        samples.clear();
    }

    void TearDown() override {
        std::filesystem::remove_all(dir);
    }

    void check_file(const std::string& data, bool progress, bool check_periodic = false,
                    int format = PIGZPP_FORMAT_GZIP, int threads = 2) {
        const auto input = (std::filesystem::path(dir) / "input.txt").string();
        const auto output = (std::filesystem::path(dir) / "input.gz").string();
        {
            std::ofstream file(input, std::ios::binary);
            ASSERT_TRUE(file);
            file.write(data.data(), static_cast<std::streamsize>(data.size()));
            ASSERT_TRUE(file);
        }

        ::testing::internal::CaptureStderr();
        auto start = std::chrono::steady_clock::now();
        const char* error = pigzpp_compress_file(
            input.c_str(), output.c_str(), 0, 6, threads, format, progress ? record_progress : nullptr);
        auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - start).count();
        std::string stderr_output = ::testing::internal::GetCapturedStderr();
        ASSERT_EQ(error, nullptr) << (error ? error : "");
        EXPECT_TRUE(stderr_output.empty());

        std::ifstream file(output, std::ios::binary);
        ASSERT_TRUE(file);
        std::vector<uint8_t> compressed{std::istreambuf_iterator<char>(file),
                                        std::istreambuf_iterator<char>()};
        if (format == PIGZPP_FORMAT_GZIP) {
            ASSERT_GE(compressed.size(), 2u);
            EXPECT_EQ(compressed[0], 0x1f);
            EXPECT_EQ(compressed[1], 0x8b);
        } else if (format == PIGZPP_FORMAT_ZLIB) {
            ASSERT_GE(compressed.size(), 2u);
            EXPECT_EQ(compressed[0], 0x78);
            EXPECT_EQ((static_cast<unsigned>(compressed[0]) * 256 + compressed[1]) % 31, 0u);
        }
        if (format == PIGZPP_FORMAT_ZIP) {
            pigzpp::zip::ZipReader archive(compressed);
            auto restored = archive.read("input.txt");
            EXPECT_EQ(std::string(restored.begin(), restored.end()), data);
            EXPECT_EQ(archive.entries().size(), 1u);
        } else if (format == PIGZPP_FORMAT_RAW) {
            z_stream stream{};
            ASSERT_EQ(inflateInit2(&stream, -MAX_WBITS), Z_OK);
            std::vector<uint8_t> restored(data.size() + 1);
            stream.next_in = compressed.data();
            stream.avail_in = static_cast<uInt>(compressed.size());
            stream.next_out = restored.data();
            stream.avail_out = static_cast<uInt>(restored.size());
            int result = inflate(&stream, Z_FINISH);
            EXPECT_EQ(result, Z_STREAM_END);
            EXPECT_EQ(stream.total_out, data.size());
            EXPECT_EQ(std::string(restored.begin(), restored.begin() + stream.total_out), data);
            EXPECT_EQ(inflateEnd(&stream), Z_OK);
        } else {
            auto decompressed = pigzpp_gzip_decompress(compressed.data(), compressed.size(), 1);
            ASSERT_EQ(decompressed.error, nullptr) << (decompressed.error ? decompressed.error : "");
            EXPECT_EQ(std::string(reinterpret_cast<const char*>(decompressed.data), decompressed.size),
                      data);
            pigzpp_free(decompressed);
        }

        if (progress) {
            ASSERT_FALSE(samples.empty());
            for (const auto& sample : samples)
                EXPECT_EQ(sample.total, data.size());
            EXPECT_EQ(samples.back().read, data.size());
            EXPECT_EQ(samples.back().written, compressed.size());
            EXPECT_LE(samples.back().elapsed_ms, static_cast<uint64_t>(elapsed));
            for (size_t i = 1; i < samples.size(); ++i)
                EXPECT_LE(samples[i - 1].elapsed_ms, samples[i].elapsed_ms);
            if (check_periodic) {
                if (samples.size() < 2 && elapsed < 400)
                    GTEST_SKIP() << "Compression completed too quickly to observe periodic progress";
                ASSERT_GE(samples.size(), 2u);
                EXPECT_GE(samples.front().elapsed_ms, 200u);
            }
        } else {
            EXPECT_TRUE(samples.empty());
        }
    }
};

TEST_F(CapiFileTest, ProgressReportsFinalBytes) {
    check_file(std::string(512 * 1024, 'x'), true);
}

TEST_F(CapiFileTest, ProgressReportsEmptyFile) {
    check_file("", true);
}

TEST_F(CapiFileTest, FormatsRoundTrip) {
    for (int format : {PIGZPP_FORMAT_GZIP, PIGZPP_FORMAT_ZLIB, PIGZPP_FORMAT_ZIP, PIGZPP_FORMAT_RAW}) {
        for (int threads : {1, 2}) {
            for (const auto& data : {std::string(), std::string(300 * 1024, 'x'),
                                     std::string("Format round trip: \0 binary data", 32)}) {
                samples.clear();
                SCOPED_TRACE(format);
                SCOPED_TRACE(threads);
                SCOPED_TRACE(data.size());
                check_file(data, true, false, format, threads);
                std::filesystem::remove(std::filesystem::path(dir) / "input.gz");
            }
        }
    }
}

TEST_F(CapiFileTest, InvalidFormatDoesNotOverwriteOutput) {
    const auto input = (std::filesystem::path(dir) / "input.txt").string();
    const auto output = (std::filesystem::path(dir) / "output").string();
    {
        std::ofstream in(input);
        std::ofstream out(output);
        in << "input";
        out << "preserved";
    }
    const char* error = pigzpp_compress_file(input.c_str(), output.c_str(), 1, 6, 1, -1, record_progress);
    ASSERT_NE(error, nullptr);
    EXPECT_STREQ(error, "pigzpp: invalid compression format");
    std::ifstream file(output);
    EXPECT_EQ(std::string(std::istreambuf_iterator<char>(file), {}), "preserved");
    EXPECT_TRUE(samples.empty());
}

TEST_F(CapiFileTest, OverwriteRejectsInputPathAsOutput) {
    const auto input = (std::filesystem::path(dir) / "input.txt").string();
    {
        std::ofstream file(input, std::ios::binary);
        file << "preserve this input";
    }

    const char* error = pigzpp_compress_file(
        input.c_str(), input.c_str(), 1, 6, 1, PIGZPP_FORMAT_GZIP, record_progress);
    ASSERT_NE(error, nullptr);
    EXPECT_STREQ(error, "pigzpp: input and output are the same file");
    std::ifstream file(input, std::ios::binary);
    EXPECT_EQ(std::string(std::istreambuf_iterator<char>(file), {}), "preserve this input");
    EXPECT_TRUE(samples.empty());
}

TEST_F(CapiFileTest, OverwriteRejectsHardLinkToInput) {
    const auto input = std::filesystem::path(dir) / "input.txt";
    const auto output = std::filesystem::path(dir) / "hardlink.gz";
    {
        std::ofstream file(input, std::ios::binary);
        file << "preserve this input";
    }
    std::filesystem::create_hard_link(input, output);

    const char* error = pigzpp_compress_file(
        input.string().c_str(), output.string().c_str(), 1, 6, 1,
        PIGZPP_FORMAT_GZIP, record_progress);
    ASSERT_NE(error, nullptr);
    EXPECT_STREQ(error, "pigzpp: input and output are the same file");
    EXPECT_TRUE(std::filesystem::exists(output));
    std::ifstream file(input, std::ios::binary);
    EXPECT_EQ(std::string(std::istreambuf_iterator<char>(file), {}), "preserve this input");
    EXPECT_TRUE(samples.empty());
}

TEST_F(CapiFileTest, OverwriteRejectsSymlinkToInput) {
    const auto input = std::filesystem::path(dir) / "input.txt";
    const auto output = std::filesystem::path(dir) / "symlink.gz";
    {
        std::ofstream file(input, std::ios::binary);
        file << "preserve this input";
    }
    std::error_code ec;
    std::filesystem::create_symlink(input.filename(), output, ec);
    if (ec)
        GTEST_SKIP() << "Unable to create symbolic link: " << ec.message();

    const char* error = pigzpp_compress_file(
        input.string().c_str(), output.string().c_str(), 1, 6, 1,
        PIGZPP_FORMAT_GZIP, record_progress);
    ASSERT_NE(error, nullptr);
    EXPECT_STREQ(error, "pigzpp: input and output are the same file");
    EXPECT_TRUE(std::filesystem::is_symlink(output));
    std::ifstream file(input, std::ios::binary);
    EXPECT_EQ(std::string(std::istreambuf_iterator<char>(file), {}), "preserve this input");
    EXPECT_TRUE(samples.empty());
}

TEST_F(CapiFileTest, OverwriteReplacesDifferentFile) {
    const auto input = (std::filesystem::path(dir) / "input.txt").string();
    const auto output = (std::filesystem::path(dir) / "output.gz").string();
    const std::string data = "compress this input";
    {
        std::ofstream in(input, std::ios::binary);
        std::ofstream out(output, std::ios::binary);
        in << data;
        out << std::string(4096, 'x');
    }

    const char* error = pigzpp_compress_file(
        input.c_str(), output.c_str(), 1, 6, 1, PIGZPP_FORMAT_GZIP, nullptr);
    ASSERT_EQ(error, nullptr) << (error ? error : "");
    std::ifstream file(output, std::ios::binary);
    std::vector<uint8_t> compressed{std::istreambuf_iterator<char>(file),
                                    std::istreambuf_iterator<char>()};
    auto decompressed = pigzpp_gzip_decompress(compressed.data(), compressed.size(), 1);
    ASSERT_EQ(decompressed.error, nullptr) << (decompressed.error ? decompressed.error : "");
    EXPECT_EQ(std::string(reinterpret_cast<const char*>(decompressed.data), decompressed.size), data);
    pigzpp_free(decompressed);
}

TEST_F(CapiFileTest, Utf8FilePathsRoundTripAndOverwrite) {
    const auto unicode_dir = std::filesystem::path(dir) / std::filesystem::path(u8"\u76ee\u5f55");
    std::filesystem::create_directory(unicode_dir);
    const auto input = unicode_dir / std::filesystem::path(u8"\u6587\u4ef6-\U0001f600.txt");
    const auto output = unicode_dir / std::filesystem::path(u8"\u538b\u7f29.gz");
    const auto input_u8 = input.u8string();
    const auto output_u8 = output.u8string();
    const std::string input_utf8(input_u8.begin(), input_u8.end());
    const std::string output_utf8(output_u8.begin(), output_u8.end());
    const std::string data = "unicode filename contents";
    {
        std::ofstream file(input, std::ios::binary);
        ASSERT_TRUE(file);
        file << data;
    }

    const char* error = pigzpp_compress_file(
        input_utf8.c_str(), output_utf8.c_str(), 0, 6, 1, PIGZPP_FORMAT_GZIP, nullptr);
    ASSERT_EQ(error, nullptr) << (error ? error : "");
    EXPECT_NE(pigzpp_compress_file(input_utf8.c_str(), output_utf8.c_str(), 0, 6, 1,
                                   PIGZPP_FORMAT_GZIP, nullptr), nullptr);
    error = pigzpp_compress_file(
        input_utf8.c_str(), output_utf8.c_str(), 1, 6, 1, PIGZPP_FORMAT_GZIP, nullptr);
    ASSERT_EQ(error, nullptr) << (error ? error : "");
    error = pigzpp_compress_file(
        input_utf8.c_str(), input_utf8.c_str(), 1, 6, 1, PIGZPP_FORMAT_GZIP, nullptr);
    ASSERT_NE(error, nullptr);
    EXPECT_STREQ(error, "pigzpp: input and output are the same file");

    std::ifstream file(output, std::ios::binary);
    ASSERT_TRUE(file);
    std::vector<uint8_t> compressed{std::istreambuf_iterator<char>(file), {}};
    auto decompressed = pigzpp_gzip_decompress(compressed.data(), compressed.size(), 1);
    ASSERT_EQ(decompressed.error, nullptr) << (decompressed.error ? decompressed.error : "");
    EXPECT_EQ(std::string(reinterpret_cast<const char*>(decompressed.data), decompressed.size), data);
    pigzpp_free(decompressed);

    const auto zip_output = unicode_dir / std::filesystem::path(u8"\u538b\u7f29.zip");
    const auto zip_output_u8 = zip_output.u8string();
    const std::string zip_output_utf8(zip_output_u8.begin(), zip_output_u8.end());
    error = pigzpp_compress_file(
        input_utf8.c_str(), zip_output_utf8.c_str(), 0, 6, 1, PIGZPP_FORMAT_ZIP, nullptr);
    ASSERT_EQ(error, nullptr) << (error ? error : "");
    std::ifstream zip_file(zip_output, std::ios::binary);
    ASSERT_TRUE(zip_file);
    std::vector<uint8_t> zip_bytes{std::istreambuf_iterator<char>(zip_file), {}};
    ASSERT_GE(zip_bytes.size(), 8u);
    uint16_t zip_flags = static_cast<uint16_t>(zip_bytes[6] | (zip_bytes[7] << 8));
    EXPECT_NE(zip_flags & 0x0800u, 0u);
    pigzpp::zip::ZipReader reader(zip_bytes);
    const auto expected_u8 = input.filename().u8string();
    const std::string expected_name(expected_u8.begin(), expected_u8.end());
    ASSERT_EQ(reader.entries().size(), 1u);
    EXPECT_EQ(reader.entries()[0].name, expected_name);
    auto extracted = reader.read(expected_name);
    EXPECT_EQ(std::string(extracted.begin(), extracted.end()), data);
}

#if defined(_WIN32)
TEST_F(CapiFileTest, InvalidUtf8PathsAreRejectedBeforeOpeningOutput) {
    const auto input = std::filesystem::path(dir) / "input.txt";
    const auto output = std::filesystem::path(dir) / "output.gz";
    {
        std::ofstream file(input);
        file << "input";
    }
    const auto input_u8 = input.u8string();
    const auto output_u8 = output.u8string();
    const std::string input_utf8(input_u8.begin(), input_u8.end());
    const std::string output_utf8(output_u8.begin(), output_u8.end());
    const char* error = pigzpp_compress_file(
        "\xc3\x28", output_utf8.c_str(), 1, 6, 1, PIGZPP_FORMAT_GZIP, nullptr);
    ASSERT_NE(error, nullptr);
    EXPECT_STREQ(error, "pigzpp: invalid UTF-8 filename");
    error = pigzpp_compress_file(
        input_utf8.c_str(), "\xc3\x28", 1, 6, 1, PIGZPP_FORMAT_GZIP, nullptr);
    ASSERT_NE(error, nullptr);
    EXPECT_STREQ(error, "pigzpp: invalid UTF-8 filename");
    EXPECT_FALSE(std::filesystem::exists(output));
}

TEST_F(CapiFileTest, WidePathCleanupRemovesUnicodeFile) {
    const auto path = std::filesystem::path(dir) / std::filesystem::path(u8"\u6587\u4ef6.gz");
    {
        std::ofstream file(path);
        ASSERT_TRUE(file);
        file << "partial output";
    }
    EXPECT_EQ(pigzpp::platform::unlink(path.c_str()), 0);
    EXPECT_FALSE(std::filesystem::exists(path));
}
#endif

TEST_F(CapiFileTest, ProgressReportsPeriodically) {
    std::string data(128 * 1024 * 1024, '\0');
    uint32_t state = 0x12345678;
    for (char& byte : data) {
        state ^= state << 13;
        state ^= state >> 17;
        state ^= state << 5;
        byte = static_cast<char>(state & 0xff);
    }
    check_file(data, true, true);
}

TEST_F(CapiFileTest, NoProgressCallback) {
    check_file("no progress callback", false);
}

} // namespace
