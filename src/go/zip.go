package pigzpp

/*
#include <stdlib.h>
#include "capi.h"
*/
import "C"

import (
	"errors"
	"runtime"
	"unsafe"
)

// ZipMethod is a ZIP member compression method.
type ZipMethod int

const (
	ZipStore   ZipMethod = 0
	ZipDeflate ZipMethod = 8
)

// ZipWriter builds a ZIP archive in memory. Add members with Add, then call
// Finish to obtain the archive bytes. A ZipWriter must not be used after
// Finish or Close.
type ZipWriter struct {
	h *C.pigzpp_zip_writer
}

// NewZipWriter creates an in-memory ZIP writer.
func NewZipWriter() (*ZipWriter, error) {
	h := C.pigzpp_zip_writer_new()
	if h == nil {
		return nil, errors.New("pigzpp: cannot create zip writer")
	}
	return &ZipWriter{h: h}, nil
}

// AddOptions controls how a member is compressed.
type AddOptions struct {
	Method  ZipMethod
	Level   int
	Threads int
	Engine  Engine
}

// DefaultAddOptions returns level-6 parallel DEFLATE with the auto engine.
func DefaultAddOptions() AddOptions {
	return AddOptions{Method: ZipDeflate, Level: 6, Threads: 0, Engine: EngineAuto}
}

// Add stores data as a member named name using the given options.
func (w *ZipWriter) Add(name string, data []byte, opts AddOptions) error {
	if w.h == nil {
		return errors.New("pigzpp: zip writer is closed")
	}
	cname := C.CString(name)
	defer C.free(unsafe.Pointer(cname))
	var ptr *C.uint8_t
	if len(data) > 0 {
		ptr = (*C.uint8_t)(unsafe.Pointer(&data[0]))
	}
	cerr := C.pigzpp_zip_writer_add(w.h, cname, ptr, C.size_t(len(data)),
		C.int(opts.Method), C.int(opts.Level), C.int(opts.Threads), C.int(opts.Engine))
	runtime.KeepAlive(data)
	if cerr != nil {
		return errors.New(C.GoString(cerr))
	}
	return nil
}

// AddString is Add for a string payload with default options.
func (w *ZipWriter) AddString(name, data string) error {
	return w.Add(name, []byte(data), DefaultAddOptions())
}

// SetComment sets the archive-level comment.
func (w *ZipWriter) SetComment(comment string) {
	if w.h == nil {
		return
	}
	c := C.CString(comment)
	defer C.free(unsafe.Pointer(c))
	C.pigzpp_zip_writer_set_comment(w.h, c)
}

// Finish finalizes the archive and returns its bytes, consuming the writer.
func (w *ZipWriter) Finish() ([]byte, error) {
	if w.h == nil {
		return nil, errors.New("pigzpp: zip writer is closed")
	}
	buf := C.pigzpp_zip_writer_finish(w.h)
	w.h = nil // consumed by finish
	if buf.error != nil {
		msg := C.GoString(buf.error)
		C.pigzpp_free(buf)
		return nil, errors.New(msg)
	}
	defer C.pigzpp_free(buf)
	if buf.data == nil {
		return []byte{}, nil
	}
	return C.GoBytes(unsafe.Pointer(buf.data), C.int(buf.size)), nil
}

// Close discards the writer without finalizing (if not already finished).
func (w *ZipWriter) Close() {
	if w.h != nil {
		C.pigzpp_zip_writer_free(w.h)
		w.h = nil
	}
}

// ZipEntry is metadata for a member of an archive.
type ZipEntry struct {
	Name             string
	CompressedSize   uint64
	UncompressedSize uint64
	CRC32            uint32
	Method           ZipMethod
	IsDir            bool
}

// ZipReader reads a ZIP archive held in memory.
type ZipReader struct {
	h       *C.pigzpp_zip_reader
	entries []ZipEntry
}

// OpenZip opens an archive from its bytes. The bytes are copied internally, so
// the caller may reuse or free the slice afterwards.
func OpenZip(data []byte) (*ZipReader, error) {
	var ptr *C.uint8_t
	if len(data) > 0 {
		ptr = (*C.uint8_t)(unsafe.Pointer(&data[0]))
	}
	var cerr *C.char
	h := C.pigzpp_zip_reader_open(ptr, C.size_t(len(data)), &cerr)
	runtime.KeepAlive(data)
	if h == nil {
		if cerr != nil {
			return nil, errors.New(C.GoString(cerr))
		}
		return nil, errors.New("pigzpp: cannot open zip archive")
	}
	r := &ZipReader{h: h}
	n := int(C.pigzpp_zip_reader_count(h))
	r.entries = make([]ZipEntry, 0, n)
	for i := 0; i < n; i++ {
		var e C.pigzpp_zip_entry
		if C.pigzpp_zip_reader_entry(h, C.size_t(i), &e) == 1 {
			r.entries = append(r.entries, ZipEntry{
				Name:             C.GoString(e.name),
				CompressedSize:   uint64(e.compressed_size),
				UncompressedSize: uint64(e.uncompressed_size),
				CRC32:            uint32(e.crc32),
				Method:           ZipMethod(e.method),
				IsDir:            e.is_dir != 0,
			})
		}
	}
	return r, nil
}

// Entries returns metadata for all members.
func (r *ZipReader) Entries() []ZipEntry { return r.entries }

// Names returns the member names in archive order.
func (r *ZipReader) Names() []string {
	names := make([]string, len(r.entries))
	for i, e := range r.entries {
		names[i] = e.Name
	}
	return names
}

// Read decompresses and returns the member named name.
func (r *ZipReader) Read(name string) ([]byte, error) {
	if r.h == nil {
		return nil, errors.New("pigzpp: zip reader is closed")
	}
	cname := C.CString(name)
	defer C.free(unsafe.Pointer(cname))
	buf := C.pigzpp_zip_reader_read(r.h, cname)
	if buf.error != nil {
		msg := C.GoString(buf.error)
		C.pigzpp_free(buf)
		return nil, errors.New(msg)
	}
	defer C.pigzpp_free(buf)
	if buf.data == nil {
		return nil, errors.New("pigzpp: null zip read result")
	}
	return C.GoBytes(unsafe.Pointer(buf.data), C.int(buf.size)), nil
}

// Comment returns the archive-level comment.
func (r *ZipReader) Comment() string {
	if r.h == nil {
		return ""
	}
	return C.GoString(C.pigzpp_zip_reader_comment(r.h))
}

// TestZip verifies every member's CRC. It returns the name of the first corrupt
// member, or "" if the archive is intact.
func (r *ZipReader) TestZip() string {
	if r.h == nil {
		return ""
	}
	c := C.pigzpp_zip_reader_testzip(r.h)
	if c == nil {
		return ""
	}
	return C.GoString(c)
}

// Close releases the reader.
func (r *ZipReader) Close() {
	if r.h != nil {
		C.pigzpp_zip_reader_free(r.h)
		r.h = nil
	}
}
