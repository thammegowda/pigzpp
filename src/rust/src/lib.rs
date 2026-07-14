//! Rust bindings for **pigzpp** — parallel gzip compression and PNG
//! encode/decode — via its C ABI (`libpigzppc`).
//!
//! The shared library is located by `build.rs` (see `PIGZPP_BUILD_DIR`). All
//! functions run in-process (no fork/pipe) and return owned `Vec<u8>`.

use std::ffi::{CStr, CString};
use std::os::raw::c_char;

#[repr(C)]
struct CBuffer {
    data: *mut u8,
    size: usize,
    error: *const c_char,
}

#[repr(C)]
struct CImage {
    pixels: *mut u8,
    pixel_size: usize,
    width: u32,
    height: u32,
    channels: u8,
    error: *const c_char,
}

extern "C" {
    fn pigzpp_gzip_compress(
        data: *const u8,
        size: usize,
        level: i32,
        threads: i32,
        engine: i32,
    ) -> CBuffer;
    fn pigzpp_gzip_decompress(data: *const u8, size: usize, threads: i32) -> CBuffer;
    fn pigzpp_free(buf: CBuffer);
    fn pigzpp_png_encode(
        pixels: *const u8,
        pixel_size: usize,
        width: u32,
        height: u32,
        channels: u8,
        level: i32,
        strategy: *const c_char,
        filter: *const c_char,
    ) -> CBuffer;
    fn pigzpp_png_decode(data: *const u8, size: usize) -> CImage;
    fn pigzpp_image_free(img: CImage);
}

// -- ZIP C ABI --

#[repr(C)]
struct CZipEntry {
    name: *const c_char,
    compressed_size: u64,
    uncompressed_size: u64,
    crc32: u32,
    method: i32,
    is_dir: i32,
}

enum CZipWriter {}
enum CZipReader {}

extern "C" {
    fn pigzpp_zip_writer_new() -> *mut CZipWriter;
    fn pigzpp_zip_writer_add(
        w: *mut CZipWriter,
        name: *const c_char,
        data: *const u8,
        size: usize,
        method: i32,
        level: i32,
        threads: i32,
        engine: i32,
    ) -> *const c_char;
    fn pigzpp_zip_writer_set_comment(w: *mut CZipWriter, comment: *const c_char);
    fn pigzpp_zip_writer_finish(w: *mut CZipWriter) -> CBuffer;
    fn pigzpp_zip_writer_free(w: *mut CZipWriter);

    fn pigzpp_zip_reader_open(
        data: *const u8,
        size: usize,
        error: *mut *const c_char,
    ) -> *mut CZipReader;
    fn pigzpp_zip_reader_count(r: *const CZipReader) -> usize;
    fn pigzpp_zip_reader_entry(r: *const CZipReader, index: usize, out: *mut CZipEntry) -> i32;
    fn pigzpp_zip_reader_read(r: *const CZipReader, name: *const c_char) -> CBuffer;
    fn pigzpp_zip_reader_comment(r: *const CZipReader) -> *const c_char;
    fn pigzpp_zip_reader_testzip(r: *mut CZipReader) -> *const c_char;
    fn pigzpp_zip_reader_free(r: *mut CZipReader);
}


/// DEFLATE backend engine.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum Engine {
    /// Best available: ISA-L if built in, else zlib-ng.
    Auto = 0,
    /// Force zlib-ng (higher ratio, slower).
    Zlib = 1,
    /// Force ISA-L (faster, lower ratio).
    Isal = 2,
}

/// A decoded image: row-major pixels with `channels` bytes each.
pub struct Image {
    pub width: u32,
    pub height: u32,
    pub channels: u8,
    pub pixels: Vec<u8>,
}

// Copy a C buffer into an owned Vec and release it. Returns Err on C-side error.
fn take_buffer(buf: CBuffer) -> Result<Vec<u8>, String> {
    if !buf.error.is_null() {
        let msg = unsafe { CStr::from_ptr(buf.error) }
            .to_string_lossy()
            .into_owned();
        unsafe { pigzpp_free(buf) };
        return Err(msg);
    }
    let out = if buf.data.is_null() || buf.size == 0 {
        Vec::new()
    } else {
        unsafe { std::slice::from_raw_parts(buf.data, buf.size) }.to_vec()
    };
    unsafe { pigzpp_free(buf) };
    Ok(out)
}

/// Gzip-compress `data`. `level` is 1-9 (or -1 for default); `threads <= 0`
/// uses all cores; `engine` selects the DEFLATE backend.
pub fn compress(data: &[u8], level: i32, threads: i32, engine: Engine) -> Result<Vec<u8>, String> {
    let buf = unsafe {
        pigzpp_gzip_compress(data.as_ptr(), data.len(), level, threads, engine as i32)
    };
    take_buffer(buf)
}

