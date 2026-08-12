// Command dockergzbench compares gzip layer-compression strategies relevant to
// `docker build` / image export.
//
// It measures exactly what BuildKit's util/compression/gzip.go does today
// (Go stdlib compress/gzip, single-threaded) against parallel alternatives:
//
//   - stdlib   : compress/gzip            (what Docker/BuildKit uses now; baseline)
//   - pgzip    : github.com/klauspost/pgzip (pure-Go parallel gzip; honest competitor)
//   - kp       : github.com/klauspost/compress/gzip (faster single-thread pure Go)
//   - pigzpp   : the pigzpp CLI via an exec pipe (native parallel gzip)
//
// Every method's output is verified to round-trip through the stdlib gzip
// reader, so we only compare valid gzip streams.
//
// Usage:
//
//	go mod tidy                         # fetch klauspost deps (needs network once)
//	go build -o dockergzbench .
//
//	# Synthetic data:
//	./dockergzbench -size 64 -iters 5 -level 6 -threads 8
//
//	# Real image layer (see gen_layer.sh to produce layer.tar):
//	./dockergzbench -input layer.tar -iters 5 -threads 8 \
//	    -pigzpp ../../build/pigzpp
//
// Flags let you point -pigzpp at any pigz-compatible binary, so you can also
// benchmark the original pigz for reference.
package main

import (
	stdgzip "compress/gzip"
	"encoding/json"
	"flag"
	"fmt"
	"io"
	"math/rand"
	"os"
	"os/exec"
	"runtime"
	"sort"
	"strconv"
	"time"

	kpgzip "github.com/klauspost/compress/gzip"
	"github.com/klauspost/pgzip"
	pigzpp "github.com/thammegowda/pigzpp/src/go"
)

func main() {
	var (
		input   = flag.String("input", "", "path to input file (e.g. a layer.tar); if empty, synthetic data is generated")
		sizeMB  = flag.Int("size", 64, "synthetic input size in MiB (used when -input is empty)")
		iters   = flag.Int("iters", 7, "timed iterations per method")
		level   = flag.Int("level", 6, "gzip compression level (1-9); -1 = library default")
		threads = flag.Int("threads", 0, "worker threads for parallel methods (0 = NumCPU)")
		pigzbin = flag.String("pigzpp", "pigzpp", "path to pigzpp (or any pigz-compatible) binary")
		methods = flag.String("methods", "stdlib,kp,pgzip,pigzpp,pigzppcgo-zlib,pigzppcgo-isal", "comma-separated methods to run")
		jsonOut = flag.String("json-out", "", "optional path for raw-sample JSON")
	)
	flag.Parse()

	data, srcName := loadInput(*input, *sizeMB)
	fmt.Printf("input: %s  (%d bytes = %.1f MiB)\n", srcName, len(data), float64(len(data))/(1<<20))
	fmt.Printf("level=%d threads=%s iters=%d\n\n", *level, threadsLabel(*threads), *iters)

	selected := splitCSV(*methods)
	var results []result
	for _, m := range selected {
		// Owned methods return a C-backed slice and need per-call cleanup, so
		// they use a dedicated runner rather than copying through C.GoBytes.
		if m == "pigzppcgo-owned" || m == "pigzppcgo-zlib-owned" ||
			m == "pigzppcgo-isal-owned" {
			r, err := benchmarkOwned(m, data, *level, *threads, *iters)
			if err != nil {
				fmt.Fprintf(os.Stderr, "%s: FAILED: %v\n", m, err)
				continue
			}
			results = append(results, r)
			continue
		}
		compress, ok := methodFor(m, *level, *threads, *pigzbin)
		if !ok {
			fmt.Fprintf(os.Stderr, "skip: unknown method %q\n", m)
			continue
		}
		r, err := benchmark(m, data, compress, *iters)
		if err != nil {
			fmt.Fprintf(os.Stderr, "%s: FAILED: %v\n", m, err)
			continue
		}
		results = append(results, r)
	}

	report(results, len(data))
	if *jsonOut != "" {
		if err := writeJSON(*jsonOut, results, srcName, len(data), *level, *threads); err != nil {
			fmt.Fprintf(os.Stderr, "write JSON: %v\n", err)
			os.Exit(1)
		}
	}
}

