"""Tests for pigzpp.png PNG encode/decode bindings."""

from __future__ import annotations

import io
import gc

import pytest

import pigzpp


def _rgb_pixels(width: int, height: int) -> bytes:
    data = bytearray(width * height * 3)
    offset = 0
    for row_index in range(height):
        for column_index in range(width):
            data[offset] = (column_index * 7 + row_index * 3) & 0xFF
            data[offset + 1] = (column_index * 5) & 0xFF
            data[offset + 2] = (row_index * 11) & 0xFF
            offset += 3
    return bytes(data)


def _rgba_pixels(width: int, height: int) -> bytes:
    data = bytearray(width * height * 4)
    offset = 0
    for row_index in range(height):
        for column_index in range(width):
            data[offset] = (column_index * 13) & 0xFF
            data[offset + 1] = (row_index * 17) & 0xFF
            data[offset + 2] = (column_index + row_index) & 0xFF
            data[offset + 3] = 255 - ((column_index * 3 + row_index) & 0x7F)
            offset += 4
    return bytes(data)


def _gray_pixels(width: int, height: int) -> bytes:
    data = bytearray(width * height)
    offset = 0
    for row_index in range(height):
        for column_index in range(width):
            data[offset] = (column_index * 9 + row_index * 5) & 0xFF
            offset += 1
    return bytes(data)


def _gray_alpha_pixels(width: int, height: int) -> bytes:
    data = bytearray(width * height * 2)
    offset = 0
    for row_index in range(height):
        for column_index in range(width):
            data[offset] = (column_index * 9 + row_index * 5) & 0xFF
            data[offset + 1] = 255 - ((column_index * 3 + row_index * 7) & 0x7F)
            offset += 2
    return bytes(data)


def _pixels_for_channels(width: int, height: int, channels: int) -> bytes:
    if channels == 1:
        return _gray_pixels(width, height)
    if channels == 2:
        return _gray_alpha_pixels(width, height)
    if channels == 3:
        return _rgb_pixels(width, height)
    if channels == 4:
        return _rgba_pixels(width, height)
    raise ValueError(channels)


def _pillow_mode_for_channels(channels: int) -> str:
    if channels == 1:
        return "L"
    if channels == 2:
        return "LA"
    if channels == 3:
        return "RGB"
    if channels == 4:
        return "RGBA"
    raise ValueError(channels)


def _png_chunks(encoded: bytes) -> list[bytes]:
    offset = 8
    chunks = []
    while offset < len(encoded):
        chunk_size = int.from_bytes(encoded[offset:offset + 4], "big")
        chunk_end = offset + 12 + chunk_size
        chunks.append(encoded[offset:chunk_end])
        offset = chunk_end
    return chunks


def _png_chunk_type(chunk: bytes) -> bytes:
    return chunk[4:8]


def test_png_grayscale_roundtrip():
    width, height = 29, 23
    pixels = _gray_pixels(width, height)
    encoded = pigzpp.png.compress(pixels, width=width, height=height, channels=1)
    decoded, shape = pigzpp.png.decompress(encoded)

    assert shape == (width, height, 1)
    assert decoded == pixels


def test_png_rgb_roundtrip():
    width, height = 32, 24
    pixels = _rgb_pixels(width, height)
    encoded = pigzpp.png.compress(pixels, width=width, height=height, channels=3)
    decoded, shape = pigzpp.png.decompress(encoded)

    assert encoded.startswith(b"\x89PNG\r\n\x1a\n")
    assert shape == (width, height, 3)
    assert decoded == pixels


def test_png_rgba_roundtrip():
    width, height = 19, 17
    pixels = _rgba_pixels(width, height)
    encoded = pigzpp.png.compress(
        pixels,
        width=width,
        height=height,
        channels=4,
        level=1,
        strategy="rle",
        filter="adaptive-all",
    )
    decoded, shape = pigzpp.png.decompress(encoded)

    assert shape == (width, height, 4)
    assert decoded == pixels


def test_png_grayscale_alpha_roundtrip():
    width, height = 31, 21
    pixels = _gray_alpha_pixels(width, height)
    encoded = pigzpp.png.compress(pixels, width=width, height=height, channels=2)
    decoded, shape = pigzpp.png.decompress(encoded)

    assert shape == (width, height, 2)
    assert decoded == pixels


def test_png_preset_balanced_matches_explicit_options():
    width, height = 32, 24
    pixels = _rgb_pixels(width, height)
    explicit = pigzpp.png.compress(
        pixels,
        width=width,
        height=height,
        channels=3,
        level=1,
        strategy="rle",
        filter="adaptive-fast",
    )
    preset = pigzpp.png.compress(pixels, width=width, height=height, channels=3, preset="balanced")

    assert preset == explicit


