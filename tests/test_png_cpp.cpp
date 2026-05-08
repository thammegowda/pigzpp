#include <gtest/gtest.h>
#include "png.h"

#include <filesystem>
#include <stdexcept>
#include <string>
#include <vector>

namespace fs = std::filesystem;

class PngTest : public ::testing::Test {
protected:
    std::string tmp_dir;

    void SetUp() override {
        fs::create_directories("tmp");
        char tmpl[] = "tmp/pigzpp_png_XXXXXX";
        char* dir = mkdtemp(tmpl);
        ASSERT_NE(dir, nullptr);
        tmp_dir = dir;
    }

    void TearDown() override {
        fs::remove_all(tmp_dir);
    }

    std::string path(const std::string& name) const {
        return tmp_dir + "/" + name;
    }
};

static std::vector<uint8_t> gray_alpha_pixels(uint32_t width, uint32_t height) {
    std::vector<uint8_t> pixels(static_cast<size_t>(width) * height * 2);
    size_t offset = 0;
    for (uint32_t row = 0; row < height; ++row) {
        for (uint32_t column = 0; column < width; ++column) {
            pixels[offset] = static_cast<uint8_t>((column * 9 + row * 5) & 0xff);
            pixels[offset + 1] = static_cast<uint8_t>(255 - ((column * 3 + row * 7) & 0x7f));
            offset += 2;
        }
    }
    return pixels;
}

static std::vector<uint8_t> rgb_pixels(uint32_t width, uint32_t height) {
    std::vector<uint8_t> pixels(static_cast<size_t>(width) * height * 3);
    size_t offset = 0;
    for (uint32_t row = 0; row < height; ++row) {
        for (uint32_t column = 0; column < width; ++column) {
            pixels[offset] = static_cast<uint8_t>((column * 7 + row * 3) & 0xff);
            pixels[offset + 1] = static_cast<uint8_t>((column * 5) & 0xff);
            pixels[offset + 2] = static_cast<uint8_t>((row * 11) & 0xff);
            offset += 3;
        }
    }
    return pixels;
}

TEST_F(PngTest, GrayAlphaRoundtrip) {
    uint32_t width = 31;
    uint32_t height = 21;
    auto pixels = gray_alpha_pixels(width, height);
    auto encoded = pigzpp::png::encode_buffer(pixels.data(), pixels.size(), width, height, 2, pigzpp::png::preset_options("fast"));
    auto decoded = pigzpp::png::decode(encoded.data(), encoded.size());

    EXPECT_EQ(decoded.width, width);
    EXPECT_EQ(decoded.height, height);
    EXPECT_EQ(decoded.channels, 2);
    EXPECT_EQ(decoded.pixels, pixels);
}

TEST_F(PngTest, BalancedPresetMatchesExplicitOptions) {
    uint32_t width = 32;
    uint32_t height = 24;
    auto pixels = rgb_pixels(width, height);

    pigzpp::png::EncodeOptions explicit_options;
    explicit_options.level = 1;
    explicit_options.strategy = pigzpp::Strategy::Rle;
    explicit_options.filter = pigzpp::png::FilterMode::AdaptiveFast;

    auto explicit_png = pigzpp::png::encode_buffer(pixels.data(), pixels.size(), width, height, 3, explicit_options);
    auto preset_png = pigzpp::png::encode_buffer(pixels.data(), pixels.size(), width, height, 3, pigzpp::png::preset_options("balanced"));

    EXPECT_EQ(preset_png, explicit_png);
}

TEST_F(PngTest, FastPresetMatchesExplicitOptions) {
    uint32_t width = 32;
    uint32_t height = 24;
    auto pixels = rgb_pixels(width, height);

    pigzpp::png::EncodeOptions explicit_options;
    explicit_options.level = 1;
    explicit_options.strategy = pigzpp::Strategy::Rle;
    explicit_options.filter = pigzpp::png::FilterMode::Up;

    auto explicit_png = pigzpp::png::encode_buffer(pixels.data(), pixels.size(), width, height, 3, explicit_options);
    auto preset_png = pigzpp::png::encode_buffer(pixels.data(), pixels.size(), width, height, 3, pigzpp::png::preset_options("fast"));

    EXPECT_EQ(preset_png, explicit_png);
}

