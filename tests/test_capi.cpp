#include <gtest/gtest.h>
#include "capi.h"
#include "test_support.h"
#include "zip.h"

#include <zlib.h>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <mutex>
#include <string>
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
