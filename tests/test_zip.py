"""Tests for pigzpp.ZipFile (the zipfile-like API), including interop with the
standard library's zipfile module."""

import os
import tempfile
import zipfile

import pytest

import pigzpp


@pytest.fixture
def tmp_zip():
    d = tempfile.mkdtemp()
    yield os.path.join(d, "test.zip")


class TestWriteRead:
    def test_roundtrip_bytes_and_str(self, tmp_zip):
        with pigzpp.ZipFile(tmp_zip, "w") as z:
            z.writestr("a.txt", "hello world")
            z.writestr("b.bin", b"\x00\x01\x02\x03" * 1000)
        with pigzpp.ZipFile(tmp_zip) as z:
            assert set(z.namelist()) == {"a.txt", "b.bin"}
            assert z.read("a.txt") == b"hello world"
            assert z.read("b.bin") == b"\x00\x01\x02\x03" * 1000

    def test_stored_method(self, tmp_zip):
        with pigzpp.ZipFile(tmp_zip, "w") as z:
            z.writestr("s.txt", "no compression", compress_type=pigzpp.ZIP_STORED)
        with pigzpp.ZipFile(tmp_zip) as z:
            info = z.getinfo("s.txt")
            assert info.compress_type == pigzpp.ZIP_STORED
            assert info.compress_size == info.file_size
            assert z.read("s.txt") == b"no compression"

    def test_deflate_compresses(self, tmp_zip):
        payload = b"the quick brown fox " * 50_000
        with pigzpp.ZipFile(tmp_zip, "w") as z:
            z.writestr("big.txt", payload)
        with pigzpp.ZipFile(tmp_zip) as z:
            info = z.getinfo("big.txt")
            assert info.compress_type == pigzpp.ZIP_DEFLATED
            assert info.compress_size < info.file_size
            assert z.read("big.txt") == payload

    def test_empty_member(self, tmp_zip):
        with pigzpp.ZipFile(tmp_zip, "w") as z:
            z.writestr("empty.txt", "")
        with pigzpp.ZipFile(tmp_zip) as z:
            assert z.read("empty.txt") == b""

    def test_comment(self, tmp_zip):
        with pigzpp.ZipFile(tmp_zip, "w") as z:
            z.writestr("a.txt", "x")
            z.setcomment("archive comment")
        with pigzpp.ZipFile(tmp_zip) as z:
            assert z.comment == b"archive comment"

    def test_testzip_clean(self, tmp_zip):
        with pigzpp.ZipFile(tmp_zip, "w") as z:
            z.writestr("a.txt", "data")
        with pigzpp.ZipFile(tmp_zip) as z:
            assert z.testzip() is None


class TestThreadsAndEngines:
    @pytest.mark.parametrize("engine", ["auto", "isal", "zlib"])
    def test_parallel_roundtrip(self, tmp_zip, engine):
        payload = os.urandom(2_000_000)
        with pigzpp.ZipFile(tmp_zip, "w", threads=4, engine=engine) as z:
            z.writestr("data.bin", payload)
        with pigzpp.ZipFile(tmp_zip) as z:
            assert z.read("data.bin") == payload
            assert z.testzip() is None


class TestAppendAndModes:
    def test_append(self, tmp_zip):
        with pigzpp.ZipFile(tmp_zip, "w") as z:
            z.writestr("orig.txt", "original")
        with pigzpp.ZipFile(tmp_zip, "a") as z:
            z.writestr("new.txt", "added")
        with pigzpp.ZipFile(tmp_zip) as z:
            assert z.namelist() == ["orig.txt", "new.txt"]
            assert z.read("orig.txt") == b"original"
            assert z.read("new.txt") == b"added"

    def test_exclusive_create_fails_if_exists(self, tmp_zip):
        with pigzpp.ZipFile(tmp_zip, "w") as z:
            z.writestr("a.txt", "x")
        with pytest.raises(Exception):
            pigzpp.ZipFile(tmp_zip, "x")

    def test_bad_mode(self, tmp_zip):
        with pytest.raises(Exception):
            pigzpp.ZipFile(tmp_zip, "q")


class TestExtract:
    def test_extractall(self, tmp_zip):
        with pigzpp.ZipFile(tmp_zip, "w") as z:
            z.writestr("top.txt", "top")
            z.writestr("sub/inner.txt", "inner")
        out = tempfile.mkdtemp()
        with pigzpp.ZipFile(tmp_zip) as z:
            z.extractall(out)
        assert open(os.path.join(out, "top.txt")).read() == "top"
        assert open(os.path.join(out, "sub", "inner.txt")).read() == "inner"

    def test_extract_single(self, tmp_zip):
        with pigzpp.ZipFile(tmp_zip, "w") as z:
            z.writestr("only.txt", "content")
        out = tempfile.mkdtemp()
        with pigzpp.ZipFile(tmp_zip) as z:
            path = z.extract("only.txt", out)
        assert open(path).read() == "content"


class TestStdlibInterop:
    def test_stdlib_reads_pigzpp(self, tmp_zip):
        payload = b"interop payload " * 10_000
        with pigzpp.ZipFile(tmp_zip, "w") as z:
            z.writestr("a.txt", "hello")
            z.writestr("b.bin", payload)
            z.writestr("s.txt", "stored", compress_type=pigzpp.ZIP_STORED)
            z.setcomment("pigzpp made this")
        with zipfile.ZipFile(tmp_zip) as z:
            assert z.testzip() is None
            assert z.read("a.txt") == b"hello"
            assert z.read("b.bin") == payload
            assert z.read("s.txt") == b"stored"
            assert z.comment == b"pigzpp made this"

    def test_pigzpp_reads_stdlib(self, tmp_zip):
        payload = b"reverse interop " * 10_000
        with zipfile.ZipFile(tmp_zip, "w", zipfile.ZIP_DEFLATED) as z:
            z.writestr("x.txt", "from stdlib")
            z.writestr("y.bin", payload)
            z.writestr("z.txt", "stored", zipfile.ZIP_STORED)
        with pigzpp.ZipFile(tmp_zip) as z:
            assert z.read("x.txt") == b"from stdlib"
            assert z.read("y.bin") == payload
            assert z.read("z.txt") == b"stored"
            assert z.testzip() is None

    def test_write_file_from_disk(self, tmp_zip):
        src = tempfile.NamedTemporaryFile(delete=False)
        src.write(b"file on disk content")
        src.close()
        with pigzpp.ZipFile(tmp_zip, "w") as z:
            z.write(src.name, arcname="stored_name.txt")
        with zipfile.ZipFile(tmp_zip) as z:
            assert z.read("stored_name.txt") == b"file on disk content"
        os.unlink(src.name)