def test_png_preset_fast_matches_explicit_options():
    width, height = 32, 24
    pixels = _rgb_pixels(width, height)
    explicit = pigzpp.png.compress(
        pixels,
        width=width,
        height=height,
        channels=3,
        level=1,
        strategy="rle",
        filter="up",
    )
    preset = pigzpp.png.compress(pixels, width=width, height=height, channels=3, preset="fast")

    assert preset == explicit


def test_png_preset_small_matches_explicit_options():
    width, height = 32, 24
    pixels = _rgb_pixels(width, height)
    explicit = pigzpp.png.compress(
        pixels,
        width=width,
        height=height,
        channels=3,
        level=9,
        strategy="filtered",
        filter="adaptive-all",
    )
    preset = pigzpp.png.compress(pixels, width=width, height=height, channels=3, preset="small")

    assert preset == explicit


def test_png_save_load_roundtrip(tmp_path):
    np = pytest.importorskip("numpy")
    width, height = 17, 15
    pixels = np.frombuffer(_gray_pixels(width, height), dtype=np.uint8).reshape(height, width)
    path = tmp_path / "gray.png"

    pigzpp.png.save(path, pixels, preset="balanced")
    decoded, shape = pigzpp.png.load(path, result="bytes")

    assert shape == (width, height, 1)
    assert decoded == pixels.tobytes()


def test_png_decompress_array_rgb_roundtrip():
    np = pytest.importorskip("numpy")
    width, height = 11, 13
    pixels = np.frombuffer(_rgb_pixels(width, height), dtype=np.uint8).reshape(height, width, 3)
    encoded = pigzpp.png.compress(pixels, preset="fast")

    decoded = pigzpp.png.decompress_array(encoded)

    assert decoded.dtype == np.uint8
    assert decoded.shape == (height, width, 3)
    assert np.array_equal(decoded, pixels)


def test_png_array_owns_decoded_pixels():
    np = pytest.importorskip("numpy")
    width, height = 23, 19
    expected = np.frombuffer(_rgb_pixels(width, height), dtype=np.uint8).reshape(height, width, 3)
    encoded = pigzpp.png.compress(expected)

    decoded = pigzpp.png.decompress_array(encoded)
    del encoded
    gc.collect()

    # Allocate enough unrelated memory to expose a dangling capsule/view.
    _ = [bytearray(decoded.nbytes) for _ in range(16)]
    assert decoded.flags.c_contiguous
    assert decoded.dtype == np.uint8
    assert np.array_equal(decoded, expected)


def test_png_decompress_result_numpy_roundtrip():
    np = pytest.importorskip("numpy")
    width, height = 11, 13
    pixels = np.frombuffer(_rgb_pixels(width, height), dtype=np.uint8).reshape(height, width, 3)
    encoded = pigzpp.png.compress(pixels, preset="fast")

    decoded = pigzpp.png.decompress(encoded, result="numpy")

    assert decoded.dtype == np.uint8
    assert decoded.shape == (height, width, 3)
    assert np.array_equal(decoded, pixels)


def test_png_decompress_array_grayscale_roundtrip():
    np = pytest.importorskip("numpy")
    width, height = 17, 15
    pixels = np.frombuffer(_gray_pixels(width, height), dtype=np.uint8).reshape(height, width)
    encoded = pigzpp.png.compress(pixels, preset="balanced")

    decoded = pigzpp.png.decompress_array(encoded)

    assert decoded.dtype == np.uint8
    assert decoded.shape == (height, width)
    assert np.array_equal(decoded, pixels)


def test_png_load_array_roundtrip(tmp_path):
    np = pytest.importorskip("numpy")
    width, height = 9, 7
    pixels = np.frombuffer(_rgba_pixels(width, height), dtype=np.uint8).reshape(height, width, 4)
    path = tmp_path / "rgba.png"

    pigzpp.png.save(path, pixels, preset="small")
    decoded = pigzpp.png.load_array(path)

    assert decoded.dtype == np.uint8
    assert decoded.shape == (height, width, 4)
    assert np.array_equal(decoded, pixels)


def test_png_load_defaults_to_numpy(tmp_path):
    np = pytest.importorskip("numpy")
    width, height = 9, 7
    pixels = np.frombuffer(_gray_pixels(width, height), dtype=np.uint8).reshape(height, width)
    path = tmp_path / "gray.png"

    pigzpp.png.save(path, pixels, preset="fast")
    decoded = pigzpp.png.load(path)

    assert decoded.dtype == np.uint8
    assert decoded.shape == (height, width)
    assert np.array_equal(decoded, pixels)


def test_png_rejects_bad_chunk_crc():
    width, height = 32, 24
    pixels = _rgb_pixels(width, height)
    encoded = bytearray(pigzpp.png.compress(pixels, width=width, height=height, channels=3))
    encoded[-1] ^= 0x01

    with pytest.raises(RuntimeError, match="CRC"):
        pigzpp.png.decompress(encoded)