/// Inflate a gzip/zlib stream produced by any standard tool.
pub fn decompress(data: &[u8], threads: i32) -> Result<Vec<u8>, String> {
    let buf = unsafe { pigzpp_gzip_decompress(data.as_ptr(), data.len(), threads) };
    take_buffer(buf)
}

/// Encode raw row-major pixels (`channels` bytes per pixel) to PNG bytes.
pub fn png_encode(
    pixels: &[u8],
    width: u32,
    height: u32,
    channels: u8,
    level: i32,
    strategy: &str,
    filter: &str,
) -> Result<Vec<u8>, String> {
    let strategy = CString::new(strategy).map_err(|e| e.to_string())?;
    let filter = CString::new(filter).map_err(|e| e.to_string())?;
    let buf = unsafe {
        pigzpp_png_encode(
            pixels.as_ptr(),
            pixels.len(),
            width,
            height,
            channels,
            level,
            strategy.as_ptr(),
            filter.as_ptr(),
        )
    };
    take_buffer(buf)
}

/// Decode PNG bytes into a raw [`Image`].
pub fn png_decode(data: &[u8]) -> Result<Image, String> {
    let img = unsafe { pigzpp_png_decode(data.as_ptr(), data.len()) };
    if !img.error.is_null() {
        let msg = unsafe { CStr::from_ptr(img.error) }
            .to_string_lossy()
            .into_owned();
        unsafe { pigzpp_image_free(img) };
        return Err(msg);
    }
    let pixels = if img.pixels.is_null() || img.pixel_size == 0 {
        Vec::new()
    } else {
        unsafe { std::slice::from_raw_parts(img.pixels, img.pixel_size) }.to_vec()
    };
    let out = Image {
        width: img.width,
        height: img.height,
        channels: img.channels,
        pixels,
    };
    unsafe { pigzpp_image_free(img) };
    Ok(out)
}

// ==========================================================================
// ZIP archives
// ==========================================================================

/// ZIP member compression method.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum ZipMethod {
    /// No compression.
    Store = 0,
    /// DEFLATE (default).
    Deflate = 8,
}

/// Options controlling how a member is added.
#[derive(Clone, Copy, Debug)]
pub struct AddOptions {
    pub method: ZipMethod,
    pub level: i32,
    pub threads: i32,
    pub engine: Engine,
}

impl Default for AddOptions {
    fn default() -> Self {
        AddOptions { method: ZipMethod::Deflate, level: 6, threads: 0, engine: Engine::Auto }
    }
}

/// Builds a ZIP archive in memory. Add members, then call [`ZipWriter::finish`].
pub struct ZipWriter {
    handle: *mut CZipWriter,
}

impl ZipWriter {
    /// Create an in-memory ZIP writer.
    pub fn new() -> Result<Self, String> {
        let handle = unsafe { pigzpp_zip_writer_new() };
        if handle.is_null() {
            return Err("pigzpp: cannot create zip writer".into());
        }
        Ok(ZipWriter { handle })
    }

    /// Add `data` as a member named `name`.
    pub fn add(&mut self, name: &str, data: &[u8], opts: AddOptions) -> Result<(), String> {
        let cname = CString::new(name).map_err(|e| e.to_string())?;
        let err = unsafe {
            pigzpp_zip_writer_add(
                self.handle,
                cname.as_ptr(),
                data.as_ptr(),
                data.len(),
                opts.method as i32,
                opts.level,
                opts.threads,
                opts.engine as i32,
            )
        };
        if err.is_null() {
            Ok(())
        } else {
            Err(unsafe { CStr::from_ptr(err) }.to_string_lossy().into_owned())
        }
    }

    /// Set the archive-level comment.
    pub fn set_comment(&mut self, comment: &str) -> Result<(), String> {
        let c = CString::new(comment).map_err(|e| e.to_string())?;
        unsafe { pigzpp_zip_writer_set_comment(self.handle, c.as_ptr()) };
        Ok(())
    }

    /// Finalize and return the archive bytes, consuming the writer.
    pub fn finish(mut self) -> Result<Vec<u8>, String> {
        let handle = self.handle;
        self.handle = std::ptr::null_mut(); // consumed; skip Drop free
        let buf = unsafe { pigzpp_zip_writer_finish(handle) };
        take_buffer(buf)
    }
}

impl Drop for ZipWriter {
    fn drop(&mut self) {
        if !self.handle.is_null() {
            unsafe { pigzpp_zip_writer_free(self.handle) };
        }
    }
}

/// Metadata for a ZIP member.
#[derive(Clone, Debug)]
pub struct ZipEntry {
    pub name: String,
    pub compressed_size: u64,
    pub uncompressed_size: u64,
    pub crc32: u32,
    pub method: i32,
    pub is_dir: bool,
}

/// Reads a ZIP archive held in memory.
pub struct ZipReader {
    handle: *mut CZipReader,
    entries: Vec<ZipEntry>,
}