type compressFunc func(src []byte) ([]byte, error)

type result struct {
	name    string
	median  time.Duration
	samples []time.Duration
	outSize int
	inSize  int
}

func benchmark(name string, data []byte, fn compressFunc, iters int) (result, error) {
	// Untimed warm-up: initialize backend dispatch, allocators, and page cache.
	if _, err := fn(data); err != nil {
		return result{}, err
	}
	runtime.GC()
	times := make([]time.Duration, 0, iters)
	var out []byte
	for i := 0; i < iters; i++ {
		// Drop the previous result and collect outside the timed region. Without
		// this, a 225 MiB layer output can trigger unrelated GC work during a
		// later method and materially reorder the results.
		out = nil
		runtime.GC()
		start := time.Now()
		o, err := fn(data)
		elapsed := time.Since(start)
		if err != nil {
			return result{}, err
		}
		times = append(times, elapsed)
		out = o
	}
	if err := verifyRoundtrip(data, out); err != nil {
		return result{}, fmt.Errorf("roundtrip verification failed: %w", err)
	}
	return result{name: name, median: medianDuration(times), samples: times,
		outSize: len(out), inSize: len(data)}, nil
}

// benchmarkOwned times pigzpp.CompressOwned, the fully zero-copy output
// path. Each iteration returns a slice aliasing a C buffer plus a release
// func; we free the previous iteration's buffer and keep the last alive for
// verification, freeing it afterwards.
func benchmarkOwned(name string, data []byte, level, threads, iters int) (result, error) {
	engine := pigzpp.EngineAuto
	switch name {
	case "pigzppcgo-zlib-owned":
		engine = pigzpp.EngineZlib
	case "pigzppcgo-isal-owned":
		engine = pigzpp.EngineIsal
	}
	compress := func() ([]byte, func(), error) {
		return pigzpp.CompressOwnedEngine(data, level, threads, engine)
	}

	// Warm-up and release the C-owned result immediately.
	if _, free, err := compress(); err != nil {
		return result{}, err
	} else if free != nil {
		free()
	}
	times := make([]time.Duration, 0, iters)
	var out []byte
	var lastFree func()
	for i := 0; i < iters; i++ {
		if lastFree != nil {
			lastFree()
			lastFree = nil
			out = nil
		}
		runtime.GC()
		start := time.Now()
		o, free, err := compress()
		elapsed := time.Since(start)
		if err != nil {
			return result{}, err
		}
		times = append(times, elapsed)
		lastFree = free
		out = o
	}
	verr := verifyRoundtrip(data, out)
	outLen := len(out)
	if lastFree != nil {
		lastFree()
	}
	if verr != nil {
		return result{}, fmt.Errorf("roundtrip verification failed: %w", verr)
	}
	return result{name: name, median: medianDuration(times), samples: times,
		outSize: outLen, inSize: len(data)}, nil
}

func medianDuration(samples []time.Duration) time.Duration {
	sorted := append([]time.Duration(nil), samples...)
	sort.Slice(sorted, func(i, j int) bool { return sorted[i] < sorted[j] })
	return sorted[len(sorted)/2]
}

// verifyRoundtrip confirms the compressed stream is a valid gzip that decodes
// back to the original bytes.
func verifyRoundtrip(orig, compressed []byte) error {
	zr, err := stdgzip.NewReader(byteReader(compressed))
	if err != nil {
		return err
	}
	defer zr.Close()
	got, err := io.ReadAll(zr)
	if err != nil {
		return err
	}
	if len(got) != len(orig) {
		return fmt.Errorf("length mismatch: got %d want %d", len(got), len(orig))
	}
	// Cheap full compare.
	for i := range got {
		if got[i] != orig[i] {
			return fmt.Errorf("byte mismatch at offset %d", i)
		}
	}
	return nil
}

