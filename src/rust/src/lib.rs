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
}

