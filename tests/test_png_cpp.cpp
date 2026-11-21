#include <gtest/gtest.h>
#include "png.h"
#include "test_support.h"

#include <algorithm>
#include <filesystem>
#include <stdexcept>
#include <string>
#include <vector>

namespace fs = std::filesystem;

class PngTest : public ::testing::Test {
protected:
    std::string tmp_dir;

    void SetUp() override {
        tmp_dir = test_support::make_temp_dir("pigzpp_png_");
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
    explicit_options.strategy = pigzpp::Strategy::Default;
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

// Minimal PNG writer for formats the encoder does not produce; IDAT uses stored deflate blocks.
namespace {

void put_be32(std::vector<uint8_t>& out, uint32_t value) {
    for (int shift = 24; shift >= 0; shift -= 8)
        out.push_back(static_cast<uint8_t>(value >> shift));
}

uint32_t test_crc32(const uint8_t* data, size_t size) {
    uint32_t crc = 0xffffffffU;
    for (size_t index = 0; index < size; ++index) {
        crc ^= data[index];
        for (int bit = 0; bit < 8; ++bit)
            crc = (crc >> 1) ^ (0xedb88320U & (0U - (crc & 1U)));
    }
    return ~crc;
}

void put_chunk(std::vector<uint8_t>& out, const char* type, const std::vector<uint8_t>& data) {
    put_be32(out, static_cast<uint32_t>(data.size()));
    const size_t start = out.size();
    out.insert(out.end(), type, type + 4);
    out.insert(out.end(), data.begin(), data.end());
    put_be32(out, test_crc32(out.data() + start, out.size() - start));
}

std::vector<uint8_t> stored_zlib(const std::vector<uint8_t>& raw) {
    std::vector<uint8_t> out{0x78, 0x01};
    size_t offset = 0;
    do {
        const auto size = static_cast<uint16_t>(std::min<size_t>(raw.size() - offset, 65535));
        out.push_back(offset + size == raw.size() ? 1 : 0);
        out.push_back(static_cast<uint8_t>(size));
        out.push_back(static_cast<uint8_t>(size >> 8));
        out.push_back(static_cast<uint8_t>(~size));
        out.push_back(static_cast<uint8_t>(~size >> 8));
        out.insert(out.end(), raw.begin() + offset, raw.begin() + offset + size);
        offset += size;
    } while (offset < raw.size());
    uint32_t a = 1, b = 0;
    for (auto byte : raw) {
        a = (a + byte) % 65521;
        b = (b + a) % 65521;
    }
    put_be32(out, (b << 16) | a);
    return out;
}

struct RawPng {
    uint32_t width;
    uint32_t height;
    uint8_t bit_depth;
    uint8_t color_type;
    // Per-pixel sample values (samples_per_pixel each, row-major), at the given bit depth.
    std::vector<uint32_t> samples = {};
    std::vector<uint8_t> palette = {};
    std::vector<uint8_t> transparency = {};
    bool interlaced = false;
};

std::vector<uint8_t> pack_rows(const RawPng& png, uint32_t x0, uint32_t y0, uint32_t dx, uint32_t dy) {
    const uint32_t spp = png.color_type == 2 ? 3 : png.color_type == 4 ? 2 : png.color_type == 6 ? 4 : 1;
    std::vector<uint8_t> raw;
    for (uint32_t y = y0; y < png.height; y += dy) {
        std::vector<uint8_t> row;
        uint32_t bits = 0, acc = 0, count = 0;
        for (uint32_t x = x0; x < png.width; x += dx, ++count) {
            for (uint32_t s = 0; s < spp; ++s) {
                const auto value = png.samples[(static_cast<size_t>(y) * png.width + x) * spp + s];
                if (png.bit_depth == 16) {
                    row.push_back(static_cast<uint8_t>(value >> 8));
                    row.push_back(static_cast<uint8_t>(value));
                } else if (png.bit_depth == 8) {
                    row.push_back(static_cast<uint8_t>(value));
                } else {
                    acc = (acc << png.bit_depth) | value;
                    bits += png.bit_depth;
                    if (bits == 8) {
                        row.push_back(static_cast<uint8_t>(acc));
                        acc = bits = 0;
                    }
                }
            }
        }
        if (count == 0)
            return raw;
        if (bits)
            row.push_back(static_cast<uint8_t>(acc << (8 - bits)));
        raw.push_back(0);
        raw.insert(raw.end(), row.begin(), row.end());
    }
    return raw;
}

std::vector<uint8_t> build_png(const RawPng& png) {
    std::vector<uint8_t> raw;
    if (png.interlaced) {
        const uint32_t passes[7][4] = {{0, 0, 8, 8}, {4, 0, 8, 8}, {0, 4, 4, 8}, {2, 0, 4, 4},
                                       {0, 2, 2, 4}, {1, 0, 2, 2}, {0, 1, 1, 2}};
        for (const auto& pass : passes) {
            auto part = pack_rows(png, pass[0], pass[1], pass[2], pass[3]);
            raw.insert(raw.end(), part.begin(), part.end());
        }
    } else {
        raw = pack_rows(png, 0, 0, 1, 1);
    }
    std::vector<uint8_t> out{137, 80, 78, 71, 13, 10, 26, 10};
    std::vector<uint8_t> ihdr;
    put_be32(ihdr, png.width);
    put_be32(ihdr, png.height);
    ihdr.insert(ihdr.end(), {png.bit_depth, png.color_type, 0, 0, static_cast<uint8_t>(png.interlaced)});
    put_chunk(out, "IHDR", ihdr);
    if (!png.palette.empty())
        put_chunk(out, "PLTE", png.palette);
    if (!png.transparency.empty())
        put_chunk(out, "tRNS", png.transparency);
    put_chunk(out, "IDAT", stored_zlib(raw));
    put_chunk(out, "IEND", {});
    return out;
}

pigzpp::png::Image decode_raw(const RawPng& png) {
    const auto encoded = build_png(png);
    return pigzpp::png::decode(encoded.data(), encoded.size());
}

} // namespace

TEST_F(PngTest, DecodesPalettePngWithTransparency) {
    RawPng png{5, 3, 4, 3, {}, {255, 0, 0, 0, 255, 0, 0, 0, 255}, {255, 128}};
    for (uint32_t index = 0; index < 15; ++index)
        png.samples.push_back(index % 3);
    const auto decoded = decode_raw(png);

    ASSERT_EQ(decoded.channels, 4);
    const uint8_t expected[3][4] = {{255, 0, 0, 255}, {0, 255, 0, 128}, {0, 0, 255, 255}};
    for (uint32_t index = 0; index < 15; ++index)
        EXPECT_EQ(std::vector<uint8_t>(decoded.pixels.begin() + index * 4, decoded.pixels.begin() + index * 4 + 4),
                  std::vector<uint8_t>(expected[index % 3], expected[index % 3] + 4));
}

TEST_F(PngTest, DecodesPalettePngWithoutTransparencyAsRgb) {
    RawPng png{9, 2, 8, 3, std::vector<uint32_t>(18, 1), {10, 20, 30, 40, 50, 60}};
    const auto decoded = decode_raw(png);

    ASSERT_EQ(decoded.channels, 3);
    for (size_t index = 0; index < 18; ++index)
        EXPECT_EQ(std::vector<uint8_t>(decoded.pixels.begin() + index * 3, decoded.pixels.begin() + index * 3 + 3),
                  (std::vector<uint8_t>{40, 50, 60}));
}

TEST_F(PngTest, DecodesLowBitDepthGrayScaledToEightBits) {
    RawPng png{11, 2, 1, 0, {}};
    for (uint32_t index = 0; index < 22; ++index)
        png.samples.push_back((index * 7) % 2);
    const auto decoded = decode_raw(png);

    ASSERT_EQ(decoded.channels, 1);
    for (uint32_t index = 0; index < 22; ++index)
        EXPECT_EQ(decoded.pixels[index], png.samples[index] ? 255 : 0);
}

TEST_F(PngTest, DecodesSixteenBitRgbWithColorKey) {
    RawPng png{2, 1, 16, 2, {0x1234, 0x5678, 0x9abc, 0xff00, 0x00ff, 0x8080}, {}, {0x12, 0x34, 0x56, 0x78, 0x9a, 0xbc}};
    const auto decoded = decode_raw(png);

    ASSERT_EQ(decoded.channels, 4);
    EXPECT_EQ(decoded.pixels, (std::vector<uint8_t>{0x12, 0x56, 0x9a, 0, 0xff, 0x00, 0x80, 255}));
}

TEST_F(PngTest, DecodesAdam7InterlacedLikeProgressive) {
    RawPng png{13, 10, 8, 2, {}};
    const auto rgb = rgb_pixels(png.width, png.height);
    png.samples.assign(rgb.begin(), rgb.end());
    png.interlaced = true;
    const auto decoded = decode_raw(png);

    ASSERT_EQ(decoded.channels, 3);
    EXPECT_EQ(decoded.pixels, rgb);
}

TEST_F(PngTest, RejectsPaletteIndexOutsidePalette) {
    RawPng png{2, 1, 8, 3, {0, 2}, {1, 2, 3, 4, 5, 6}};
    EXPECT_THROW(static_cast<void>(decode_raw(png)), std::runtime_error);
}

TEST_F(PngTest, RejectsInvalidBitDepthForColorType) {
    RawPng png{2, 1, 4, 2, {1, 2, 3, 4, 5, 6}};
    EXPECT_THROW(static_cast<void>(decode_raw(png)), std::runtime_error);
}