func methodFor(name string, level, threads int, pigzbin string) (compressFunc, bool) {
	switch name {
	case "stdlib":
		return func(src []byte) ([]byte, error) {
			var buf sizedBuffer
			w, err := stdgzip.NewWriterLevel(&buf, normLevel(level, stdgzip.DefaultCompression))
			if err != nil {
				return nil, err
			}
			if _, err := w.Write(src); err != nil {
				return nil, err
			}
			if err := w.Close(); err != nil {
				return nil, err
			}
			return buf.Bytes(), nil
		}, true

	case "kp": // klauspost/compress/gzip — faster single-thread pure Go
		return func(src []byte) ([]byte, error) {
			var buf sizedBuffer
			w, err := kpgzip.NewWriterLevel(&buf, normLevel(level, kpgzip.DefaultCompression))
			if err != nil {
				return nil, err
			}
			if _, err := w.Write(src); err != nil {
				return nil, err
			}
			if err := w.Close(); err != nil {
				return nil, err
			}
			return buf.Bytes(), nil
		}, true

	case "pgzip": // klauspost/pgzip — pure-Go parallel gzip
		return func(src []byte) ([]byte, error) {
			var buf sizedBuffer
			w, err := pgzip.NewWriterLevel(&buf, normLevel(level, pgzip.DefaultCompression))
			if err != nil {
				return nil, err
			}
			if threads > 0 {
				// 128 KiB blocks per worker mirrors pigz's default granularity.
				if err := w.SetConcurrency(128<<10, threads); err != nil {
					return nil, err
				}
			}
			if _, err := w.Write(src); err != nil {
				return nil, err
			}
			if err := w.Close(); err != nil {
				return nil, err
			}
			return buf.Bytes(), nil
		}, true

	case "pigzpp": // native pigzpp CLI via exec pipe
		return func(src []byte) ([]byte, error) {
			return runPipe(pigzbin, pigzArgs(level, threads), src)
		}, true

	case "pigzppcgo": // pigzpp linked in-process via cgo (no fork/pipe)
		return func(src []byte) ([]byte, error) {
			return pigzpp.Compress(src, level, threads)
		}, true

	case "pigzppcgo-isal": // force the ISA-L backend
		return func(src []byte) ([]byte, error) {
			return pigzpp.CompressEngine(src, level, threads, pigzpp.EngineIsal)
		}, true

	case "pigzppcgo-zlib": // force the zlib-ng backend
		return func(src []byte) ([]byte, error) {
			return pigzpp.CompressEngine(src, level, threads, pigzpp.EngineZlib)
		}, true
	}
	return nil, false
}

func pigzArgs(level, threads int) []string {
	args := []string{"-c"}
	if threads > 0 {
		args = append(args, "-p", strconv.Itoa(threads))
	}
	if level >= 1 && level <= 9 {
		args = append(args, "-"+strconv.Itoa(level))
	}
	return args
}

// runPipe feeds src to the command's stdin and returns its stdout.
func runPipe(bin string, args []string, src []byte) ([]byte, error) {
	cmd := exec.Command(bin, args...)
	cmd.Stdin = byteReader(src)
	var out sizedBuffer
	cmd.Stdout = &out
	cmd.Stderr = os.Stderr
	if err := cmd.Run(); err != nil {
		return nil, err
	}
	return out.Bytes(), nil
}

