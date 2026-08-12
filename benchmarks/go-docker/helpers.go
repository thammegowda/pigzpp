package main

import "bytes"

// sizedBuffer is a plain bytes.Buffer; aliased so call sites read clearly.
type sizedBuffer = bytes.Buffer

// byteReader returns an io.Reader over b.
func byteReader(b []byte) *bytes.Reader { return bytes.NewReader(b) }
