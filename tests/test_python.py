"""Tests for pigzpp Python bindings."""

import gzip
import os
import tempfile
import zlib

import pytest

import pigzpp


class TestCompressDecompress:
    """Tests for the in-memory compress/decompress API."""

    def test_roundtrip(self):
        data = b"hello world"
        compressed = pigzpp.compress(data)
        assert pigzpp.decompress(compressed) == data

    def test_roundtrip_large(self):
        data = os.urandom(1_000_000)
        assert pigzpp.decompress(pigzpp.compress(data)) == data

    def test_empty(self):
        compressed = pigzpp.compress(b"")
        assert pigzpp.decompress(compressed) == b""

    def test_compress_produces_valid_gzip(self):
        data = b"test data for gzip compat"
        compressed = pigzpp.compress(data)
        assert gzip.decompress(compressed) == data

    def test_compress_zlib_produces_valid_zlib(self):
        data = b"test data for zlib compat" * 100
        compressed = pigzpp.compress_zlib(data, level=1)
        assert zlib.decompress(compressed) == data

    def test_compress_raw_produces_valid_raw_deflate(self):
        data = b"test data for raw deflate compat" * 100
        compressed = pigzpp.compress_raw(data, level=1)
        assert zlib.decompress(compressed, wbits=-15) == data

    def test_decompress_stdlib_gzip(self):
        data = b"created by stdlib gzip"
        compressed = gzip.compress(data)
        assert pigzpp.decompress(compressed) == data

    def test_compress_levels(self):
        data = b"x" * 10000
        sizes = {}
        for level in (1, 6, 9):
            sizes[level] = len(pigzpp.compress(data, level=level))
        # Level 9 should be at most as large as level 1
        assert sizes[9] <= sizes[1]

    def test_decompress_invalid_data(self):
        with pytest.raises(RuntimeError):
            pigzpp.decompress(b"not gzip data")


class TestFileAPI:
    """Tests for the file-based open() context manager API."""

    def test_write_read_roundtrip(self, tmp_path):
        path = str(tmp_path / "test.gz")
        text = "hello pigzpp\n"

        with pigzpp.open(path, "w") as f:
            f.write(text)

        with pigzpp.open(path, "r") as f:
            assert f.read() == text

    def test_write_readable_by_gzip(self, tmp_path):
        path = str(tmp_path / "test.gz")
        data = "gzip compat test\n"

        with pigzpp.open(path, "w") as f:
            f.write(data)

        with gzip.open(path, "rt") as f:
            assert f.read() == data

    def test_read_gzip_written_file(self, tmp_path):
        path = str(tmp_path / "test.gz")
        data = "written by stdlib\n"

        with gzip.open(path, "wt") as f:
            f.write(data)

        with pigzpp.open(path, "r") as f:
            assert f.read() == data

    def test_large_file(self, tmp_path):
        path = str(tmp_path / "large.gz")
        line = "A" * 999 + "\n"
        text = line * 1000  # ~1MB

        with pigzpp.open(path, "w") as f:
            f.write(text)

        with pigzpp.open(path, "r") as f:
            assert f.read() == text

    def test_line_iteration(self, tmp_path):
        path = str(tmp_path / "lines.gz")
        lines = [f"line {i}\n" for i in range(100)]
        text = "".join(lines)

        with pigzpp.open(path, "w") as f:
            f.write(text)

        with pigzpp.open(path, "r") as f:
            read_lines = list(f)

        assert read_lines == lines

    def test_invalid_mode(self):
        with pytest.raises(ValueError):
            pigzpp.open("/dev/null", "x")

    def test_read_nonexistent_file(self):
        with pytest.raises(RuntimeError):
            with pigzpp.open("/nonexistent/path.gz", "r") as f:
                f.read()

    def test_compression_level(self, tmp_path):
        path1 = str(tmp_path / "level1.gz")
        path9 = str(tmp_path / "level9.gz")
        data = "abcdef" * 10000

        with pigzpp.open(path1, "w", level=1) as f:
            f.write(data)
        with pigzpp.open(path9, "w", level=9) as f:
            f.write(data)

        assert os.path.getsize(path9) <= os.path.getsize(path1)

    def test_multiple_writes(self, tmp_path):
        path = str(tmp_path / "multi.gz")
        with pigzpp.open(path, "w") as f:
            f.write("part1 ")
            f.write("part2 ")
            f.write("part3")

        with pigzpp.open(path, "r") as f:
            assert f.read() == "part1 part2 part3"