TEST_F(PngTest, SmallPresetMatchesExplicitOptions) {
    uint32_t width = 32;
    uint32_t height = 24;
    auto pixels = rgb_pixels(width, height);

    pigzpp::png::EncodeOptions explicit_options;
    explicit_options.level = 9;
    explicit_options.strategy = pigzpp::Strategy::Filtered;
    explicit_options.filter = pigzpp::png::FilterMode::AdaptiveAll;

    auto explicit_png = pigzpp::png::encode_buffer(pixels.data(), pixels.size(), width, height, 3, explicit_options);
    auto preset_png = pigzpp::png::encode_buffer(pixels.data(), pixels.size(), width, height, 3, pigzpp::png::preset_options("small"));

    EXPECT_EQ(preset_png, explicit_png);
}

TEST_F(PngTest, SaveLoadRoundtrip) {
    uint32_t width = 32;
    uint32_t height = 24;
    auto pixels = rgb_pixels(width, height);
    auto filename = path("rgb.png");

    pigzpp::png::save_buffer(filename, pixels.data(), pixels.size(), width, height, 3, pigzpp::png::preset_options("balanced"));
    auto decoded = pigzpp::png::load(filename);

    EXPECT_EQ(decoded.width, width);
    EXPECT_EQ(decoded.height, height);
    EXPECT_EQ(decoded.channels, 3);
    EXPECT_EQ(decoded.pixels, pixels);
}

TEST_F(PngTest, RejectsBadChunkCrc) {
    uint32_t width = 32;
    uint32_t height = 24;
    auto pixels = rgb_pixels(width, height);
    auto encoded = pigzpp::png::encode_buffer(pixels.data(), pixels.size(), width, height, 3, pigzpp::png::preset_options("fast"));
    encoded.back() ^= 0x01;

    EXPECT_THROW(static_cast<void>(pigzpp::png::decode(encoded.data(), encoded.size())), std::runtime_error);
}

TEST_F(PngTest, LevelZeroUsesStoredDeflate) {
    uint32_t width = 128;
    uint32_t height = 128;
    std::vector<uint8_t> pixels(static_cast<size_t>(width) * height, 0);

    pigzpp::png::EncodeOptions stored_options;
    stored_options.level = 0;
    stored_options.strategy = pigzpp::Strategy::Default;
    stored_options.filter = pigzpp::png::FilterMode::None;

    pigzpp::png::EncodeOptions compressed_options;
    compressed_options.level = 1;
    compressed_options.strategy = pigzpp::Strategy::Default;
    compressed_options.filter = pigzpp::png::FilterMode::None;

    auto stored = pigzpp::png::encode_buffer(pixels.data(), pixels.size(), width, height, 1, stored_options);
    auto compressed = pigzpp::png::encode_buffer(pixels.data(), pixels.size(), width, height, 1, compressed_options);

    EXPECT_GT(stored.size(), compressed.size() * 4);
    auto decoded = pigzpp::png::decode(stored.data(), stored.size());
    EXPECT_EQ(decoded.pixels, pixels);
}

TEST_F(PngTest, RejectsTrailingDataAfterIend) {
    uint32_t width = 32;
    uint32_t height = 24;
    auto pixels = rgb_pixels(width, height);
    auto encoded = pigzpp::png::encode_buffer(pixels.data(), pixels.size(), width, height, 3, pigzpp::png::preset_options("fast"));
    encoded.push_back(0);

    EXPECT_THROW(static_cast<void>(pigzpp::png::decode(encoded.data(), encoded.size())), std::runtime_error);
}
