// PNG encode/decode helpers using zlib-wrapped DEFLATE for IDAT chunks.

#include "png.h"

#include <algorithm>
#include <array>
#include <climits>
#include <cstring>
#include <fstream>
#include <stdexcept>
#include <string_view>

#include <zlib.h>

#ifdef PIGZPP_USE_ISAL
#include <isa-l/igzip_lib.h>
#endif

namespace pigzpp::png {

namespace {

constexpr std::array<uint8_t, 8> PNG_SIGNATURE{137, 80, 78, 71, 13, 10, 26, 10};
constexpr size_t DEFAULT_IDAT_CHUNK_SIZE = 1 << 20;

uint32_t read_be32(const uint8_t* data) {
    return (static_cast<uint32_t>(data[0]) << 24) |
           (static_cast<uint32_t>(data[1]) << 16) |
           (static_cast<uint32_t>(data[2]) << 8) |
           static_cast<uint32_t>(data[3]);
}

uint32_t chunk_crc(const uint8_t* type, const uint8_t* data, size_t size) {
    uint32_t crc_value = crc32(0L, Z_NULL, 0);
    crc_value = crc32(crc_value, type, 4);
    if (size > 0) {
        const uint8_t* cursor = data;
        size_t remaining = size;
        while (remaining > 0) {
            uInt chunk_size = static_cast<uInt>(std::min<size_t>(remaining, UINT_MAX));
            crc_value = crc32(crc_value, cursor, chunk_size);
            cursor += chunk_size;
            remaining -= chunk_size;
        }
    }
    return crc_value;
}

void append_be32(std::vector<uint8_t>& output, uint32_t value) {
    output.push_back(static_cast<uint8_t>((value >> 24) & 0xff));
    output.push_back(static_cast<uint8_t>((value >> 16) & 0xff));
    output.push_back(static_cast<uint8_t>((value >> 8) & 0xff));
    output.push_back(static_cast<uint8_t>(value & 0xff));
}

void append_chunk(
    std::vector<uint8_t>& output,
    const std::array<uint8_t, 4>& type,
    const uint8_t* data,
    size_t size
) {
    if (size > UINT32_MAX)
        throw std::invalid_argument("PNG chunk too large");
    append_be32(output, static_cast<uint32_t>(size));
    output.insert(output.end(), type.begin(), type.end());
    if (size > 0)
        output.insert(output.end(), data, data + size);

    append_be32(output, chunk_crc(type.data(), data, size));
}

void write_binary_file(const std::string& path, const std::vector<uint8_t>& data) {
    std::ofstream output(path, std::ios::binary);
    if (!output)
        throw std::runtime_error("cannot open PNG output file: " + path);
    output.write(reinterpret_cast<const char*>(data.data()), static_cast<std::streamsize>(data.size()));
    if (!output)
        throw std::runtime_error("failed to write PNG output file: " + path);
}

std::vector<uint8_t> read_binary_file(const std::string& path) {
    std::ifstream input(path, std::ios::binary | std::ios::ate);
    if (!input)
        throw std::runtime_error("cannot open PNG input file: " + path);
    std::streamsize size = input.tellg();
    if (size < 0)
        throw std::runtime_error("cannot determine PNG input file size: " + path);
    input.seekg(0, std::ios::beg);
    std::vector<uint8_t> data(static_cast<size_t>(size));
    if (size > 0 && !input.read(reinterpret_cast<char*>(data.data()), size))
        throw std::runtime_error("failed to read PNG input file: " + path);
    return data;
}

#ifdef PIGZPP_USE_ISAL
uint16_t zlib_header(int level) {
    int normalized_level = level == -1 ? 6 : level;
    uint16_t flevel = static_cast<uint16_t>(
        normalized_level <= 1 ? 0 :
        normalized_level <= 5 ? 1 :
        normalized_level == 6 ? 2 : 3);
    uint16_t header = static_cast<uint16_t>((0x78 << 8) | (flevel << 6));
    header = static_cast<uint16_t>(header + ((31 - (header % 31)) % 31));
    return header;
}

int isal_level(int level) {
    if (level <= 1) return 0;
    if (level <= 5) return 1;
    if (level <= 8) return 2;
    return 3;
}

size_t isal_level_buf_size(int isal_level_value) {
    switch (isal_level_value) {
    case 0: return ISAL_DEF_LVL0_DEFAULT;
    case 1: return ISAL_DEF_LVL1_DEFAULT;
    case 2: return ISAL_DEF_LVL2_DEFAULT;
    case 3: return ISAL_DEF_LVL3_DEFAULT;
    default: return ISAL_DEF_LVL3_DEFAULT;
    }
}

bool isal_supports_strategy(Strategy strategy) {
    return strategy == Strategy::Default;
}

std::vector<uint8_t> zlib_compress_isal(
    const uint8_t* input,
    size_t input_size,
    int level,
    Strategy strategy
) {
    if (level == 0 || level > 9 || input_size > UINT32_MAX || !isal_supports_strategy(strategy))
        return {};

    struct isal_zstream stream;
    isal_deflate_init(&stream);
    int mapped_level = isal_level(level);
    std::vector<uint8_t> level_buffer(isal_level_buf_size(mapped_level));
    stream.level = static_cast<uint32_t>(mapped_level);
    stream.level_buf = level_buffer.empty() ? nullptr : level_buffer.data();
    stream.level_buf_size = static_cast<uint32_t>(level_buffer.size());
    stream.gzip_flag = IGZIP_DEFLATE;
    stream.next_in = const_cast<uint8_t*>(input);
    stream.avail_in = static_cast<uint32_t>(input_size);
    stream.end_of_stream = 1;

    uint16_t header = zlib_header(level);
    std::vector<uint8_t> output;
    output.reserve(input_size / 2 + 1024);
    output.push_back(static_cast<uint8_t>((header >> 8) & 0xff));
    output.push_back(static_cast<uint8_t>(header & 0xff));

    std::array<uint8_t, 1 << 16> buffer{};
    do {
        stream.next_out = buffer.data();
        stream.avail_out = static_cast<uint32_t>(buffer.size());
        int ret = isal_deflate(&stream);
        if (ret != COMP_OK)
            throw std::runtime_error("ISA-L deflate failed: " + std::to_string(ret));
        output.insert(output.end(), buffer.data(), buffer.data() + (buffer.size() - stream.avail_out));
    } while (stream.avail_out == 0);

    uint32_t checksum = isal_adler32(1, input, input_size);
    append_be32(output, checksum);
    return output;
}

std::vector<uint8_t> zlib_decompress_isal(
    const uint8_t* input,
    size_t input_size,
    size_t expected_size
) {
    if (input_size > UINT32_MAX || expected_size > UINT32_MAX)
        return {};

    std::vector<uint8_t> output(expected_size);
    struct inflate_state state;
    isal_inflate_init(&state);
    state.crc_flag = ISAL_ZLIB;
    state.next_in = const_cast<uint8_t*>(input);
    state.avail_in = static_cast<uint32_t>(input_size);
    state.next_out = output.data();
    state.avail_out = static_cast<uint32_t>(output.size());

    int ret = isal_inflate(&state);
    if (ret != ISAL_DECOMP_OK || state.total_out != expected_size || state.avail_out != 0)
        return {};
    return output;
}
#endif

std::vector<uint8_t> zlib_compress(
    const uint8_t* input,
    size_t input_size,
    int level,
    Strategy strategy
) {
    if (level < -1 || level > 9)
        throw std::invalid_argument("PNG compression level must be -1 or 0..9");

#ifdef PIGZPP_USE_ISAL
    std::vector<uint8_t> isal_output = zlib_compress_isal(input, input_size, level == -1 ? 6 : level, strategy);
    if (!isal_output.empty())
        return isal_output;
#endif

    z_stream stream{};
    int ret = deflateInit2(&stream, level == -1 ? 6 : level, Z_DEFLATED, 15, 8,
                           static_cast<int>(strategy));
    if (ret != Z_OK)
        throw std::runtime_error("deflateInit2 failed");

    stream.next_in = const_cast<uint8_t*>(input);
    stream.avail_in = static_cast<uInt>(input_size);
    uLong bound = deflateBound(&stream, stream.avail_in);
    std::vector<uint8_t> output(bound);
    stream.next_out = output.data();
    stream.avail_out = static_cast<uInt>(output.size());

    ret = deflate(&stream, Z_FINISH);
    if (ret != Z_STREAM_END) {
        deflateEnd(&stream);
        throw std::runtime_error("deflate failed");
    }
    output.resize(stream.total_out);
    deflateEnd(&stream);
    return output;
}

std::vector<uint8_t> zlib_decompress(
    const uint8_t* input,
    size_t input_size,
    size_t expected_size
) {
#ifdef PIGZPP_USE_ISAL
    std::vector<uint8_t> isal_output = zlib_decompress_isal(input, input_size, expected_size);
    if (!isal_output.empty())
        return isal_output;
#endif

    std::vector<uint8_t> output(expected_size);
    z_stream stream{};
    int ret = inflateInit2(&stream, 15);
    if (ret != Z_OK)
        throw std::runtime_error("inflateInit2 failed");
    stream.next_in = const_cast<uint8_t*>(input);
    stream.avail_in = static_cast<uInt>(input_size);
    stream.next_out = output.data();
    stream.avail_out = static_cast<uInt>(output.size());

    ret = inflate(&stream, Z_FINISH);
    if (ret != Z_STREAM_END || stream.total_out != expected_size) {
        inflateEnd(&stream);
        throw std::runtime_error("PNG IDAT inflate failed");
    }
    inflateEnd(&stream);
    return output;
}

uint8_t paeth_predictor(uint8_t left, uint8_t up, uint8_t upper_left) {
    int estimate = static_cast<int>(left) + static_cast<int>(up) - static_cast<int>(upper_left);
    int left_distance = std::abs(estimate - static_cast<int>(left));
    int up_distance = std::abs(estimate - static_cast<int>(up));
    int upper_left_distance = std::abs(estimate - static_cast<int>(upper_left));
    if (left_distance <= up_distance && left_distance <= upper_left_distance)
        return left;
    if (up_distance <= upper_left_distance)
        return up;
    return upper_left;
}

uint64_t filter_row(
    uint8_t filter_type,
    const uint8_t* row,
    const uint8_t* previous_row,
    size_t row_bytes,
    size_t bytes_per_pixel,
    uint8_t* filtered
) {
    filtered[0] = filter_type;
    uint64_t score = 0;
    for (size_t byte_index = 0; byte_index < row_bytes; ++byte_index) {
        uint8_t left = byte_index >= bytes_per_pixel ? row[byte_index - bytes_per_pixel] : 0;
        uint8_t up = previous_row ? previous_row[byte_index] : 0;
        uint8_t upper_left = previous_row && byte_index >= bytes_per_pixel
            ? previous_row[byte_index - bytes_per_pixel]
            : 0;
        int prediction = 0;
        switch (filter_type) {
        case 0: prediction = 0; break;
        case 1: prediction = left; break;
        case 2: prediction = up; break;
        case 3: prediction = (static_cast<int>(left) + static_cast<int>(up)) / 2; break;
        case 4: prediction = paeth_predictor(left, up, upper_left); break;
        default: throw std::invalid_argument("invalid PNG filter type");
        }
        uint8_t value = static_cast<uint8_t>(row[byte_index] - prediction);
        filtered[byte_index + 1] = value;
        score += value < 128 ? value : 256 - value;
    }
    return score;
}

std::vector<uint8_t> filtered_scanlines(
    const uint8_t* pixels,
    uint32_t width,
    uint32_t height,
    uint8_t channels,
    FilterMode filter_mode
) {
    size_t row_bytes = static_cast<size_t>(width) * channels;
    size_t output_stride = row_bytes + 1;
    std::vector<uint8_t> output(static_cast<size_t>(height) * output_stride);
    std::vector<uint8_t> candidate(output_stride);
    static constexpr std::array<uint8_t, 3> adaptive_fast_filters{0, 1, 2};
    static constexpr std::array<uint8_t, 5> adaptive_all_filters{0, 1, 2, 3, 4};

    for (uint32_t row_index = 0; row_index < height; ++row_index) {
        const uint8_t* row = pixels + static_cast<size_t>(row_index) * row_bytes;
        const uint8_t* previous_row = row_index == 0 ? nullptr : row - row_bytes;
        uint8_t* destination = output.data() + static_cast<size_t>(row_index) * output_stride;

        if (filter_mode <= FilterMode::Paeth) {
            filter_row(static_cast<uint8_t>(filter_mode), row, previous_row, row_bytes, channels, destination);
            continue;
        }

        const uint8_t* filters = filter_mode == FilterMode::AdaptiveFast
            ? adaptive_fast_filters.data()
            : adaptive_all_filters.data();
        size_t filter_count = filter_mode == FilterMode::AdaptiveFast
            ? adaptive_fast_filters.size()
            : adaptive_all_filters.size();
        uint64_t best_score = UINT64_MAX;
        for (size_t filter_index = 0; filter_index < filter_count; ++filter_index) {
            uint8_t filter_type = filters[filter_index];
            uint64_t score = filter_row(filter_type, row, previous_row, row_bytes, channels, candidate.data());
            if (score < best_score) {
                best_score = score;
                std::memcpy(destination, candidate.data(), output_stride);
            }
        }
    }
    return output;
}

void unfilter_row(
    uint8_t filter_type,
    const uint8_t* source,
    const uint8_t* previous_row,
    size_t row_bytes,
    size_t bytes_per_pixel,
    uint8_t* row
) {
    for (size_t byte_index = 0; byte_index < row_bytes; ++byte_index) {
        uint8_t left = byte_index >= bytes_per_pixel ? row[byte_index - bytes_per_pixel] : 0;
        uint8_t up = previous_row ? previous_row[byte_index] : 0;
        uint8_t upper_left = previous_row && byte_index >= bytes_per_pixel
            ? previous_row[byte_index - bytes_per_pixel]
            : 0;
        int prediction = 0;
        switch (filter_type) {
        case 0: prediction = 0; break;
        case 1: prediction = left; break;
        case 2: prediction = up; break;
        case 3: prediction = (static_cast<int>(left) + static_cast<int>(up)) / 2; break;
        case 4: prediction = paeth_predictor(left, up, upper_left); break;
        default: throw std::runtime_error("unsupported PNG filter type");
        }
        row[byte_index] = static_cast<uint8_t>(source[byte_index] + prediction);
    }
}

} // namespace

uint8_t png_color_type(uint8_t channels) {
    switch (channels) {
    case 1: return 0;
    case 2: return 4;
    case 3: return 2;
    case 4: return 6;
    default: throw std::invalid_argument("PNG encoder supports only grayscale, grayscale+alpha, RGB, or RGBA uint8 input");
    }
}

FilterMode parse_filter_mode(const std::string& value) {
    if (value == "none") return FilterMode::None;
    if (value == "sub") return FilterMode::Sub;
    if (value == "up") return FilterMode::Up;
    if (value == "average") return FilterMode::Average;
    if (value == "paeth") return FilterMode::Paeth;
    if (value == "adaptive-fast") return FilterMode::AdaptiveFast;
    if (value == "adaptive-all") return FilterMode::AdaptiveAll;
    throw std::invalid_argument("unknown PNG filter mode: " + value);
}

Strategy parse_strategy(const std::string& value) {
    if (value == "default") return Strategy::Default;
    if (value == "filtered") return Strategy::Filtered;
    if (value == "huffman" || value == "huffman-only") return Strategy::HuffmanOnly;
    if (value == "rle") return Strategy::Rle;
    if (value == "fixed") return Strategy::Fixed;
    throw std::invalid_argument("unknown DEFLATE strategy: " + value);
}

Preset parse_preset(const std::string& value) {
    if (value == "" || value == "fast") return Preset::Fast;
    if (value == "balanced") return Preset::Balanced;
    if (value == "small") return Preset::Small;
    throw std::invalid_argument("unknown PNG preset: " + value);
}

EncodeOptions preset_options(Preset preset) {
    switch (preset) {
    case Preset::Fast:
        return EncodeOptions{1, Strategy::Rle, FilterMode::Up, DEFAULT_IDAT_CHUNK_SIZE};
    case Preset::Balanced:
        return EncodeOptions{1, Strategy::Rle, FilterMode::AdaptiveFast, DEFAULT_IDAT_CHUNK_SIZE};
    case Preset::Small:
        return EncodeOptions{9, Strategy::Filtered, FilterMode::AdaptiveAll, DEFAULT_IDAT_CHUNK_SIZE};
    }
    throw std::invalid_argument("unknown PNG preset");
}

EncodeOptions preset_options(const std::string& preset) {
    return preset_options(parse_preset(preset));
}

std::vector<uint8_t> encode(
    const uint8_t* pixels,
    uint32_t width,
    uint32_t height,
    uint8_t channels,
    const EncodeOptions& options
) {
    if (pixels == nullptr)
        throw std::invalid_argument("pixels must not be null");
    if (width == 0 || height == 0)
        throw std::invalid_argument("PNG dimensions must be nonzero");
    if (channels != 1 && channels != 2 && channels != 3 && channels != 4)
        throw std::invalid_argument("PNG encoder supports only grayscale, grayscale+alpha, RGB, or RGBA uint8 input");

    std::vector<uint8_t> scanlines = filtered_scanlines(pixels, width, height, channels, options.filter);
    std::vector<uint8_t> compressed = zlib_compress(scanlines.data(), scanlines.size(), options.level, options.strategy);

    std::vector<uint8_t> output;
    output.reserve(compressed.size() + 128);
    output.insert(output.end(), PNG_SIGNATURE.begin(), PNG_SIGNATURE.end());

    std::array<uint8_t, 13> ihdr{};
    ihdr[0] = static_cast<uint8_t>((width >> 24) & 0xff);
    ihdr[1] = static_cast<uint8_t>((width >> 16) & 0xff);
    ihdr[2] = static_cast<uint8_t>((width >> 8) & 0xff);
    ihdr[3] = static_cast<uint8_t>(width & 0xff);
    ihdr[4] = static_cast<uint8_t>((height >> 24) & 0xff);
    ihdr[5] = static_cast<uint8_t>((height >> 16) & 0xff);
    ihdr[6] = static_cast<uint8_t>((height >> 8) & 0xff);
    ihdr[7] = static_cast<uint8_t>(height & 0xff);
    ihdr[8] = 8;
    ihdr[9] = png_color_type(channels);
    ihdr[10] = 0;
    ihdr[11] = 0;
    ihdr[12] = 0;
    append_chunk(output, {'I', 'H', 'D', 'R'}, ihdr.data(), ihdr.size());

    size_t idat_chunk_size = options.idat_chunk_size == 0 ? DEFAULT_IDAT_CHUNK_SIZE : options.idat_chunk_size;
    for (size_t offset = 0; offset < compressed.size(); offset += idat_chunk_size) {
        size_t current_size = std::min(idat_chunk_size, compressed.size() - offset);
        append_chunk(output, {'I', 'D', 'A', 'T'}, compressed.data() + offset, current_size);
    }
    append_chunk(output, {'I', 'E', 'N', 'D'}, nullptr, 0);
    return output;
}

std::vector<uint8_t> encode_buffer(
    const uint8_t* pixels,
    size_t pixel_size,
    uint32_t width,
    uint32_t height,
    uint8_t channels,
    const EncodeOptions& options
) {
    if (width == 0 || height == 0 || channels == 0)
        throw std::invalid_argument("width, height, and channels are required for PNG input");
    if (channels != 1 && channels != 2 && channels != 3 && channels != 4)
        throw std::invalid_argument("PNG encoder supports only grayscale, grayscale+alpha, RGB, or RGBA uint8 input");

    size_t expected_size = static_cast<size_t>(width) * height * channels;
    if (pixel_size != expected_size)
        throw std::invalid_argument("PNG input size does not match width * height * channels");
    return encode(pixels, width, height, channels, options);
}

std::vector<uint8_t> encode_buffer(
    const uint8_t* pixels,
    size_t pixel_size,
    uint32_t width,
    uint32_t height,
    uint8_t channels,
    int level,
    const std::string& strategy,
    const std::string& filter,
    size_t idat_chunk_size
) {
    EncodeOptions options;
    options.level = level;
    options.strategy = parse_strategy(strategy);
    options.filter = parse_filter_mode(filter);
    options.idat_chunk_size = idat_chunk_size;
    return encode_buffer(pixels, pixel_size, width, height, channels, options);
}

void save(
    const std::string& path,
    const uint8_t* pixels,
    uint32_t width,
    uint32_t height,
    uint8_t channels,
    const EncodeOptions& options
) {
    std::vector<uint8_t> encoded = encode(pixels, width, height, channels, options);
    write_binary_file(path, encoded);
}

void save_buffer(
    const std::string& path,
    const uint8_t* pixels,
    size_t pixel_size,
    uint32_t width,
    uint32_t height,
    uint8_t channels,
    const EncodeOptions& options
) {
    std::vector<uint8_t> encoded = encode_buffer(pixels, pixel_size, width, height, channels, options);
    write_binary_file(path, encoded);
}

Image decode(const uint8_t* png_data, size_t png_size) {
    if (png_data == nullptr || png_size < PNG_SIGNATURE.size())
        throw std::runtime_error("invalid PNG data");
    if (!std::equal(PNG_SIGNATURE.begin(), PNG_SIGNATURE.end(), png_data))
        throw std::runtime_error("invalid PNG signature");

    uint32_t width = 0;
    uint32_t height = 0;
    uint8_t channels = 0;
    bool seen_ihdr = false;
    bool seen_iend = false;
    std::vector<uint8_t> compressed;

    size_t offset = PNG_SIGNATURE.size();
    while (offset + 12 <= png_size) {
        uint32_t chunk_size = read_be32(png_data + offset);
        offset += 4;
        if (offset + 4 + chunk_size + 4 > png_size)
            throw std::runtime_error("truncated PNG chunk");
        const uint8_t* chunk_type = png_data + offset;
        offset += 4;
        const uint8_t* chunk_data = png_data + offset;
        offset += chunk_size;
        uint32_t stored_crc = read_be32(png_data + offset);
        offset += 4;

        if (chunk_crc(chunk_type, chunk_data, chunk_size) != stored_crc)
            throw std::runtime_error("PNG chunk CRC mismatch");

        std::string_view type(reinterpret_cast<const char*>(chunk_type), 4);
        if (!seen_ihdr && type != "IHDR")
            throw std::runtime_error("PNG IHDR must be the first chunk");

        if (type == "IHDR") {
            if (seen_ihdr)
                throw std::runtime_error("duplicate PNG IHDR chunk");
            if (chunk_size != 13)
                throw std::runtime_error("invalid PNG IHDR size");
            width = read_be32(chunk_data);
            height = read_be32(chunk_data + 4);
            uint8_t bit_depth = chunk_data[8];
            uint8_t color_type = chunk_data[9];
            uint8_t compression_method = chunk_data[10];
            uint8_t filter_method = chunk_data[11];
            uint8_t interlace_method = chunk_data[12];
            if (bit_depth != 8 || compression_method != 0 || filter_method != 0 || interlace_method != 0)
                throw std::runtime_error("unsupported PNG format");
            if (color_type == 0)
                channels = 1;
            else if (color_type == 4)
                channels = 2;
            else if (color_type == 2)
                channels = 3;
            else if (color_type == 6)
                channels = 4;
            else
                throw std::runtime_error("unsupported PNG color type");
            seen_ihdr = true;
        } else if (type == "IDAT") {
            compressed.insert(compressed.end(), chunk_data, chunk_data + chunk_size);
        } else if (type == "IEND") {
            if (chunk_size != 0)
                throw std::runtime_error("invalid PNG IEND size");
            if (offset != png_size)
                throw std::runtime_error("trailing data after PNG IEND");
            seen_iend = true;
            break;
        }
    }

    if (!seen_ihdr || !seen_iend)
        throw std::runtime_error("incomplete PNG data");

    size_t row_bytes = static_cast<size_t>(width) * channels;
    size_t scanline_size = (row_bytes + 1) * static_cast<size_t>(height);
    std::vector<uint8_t> scanlines = zlib_decompress(compressed.data(), compressed.size(), scanline_size);
    std::vector<uint8_t> pixels(row_bytes * static_cast<size_t>(height));

    for (uint32_t row_index = 0; row_index < height; ++row_index) {
        const uint8_t* source = scanlines.data() + static_cast<size_t>(row_index) * (row_bytes + 1);
        uint8_t* row = pixels.data() + static_cast<size_t>(row_index) * row_bytes;
        const uint8_t* previous_row = row_index == 0 ? nullptr : row - row_bytes;
        unfilter_row(source[0], source + 1, previous_row, row_bytes, channels, row);
    }

    return Image{width, height, channels, std::move(pixels)};
}

Image load(const std::string& path) {
    std::vector<uint8_t> data = read_binary_file(path);
    return decode(data.data(), data.size());
}

} // namespace pigzpp::png