impl ZipReader {
    /// Open an archive from its bytes (copied internally).
    pub fn open(data: &[u8]) -> Result<Self, String> {
        let mut err: *const c_char = std::ptr::null();
        let handle = unsafe { pigzpp_zip_reader_open(data.as_ptr(), data.len(), &mut err) };
        if handle.is_null() {
            if !err.is_null() {
                return Err(unsafe { CStr::from_ptr(err) }.to_string_lossy().into_owned());
            }
            return Err("pigzpp: cannot open zip archive".into());
        }
        let n = unsafe { pigzpp_zip_reader_count(handle) };
        let mut entries = Vec::with_capacity(n);
        for i in 0..n {
            let mut e = CZipEntry {
                name: std::ptr::null(),
                compressed_size: 0,
                uncompressed_size: 0,
                crc32: 0,
                method: 0,
                is_dir: 0,
            };
            if unsafe { pigzpp_zip_reader_entry(handle, i, &mut e) } == 1 {
                entries.push(ZipEntry {
                    name: unsafe { CStr::from_ptr(e.name) }.to_string_lossy().into_owned(),
                    compressed_size: e.compressed_size,
                    uncompressed_size: e.uncompressed_size,
                    crc32: e.crc32,
                    method: e.method,
                    is_dir: e.is_dir != 0,
                });
            }
        }
        Ok(ZipReader { handle, entries })
    }

    /// Metadata for all members.
    pub fn entries(&self) -> &[ZipEntry] {
        &self.entries
    }

    /// Member names in archive order.
    pub fn names(&self) -> Vec<String> {
        self.entries.iter().map(|e| e.name.clone()).collect()
    }

    /// Decompress and return the member named `name`.
    pub fn read(&self, name: &str) -> Result<Vec<u8>, String> {
        let cname = CString::new(name).map_err(|e| e.to_string())?;
        let buf = unsafe { pigzpp_zip_reader_read(self.handle, cname.as_ptr()) };
        take_buffer(buf)
    }

    /// Archive-level comment.
    pub fn comment(&self) -> String {
        let c = unsafe { pigzpp_zip_reader_comment(self.handle) };
        if c.is_null() {
            String::new()
        } else {
            unsafe { CStr::from_ptr(c) }.to_string_lossy().into_owned()
        }
    }

    /// Verify every member's CRC. Returns the name of the first corrupt member,
    /// or `None` if the archive is intact.
    pub fn testzip(&mut self) -> Option<String> {
        let c = unsafe { pigzpp_zip_reader_testzip(self.handle) };
        if c.is_null() {
            None
        } else {
            Some(unsafe { CStr::from_ptr(c) }.to_string_lossy().into_owned())
        }
    }
}

impl Drop for ZipReader {
    fn drop(&mut self) {
        if !self.handle.is_null() {
            unsafe { pigzpp_zip_reader_free(self.handle) };
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn gzip_roundtrip() {
        let data = b"hello pigzpp from rust ".repeat(10_000);
        for eng in [Engine::Auto, Engine::Isal, Engine::Zlib] {
            let gz = compress(&data, 6, 4, eng).unwrap();
            assert_eq!(decompress(&gz, 4).unwrap(), data);
        }
    }

    #[test]
    fn png_roundtrip() {
        let (w, h, ch) = (32u32, 24u32, 3u8);
        let px: Vec<u8> = (0..(w * h * ch as u32)).map(|i| i as u8).collect();
        let png = png_encode(&px, w, h, ch, 6, "rle", "up").unwrap();
        assert_eq!(&png[..4], &[0x89, b'P', b'N', b'G']);
        let img = png_decode(&png).unwrap();
        assert_eq!((img.width, img.height, img.channels), (w, h, ch));
        assert_eq!(img.pixels, px);
    }

    #[test]
    fn zip_roundtrip() {
        let payload = b"the quick brown fox ".repeat(50_000);
        let mut w = ZipWriter::new().unwrap();
        w.add("hello.txt", b"hello world", AddOptions::default()).unwrap();
        w.add("data.bin", &payload, AddOptions { threads: 4, ..Default::default() }).unwrap();
        w.add("stored.txt", b"raw", AddOptions { method: ZipMethod::Store, ..Default::default() })
            .unwrap();
        w.set_comment("rust made this").unwrap();
        let archive = w.finish().unwrap();

        let mut r = ZipReader::open(&archive).unwrap();
        assert_eq!(r.entries().len(), 3);
        assert_eq!(r.testzip(), None);
        assert_eq!(r.comment(), "rust made this");
        assert_eq!(r.read("hello.txt").unwrap(), b"hello world");
        assert_eq!(r.read("data.bin").unwrap(), payload);
        assert_eq!(r.read("stored.txt").unwrap(), b"raw");
    }
}

