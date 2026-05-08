// PNG encode/decode helpers using pigzpp's accelerated DEFLATE stack.

#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "config.h"

namespace pigzpp::png {

enum class FilterMode : int {
    None = 0,
    Sub = 1,
    Up = 2,
    Average = 3,
    Paeth = 4,
    AdaptiveFast = 5,
    AdaptiveAll = 6,
};

enum class Preset : int {
    Fast = 0,
    Balanced = 1,
    Small = 2,
};

struct EncodeOptions {
    int level = 1;
    Strategy strategy = Strategy::Rle;
    FilterMode filter = FilterMode::Up;
    size_t idat_chunk_size = 1 << 20;
};

struct Image {
    uint32_t width = 0;
    uint32_t height = 0;
    uint8_t channels = 0;
    std::vector<uint8_t> pixels;
};

FilterMode parse_filter_mode(const std::string& value);
Strategy parse_strategy(const std::string& value);
Preset parse_preset(const std::string& value);
EncodeOptions preset_options(Preset preset);
EncodeOptions preset_options(const std::string& preset);

std::vector<uint8_t> encode(
    const uint8_t* pixels,
    uint32_t width,
    uint32_t height,
    uint8_t channels,
    const EncodeOptions& options = {}
);

std::vector<uint8_t> encode_buffer(
    const uint8_t* pixels,
    size_t pixel_size,
    uint32_t width,
    uint32_t height,
    uint8_t channels,
    const EncodeOptions& options
);

std::vector<uint8_t> encode_buffer(
    const uint8_t* pixels,
    size_t pixel_size,
    uint32_t width,
    uint32_t height,
    uint8_t channels,
    int level = 1,
    const std::string& strategy = "rle",
    const std::string& filter = "up",
    size_t idat_chunk_size = 1 << 20
);

void save(
    const std::string& path,
    const uint8_t* pixels,
    uint32_t width,
    uint32_t height,
    uint8_t channels,
    const EncodeOptions& options = {}
);

void save_buffer(
    const std::string& path,
    const uint8_t* pixels,
    size_t pixel_size,
    uint32_t width,
    uint32_t height,
    uint8_t channels,
    const EncodeOptions& options = {}
);

Image decode(const uint8_t* png_data, size_t png_size);
Image load(const std::string& path);

} // namespace pigzpp::png