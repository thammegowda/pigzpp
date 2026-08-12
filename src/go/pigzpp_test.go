package pigzpp

import (
	"bytes"
	"errors"
	"testing"
)

func TestCheckedSize(t *testing.T) {
	maxInt := uint64(^uint(0) >> 1)
	length, err := checkedSize(maxInt)
	if err != nil || uint64(length) != maxInt {
		t.Fatalf("checkedSize(maxInt) = %d, %v", length, err)
	}
	if _, err := checkedSize(maxInt + 1); !errors.Is(err, errResultTooLarge) {
		t.Fatalf("checkedSize(maxInt + 1) error = %v", err)
	}
}

func TestCompressDecompress(t *testing.T) {
	input := bytes.Repeat([]byte("pigzpp go binding\n"), 100)
	compressed, err := Compress(input, 6, 1)
	if err != nil {
		t.Fatal(err)
	}
	restored, err := Decompress(compressed, 1)
	if err != nil {
		t.Fatal(err)
	}
	if !bytes.Equal(restored, input) {
		t.Fatal("decompressed data differs from input")
	}
}

func TestPngEncodeDecode(t *testing.T) {
	pixels := []byte{10, 20, 30, 40, 50, 60}
	encoded, err := PngEncode(pixels, 2, 1, 3, 6, "", "")
	if err != nil {
		t.Fatal(err)
	}
	image, err := PngDecode(encoded)
	if err != nil {
		t.Fatal(err)
	}
	if image.Width != 2 || image.Height != 1 || image.Channels != 3 {
		t.Fatalf("unexpected image metadata: %+v", image)
	}
	if !bytes.Equal(image.Pixels, pixels) {
		t.Fatal("decoded pixels differ from input")
	}
}