def test_png_level_zero_uses_stored_deflate():
    width, height = 128, 128
    pixels = b"\x00" * (width * height)
    stored = pigzpp.png.compress(pixels, width=width, height=height, channels=1, level=0, filter="none")
    compressed = pigzpp.png.compress(pixels, width=width, height=height, channels=1, level=1, filter="none")

    assert len(stored) > len(compressed) * 4
    decoded, shape = pigzpp.png.decompress(stored)
    assert shape == (width, height, 1)
    assert decoded == pixels


def test_png_rejects_trailing_data_after_iend():
    width, height = 32, 24
    pixels = _rgb_pixels(width, height)
    encoded = pigzpp.png.compress(pixels, width=width, height=height, channels=3) + b"extra"

    with pytest.raises(RuntimeError, match="trailing"):
        pigzpp.png.decompress(encoded)


def test_png_rejects_idat_before_ihdr():
    width, height = 32, 24
    pixels = _rgb_pixels(width, height)
    encoded = pigzpp.png.compress(pixels, width=width, height=height, channels=3)
    chunks = _png_chunks(encoded)
    idat_index = next(index for index, chunk in enumerate(chunks) if _png_chunk_type(chunk) == b"IDAT")
    bad_order = encoded[:8] + chunks[idat_index] + chunks[0] + b"".join(chunks[1:idat_index] + chunks[idat_index + 1:])

    with pytest.raises(RuntimeError, match="IHDR"):
        pigzpp.png.decompress(bad_order)


def test_png_pillow_can_decode_output():
    pillow_image = pytest.importorskip("PIL.Image")
    width, height = 32, 24
    pixels = _rgb_pixels(width, height)
    encoded = pigzpp.png.compress(pixels, width=width, height=height, channels=3)

    with pillow_image.open(io.BytesIO(encoded)) as image:
        assert image.mode == "RGB"
        assert image.size == (width, height)
        assert image.tobytes() == pixels


def test_png_pillow_can_decode_grayscale_output():
    pillow_image = pytest.importorskip("PIL.Image")
    width, height = 29, 23
    pixels = _gray_pixels(width, height)
    encoded = pigzpp.png.compress(pixels, width=width, height=height, channels=1)

    with pillow_image.open(io.BytesIO(encoded)) as image:
        assert image.mode == "L"
        assert image.size == (width, height)
        assert image.tobytes() == pixels


@pytest.mark.parametrize("channels", [1, 2, 3, 4])
def test_png_pigzpp_to_pillow_interop_exact_pixels(channels: int):
    pillow_image = pytest.importorskip("PIL.Image")
    width, height = 31, 23
    pixels = _pixels_for_channels(width, height, channels)
    encoded = pigzpp.png.compress(pixels, width=width, height=height, channels=channels, preset="balanced")

    with pillow_image.open(io.BytesIO(encoded)) as image:
        assert image.mode == _pillow_mode_for_channels(channels)
        assert image.size == (width, height)
        assert image.tobytes() == pixels


@pytest.mark.parametrize("channels", [1, 2, 3, 4])
def test_png_pillow_to_pigzpp_interop_exact_pixels(channels: int):
    pillow_image = pytest.importorskip("PIL.Image")
    width, height = 31, 23
    pixels = _pixels_for_channels(width, height, channels)
    image = pillow_image.frombytes(_pillow_mode_for_channels(channels), (width, height), pixels)
    output = io.BytesIO()
    image.save(output, format="PNG")

    decoded, shape = pigzpp.png.decompress(output.getvalue())

    assert shape == (width, height, channels)
    assert decoded == pixels


def test_png_compress_infers_numpy_rgb_shape():
    np = pytest.importorskip("numpy")
    width, height = 11, 13
    pixels = np.frombuffer(_rgb_pixels(width, height), dtype=np.uint8).reshape(height, width, 3)

    encoded = pigzpp.png.compress(pixels, filter="sub")
    decoded, shape = pigzpp.png.decompress(encoded)

    assert shape == (width, height, 3)
    assert decoded == pixels.tobytes()


def test_png_compress_infers_numpy_grayscale_shape():
    np = pytest.importorskip("numpy")
    width, height = 17, 15
    pixels = np.frombuffer(_gray_pixels(width, height), dtype=np.uint8).reshape(height, width)

    encoded = pigzpp.png.compress(pixels, filter="up")
    decoded, shape = pigzpp.png.decompress(encoded)

    assert shape == (width, height, 1)
    assert decoded == pixels.tobytes()


def test_png_rejects_unsupported_channels():
    with pytest.raises(ValueError):
        pigzpp.png.compress(b"\x00" * 80, width=4, height=4, channels=5)


def test_png_rejects_non_contiguous_byte_buffer():
    pixels = memoryview(bytearray(range(64)))[::2]
    with pytest.raises(ValueError, match="C-contiguous"):
        pigzpp.png.compress(pixels, width=8, height=4, channels=1)