func report(results []result, inSize int) {
	if len(results) == 0 {
		fmt.Println("no results")
		return
	}
	// Sort fastest first.
	sort.Slice(results, func(i, j int) bool { return results[i].median < results[j].median })

	fmt.Printf("%-10s %12s %12s %10s %8s\n", "method", "time", "throughput", "out(MiB)", "ratio")
	fmt.Println("--------------------------------------------------------------")
	base := results[len(results)-1].median // slowest as reference for speedup
	for _, r := range results {
		mbps := float64(inSize) / 1e6 / r.median.Seconds()
		ratio := float64(inSize) / float64(r.outSize)
		speedup := float64(base) / float64(r.median)
		fmt.Printf("%-10s %12s %9.1f MB/s %10.2f %7.2fx   (%.2fx vs slowest)\n",
			r.name, r.median.Round(time.Microsecond), mbps,
			float64(r.outSize)/(1<<20), ratio, speedup)
	}
}

func writeJSON(path string, results []result, input string, inSize, level, threads int) error {
	type jsonResult struct {
		Method       string    `json:"method"`
		SamplesMS    []float64 `json:"samples_ms"`
		MedianMS     float64   `json:"median_ms"`
		ThroughputMB float64   `json:"throughput_mbps"`
		OutputBytes  int       `json:"output_bytes"`
		Ratio        float64   `json:"ratio"`
	}
	payload := struct {
		Input      string       `json:"input"`
		InputBytes int          `json:"input_bytes"`
		Level      int          `json:"level"`
		Threads    int          `json:"threads"`
		Warmups    int          `json:"warmups"`
		Results    []jsonResult `json:"results"`
	}{Input: input, InputBytes: inSize, Level: level, Threads: threads, Warmups: 1}
	for _, r := range results {
		samples := make([]float64, len(r.samples))
		for i, sample := range r.samples {
			samples[i] = float64(sample) / float64(time.Millisecond)
		}
		payload.Results = append(payload.Results, jsonResult{
			Method: r.name, SamplesMS: samples,
			MedianMS:     float64(r.median) / float64(time.Millisecond),
			ThroughputMB: float64(inSize) / 1e6 / r.median.Seconds(),
			OutputBytes:  r.outSize, Ratio: float64(inSize) / float64(r.outSize),
		})
	}
	data, err := json.MarshalIndent(payload, "", "  ")
	if err != nil {
		return err
	}
	return os.WriteFile(path, append(data, '\n'), 0o644)
}

// ---- helpers ----

func loadInput(path string, sizeMB int) ([]byte, string) {
	if path != "" {
		b, err := os.ReadFile(path)
		if err != nil {
			fmt.Fprintf(os.Stderr, "cannot read %s: %v\n", path, err)
			os.Exit(1)
		}
		return b, path
	}
	return genSynthetic(sizeMB), fmt.Sprintf("synthetic(%dMiB)", sizeMB)
}

// genSynthetic produces semi-compressible data: a mix of repeated tokens and
// random bytes, roughly mimicking a source/text-heavy layer.
func genSynthetic(sizeMB int) []byte {
	n := sizeMB << 20
	buf := make([]byte, 0, n)
	rng := rand.New(rand.NewSource(42))
	words := [][]byte{
		[]byte("package main\n"), []byte("func "), []byte("return nil\n"),
		[]byte("import ("), []byte("/usr/lib/x86_64-linux-gnu/"),
		[]byte("0123456789abcdef"), []byte("the quick brown fox "),
	}
	for len(buf) < n {
		if rng.Intn(100) < 70 {
			buf = append(buf, words[rng.Intn(len(words))]...)
		} else {
			chunk := make([]byte, 16)
			rng.Read(chunk)
			buf = append(buf, chunk...)
		}
	}
	return buf[:n]
}

func normLevel(level, def int) int {
	if level < 0 {
		return def
	}
	return level
}

func threadsLabel(t int) string {
	if t == 0 {
		return "NumCPU"
	}
	return strconv.Itoa(t)
}

func splitCSV(s string) []string {
	var out []string
	cur := ""
	for _, c := range s {
		if c == ',' {
			if cur != "" {
				out = append(out, cur)
			}
			cur = ""
			continue
		}
		cur += string(c)
	}
	if cur != "" {
		out = append(out, cur)
	}
	return out
}
