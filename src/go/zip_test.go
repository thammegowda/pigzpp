package pigzpp

import (
	"archive/zip"
	"bytes"
	"io"
	"testing"
)

func TestZipWriteReadRoundTrip(t *testing.T) {
	w, err := NewZipWriter()
	if err != nil {
		t.Fatal(err)
	}
	payload := bytes.Repeat([]byte("the quick brown fox "), 50000)
	if err := w.AddString("hello.txt", "hello world"); err != nil {
		t.Fatal(err)
	}
	if err := w.Add("data.bin", payload, DefaultAddOptions()); err != nil {
		t.Fatal(err)
	}
	if err := w.Add("stored.txt", []byte("raw"), AddOptions{Method: ZipStore}); err != nil {
		t.Fatal(err)
	}
	w.SetComment("go made this")
	archive, err := w.Finish()
	if err != nil {
		t.Fatal(err)
	}

	r, err := OpenZip(archive)
	if err != nil {
		t.Fatal(err)
	}
	defer r.Close()

	if got := r.Names(); len(got) != 3 {
		t.Fatalf("expected 3 entries, got %v", got)
	}
	if bad := r.TestZip(); bad != "" {
		t.Fatalf("testzip found corrupt entry: %s", bad)
	}
	if r.Comment() != "go made this" {
		t.Fatalf("comment mismatch: %q", r.Comment())
	}
	hello, err := r.Read("hello.txt")
	if err != nil || string(hello) != "hello world" {
		t.Fatalf("hello read failed: %v %q", err, hello)
	}
	data, err := r.Read("data.bin")
	if err != nil || !bytes.Equal(data, payload) {
		t.Fatalf("data read failed: %v (len %d)", err, len(data))
	}
}

func TestZipStdlibInterop(t *testing.T) {
	// pigzpp writes; Go's archive/zip reads.
	w, _ := NewZipWriter()
	payload := bytes.Repeat([]byte("interop "), 20000)
	_ = w.Add("a.txt", []byte("from pigzpp"), DefaultAddOptions())
	_ = w.Add("b.bin", payload, DefaultAddOptions())
	archive, err := w.Finish()
	if err != nil {
		t.Fatal(err)
	}

	zr, err := zip.NewReader(bytes.NewReader(archive), int64(len(archive)))
	if err != nil {
		t.Fatal(err)
	}
	found := map[string][]byte{}
	for _, f := range zr.File {
		rc, err := f.Open()
		if err != nil {
			t.Fatal(err)
		}
		b, _ := io.ReadAll(rc)
		rc.Close()
		found[f.Name] = b
	}
	if string(found["a.txt"]) != "from pigzpp" {
		t.Fatalf("a.txt mismatch: %q", found["a.txt"])
	}
	if !bytes.Equal(found["b.bin"], payload) {
		t.Fatalf("b.bin mismatch")
	}

	// Go's archive/zip writes; pigzpp reads.
	var buf bytes.Buffer
	zw := zip.NewWriter(&buf)
	fw, _ := zw.Create("x.txt")
	fw.Write([]byte("from go stdlib"))
	zw.Close()

	r, err := OpenZip(buf.Bytes())
	if err != nil {
		t.Fatal(err)
	}
	defer r.Close()
	got, err := r.Read("x.txt")
	if err != nil || string(got) != "from go stdlib" {
		t.Fatalf("pigzpp read of stdlib zip failed: %v %q", err, got)
	}
}
