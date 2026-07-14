// Package pigzpp binds pigzpp's C ABI (libpigzppc) directly into Go via cgo,
// running compression and PNG encode/decode in-process with no fork/exec/pipe
// overhead.
//
// Build requirements:
//   - Build the shared library first, from the repository root:
//       cmake -DPIGZPP_BUILD_CAPI=ON -S . -B build && cmake --build build --target pigzppc
//   - The cgo directives below locate capi.h in ../pigzpp and link against
//     ../../build/libpigzppc.so relative to this file (${SRCDIR}).
//
// At run time the loader must find libpigzppc.so. The LDFLAGS set an rpath to
// the build directory, so running from the source tree works without setting
// LD_LIBRARY_PATH. For installed use, place libpigzppc.so on the loader path.
package pigzpp

/*
#cgo CPPFLAGS: -I${SRCDIR}/../pigzpp
#cgo LDFLAGS: -L${SRCDIR}/../../build -Wl,-rpath,${SRCDIR}/../../build -lpigzppc
#include <stdlib.h>
#include "capi.h"
*/
import "C"

import (
	"errors"
	"runtime"
	"unsafe"
)

// Engine selects the DEFLATE backend.
type Engine int

const (
	EngineAuto Engine = iota // best available (ISA-L if built in, else zlib-ng)
	EngineZlib               // force zlib-ng (higher ratio, slower)
	EngineIsal               // force ISA-L (faster, lower ratio)
)

// Compress gzip-compresses src using pigzpp with the default (auto) backend.
// level is 1-9 (or -1 for the library default); threads <= 0 uses all cores.
func Compress(src []byte, level, threads int) ([]byte, error) {
	return CompressEngine(src, level, threads, EngineAuto)
}

// CompressEngine is Compress with an explicit backend engine.
func CompressEngine(src []byte, level, threads int, engine Engine) ([]byte, error) {
	return call(src, level, threads, engine, true)
}

// CompressOwned gzip-compresses src (auto backend) and returns a byte slice
// that ALIASES the C-allocated output buffer with no copy, plus a release
// function that MUST be called once the slice is no longer needed. This is the
// fully zero-copy output path (no GoBytes copy). The slice must not be used
// after release.
func CompressOwned(src []byte, level, threads int) (data []byte, release func(), err error) {
	return CompressOwnedEngine(src, level, threads, EngineAuto)
}

// CompressOwnedEngine is CompressOwned with an explicit backend engine.
func CompressOwnedEngine(src []byte, level, threads int, engine Engine) (data []byte, release func(), err error) {
	var ptr *C.uint8_t
	if len(src) > 0 {
		ptr = (*C.uint8_t)(unsafe.Pointer(&src[0]))
	}
	buf := C.pigzpp_gzip_compress(ptr, C.size_t(len(src)), C.int(level), C.int(threads), C.int(engine))
	runtime.KeepAlive(src)

	if buf.error != nil {
		msg := C.GoString(buf.error)
		C.pigzpp_free(buf)
		return nil, nil, errors.New(msg)
	}
	free := func() { C.pigzpp_free(buf) }
	if buf.data == nil || buf.size == 0 {
		return nil, free, nil
	}
	out := unsafe.Slice((*byte)(unsafe.Pointer(buf.data)), int(buf.size))
	return out, free, nil
}

// Decompress inflates a gzip/zlib stream produced by any standard tool.
// threads <= 0 uses all available cores.
func Decompress(src []byte, threads int) ([]byte, error) {
	return call(src, 0, threads, EngineAuto, false)
}

func call(src []byte, level, threads int, engine Engine, compress bool) ([]byte, error) {
	var ptr *C.uint8_t
	if len(src) > 0 {
		ptr = (*C.uint8_t)(unsafe.Pointer(&src[0]))
	}

	var buf C.pigzpp_buffer
	if compress {
		buf = C.pigzpp_gzip_compress(ptr, C.size_t(len(src)), C.int(level), C.int(threads), C.int(engine))
	} else {
		buf = C.pigzpp_gzip_decompress(ptr, C.size_t(len(src)), C.int(threads))
	}
	// Keep src alive across the cgo call.
	runtime.KeepAlive(src)

	if buf.error != nil {
		msg := C.GoString(buf.error)
		C.pigzpp_free(buf)
		return nil, errors.New(msg)
	}
	defer C.pigzpp_free(buf)

	if buf.data == nil {
		return nil, errors.New("pigzpp: null result")
	}
	// Copy out of the C allocation into Go-managed memory.
	out := C.GoBytes(unsafe.Pointer(buf.data), C.int(buf.size))
	return out, nil
}

// Image is a decoded raster image: row-major pixels with Channels bytes each.
type Image struct {
	Width    uint32
	Height   uint32
	Channels uint32
	Pixels   []byte
}

// PngEncode encodes raw row-major pixels (channels bytes per pixel) to PNG
// bytes. level is 1-9; strategy and filter are option names (empty = default).
func PngEncode(pixels []byte, width, height, channels uint32, level int, strategy, filter string) ([]byte, error) {
	var ptr *C.uint8_t
	if len(pixels) > 0 {
		ptr = (*C.uint8_t)(unsafe.Pointer(&pixels[0]))
	}
	var cs, cf *C.char
	if strategy != "" {
		cs = C.CString(strategy)
		defer C.free(unsafe.Pointer(cs))
	}
	if filter != "" {
		cf = C.CString(filter)
		defer C.free(unsafe.Pointer(cf))
	}
	buf := C.pigzpp_png_encode(ptr, C.size_t(len(pixels)),
		C.uint32_t(width), C.uint32_t(height), C.uint8_t(channels),
		C.int(level), cs, cf)
	runtime.KeepAlive(pixels)

	if buf.error != nil {
		msg := C.GoString(buf.error)
		C.pigzpp_free(buf)
		return nil, errors.New(msg)
	}
	defer C.pigzpp_free(buf)
	return C.GoBytes(unsafe.Pointer(buf.data), C.int(buf.size)), nil
}

// PngDecode decodes PNG bytes into a raw Image.
func PngDecode(data []byte) (Image, error) {
	var ptr *C.uint8_t
	if len(data) > 0 {
		ptr = (*C.uint8_t)(unsafe.Pointer(&data[0]))
	}
	img := C.pigzpp_png_decode(ptr, C.size_t(len(data)))
	runtime.KeepAlive(data)

	if img.error != nil {
		msg := C.GoString(img.error)
		C.pigzpp_image_free(img)
		return Image{}, errors.New(msg)
	}
	defer C.pigzpp_image_free(img)
	return Image{
		Width:    uint32(img.width),
		Height:   uint32(img.height),
		Channels: uint32(img.channels),
		Pixels:   C.GoBytes(unsafe.Pointer(img.pixels), C.int(img.pixel_size)),
	}, nil
}
