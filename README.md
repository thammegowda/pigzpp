# pigzpp — Parallel gzip, rewritten in C++23

**pigzpp is fast, parallel compression for your whole stack.** It's a drop-in replacement for `gzip`/`pigz` on the command line — *and* a proper library you can call from **Python, WebAssembly, C++, Go, and Rust**, with native **ZIP archives** and **PNG** encode/decode built on the same SIMD-accelerated ([zlib-ng](https://github.com/zlib-ng/zlib-ng)) and hand-tuned-assembly ([ISA-L](https://github.com/intel/isa-l)) DEFLATE core.

### Highlights — at a glance

Measured on an Intel Xeon W-2235 (128 MB text, level 6); see [Performance](#performance) for the full tables.

| You want to… | pigzpp gives you |
|---|---|
| Compress on the **command line** | Up to **8.5× faster than `pigz`**, **66× faster than `gzip`** — or preserve the zlib-class ratio and still be **2.1× faster than `pigz`** |
| Compress from **Python** | Up to **50× faster than the standard-library `gzip`** — and the fastest option tested in Go, Rust, and JavaScript/WASM too |
| Speed up **OCI layer compression** | Compress a real image layer via a cgo owned buffer **11× faster than Go's stdlib gzip at the same ratio** (or **42×** with ISA-L) |
| Build/read **ZIP archives** | `pigzpp.ZipFile` (a `zipfile` drop-in) writes **12–28× faster than Python's `zipfile`**; in the browser it reads `.docx`/`.xlsx`/`.zip` faster than fflate & JSZip |
| Write **PNG** images | **The fastest PNG encoder we tested** — ahead of **OpenCV** (`cv2`, a hand-optimized C++ encoder) and **11.8× faster than Pillow**, at a comparable size |
| Use it **as a library** | Thread-safe (no globals), selectable backend (`auto`/`zlib`/`isal`), one accelerated core shared by every language |
| Stay **compatible** | Compress with pigzpp, decompress with `gzip`/`pigz`/`unzip` — and vice-versa |

> **Note:** This project is an experiment in AI-assisted software modernization. The goal was to study how capable coding agents are at rewriting real-world tools — not to take credit away from the original authors. pigz is the work of [Mark Adler](https://en.wikipedia.org/wiki/Mark_Adler), co-creator of zlib, gzip, and the DEFLATE format, who invested countless hours building and maintaining it. pigzpp exists because of that foundation.
>
> Read the full writeup: **[I Let Two AI Agents Race to Modernize pigz](https://gowda.ai/posts/2026/03/pigzpp-with-agents/)**
>
> Read the full report: **[pigzpp technical report](report/pigzpp-report.pdf)**

## What's inside

pigz is one of those essential tools — if you've ever compressed GBs to TBs of data, you've probably used it. But pigz was written as a monolithic C program with a single global state (`struct g`, ~60 mutable fields), making it impossible to use as a library. pigzpp is a from-scratch C++23 rewrite that keeps the speed and adds everything a modern codebase needs:

- **Faster** — [zlib-ng](https://github.com/zlib-ng/zlib-ng) (SIMD) and [ISA-L](https://github.com/intel/isa-l) (hand-tuned assembly) DEFLATE backends, parallelized across cores
- **Selectable backend** — `auto` (ISA-L, fastest, x86-64 only), `zlib` (zlib-ng, best ratio, all platforms), or `isal`, via API and the `--engine` CLI flag
- **Thread-safe library** — no global state, run many compress/decompress operations in one process
- **Bindings for many languages** — Python (nanobind), C++, WebAssembly (Embind), Go (cgo), and Rust (FFI), all sharing one accelerated core
- **ZIP archives** — native multi-entry ZIP (STORED + DEFLATE, Zip64) with a `zipfile`-like Python API, interoperable with `zipfile`/`unzip`
- **PNG helpers** — encode/decode grayscale, grayscale+alpha, RGB, and RGBA image buffers
- **Fully compatible** — compress with pigzpp, decompress with gzip/pigz, and vice versa
- **Modern C++23** — `std::jthread`, exceptions, RAII, no `setjmp`/`longjmp`

## Performance

Benchmarked in an **Ubuntu 22.04 WSL2 guest exposing 5 cores / 10 logical CPUs** on an Intel Xeon W-2235 host. The processor itself has 6 cores / 12 hardware threads, but WSL2 exposed only 10 logical processors; Linux reported that guest allocation as 5 cores with 2 threads per core. Results use a **128 MB multilingual-text corpus** (English + Chinese Wikipedia; see `benchmarks/core/gen_data.py`) at **level 6, 8 workers** unless noted. Each report-aligned result is the median of **seven timed samples after one untimed warm-up**. The archived raw samples, commands, metadata, and corpus hashes are in [`benchmarks/results/2026-07-14/report-benchmarks.json`](benchmarks/results/2026-07-14/report-benchmarks.json).

pigzpp ships two DEFLATE backends: **ISA-L** (`auto`/`isal`, fastest) and **zlib-ng** (`zlib`, best ratio). **ISA-L is x86-64 only** (its DEFLATE is hand-written x86 assembly); on ARM64 — and in WebAssembly, which has no ISA-L build — pigzpp uses zlib-ng, so the `zlib` numbers are the portable path every platform gets. `ratio` is input/output (higher = smaller output).

**CLI — pigzpp vs gzip and pigz:**

| tool | MB/s | ratio |
|---|---|---|
| gzip 1.12 (`-6`) | 17 | 2.83 |
| pigz 2.6 (`-6 -p8`) | 133 | 2.83 |
| pigzpp (`--engine zlib`) | 285 | 2.81 |
| pigzpp (`--engine isal`, default) | **1124** | 2.58 |

pigzpp's zlib-ng backend is **2.1× faster than pigz** at the same ratio; ISA-L is **8.5× faster** (producing about 9% more output for speed).

**Native scaling and decompression:** the report run also measures `pigz`, pigzpp zlib-ng, and pigzpp ISA-L at 1, 2, 4, 5, 8, and 10 workers. From 1 to 10 workers, measured throughput rises from 18 to 162 MB/s for `pigz` and from 49 to 350 MB/s for pigzpp zlib-ng. Pigzpp ISA-L rises from 199 MB/s to a peak of 1158 MB/s at 8 workers, then reaches 1098 MB/s at 10. On a common level-6 gzip stream, native pigzpp decompresses at **711 MB/s**, versus 212 MB/s for `pigz` and 166 MB/s for `gzip`. Python decompression is single-threaded; the in-memory panel compares stdlib gzip (192 MB/s), zlib-ng (457 MB/s), python-isal (525 MB/s), and pigzpp (498 MB/s). The pigzpp binding inflates directly into the result `bytes` allocation rather than making a full-output copy.

**Level and corpus sensitivity:** a targeted CLI check repeats levels 1, 6, and 9 on both multilingual text and incompressible random data, reporting throughput together with ratio. The complete points are archived in [`report/plots/data/robustness.tsv`](report/plots/data/robustness.tsv); they keep the speed/ratio trade-off explicit rather than extrapolating the level-6 text result to every input.

**Language bindings — pigzpp vs the best competitor in each ecosystem** (128 MB text, L6, 8 threads):

| language | pigzpp `isal` | pigzpp `zlib` | best competitor |
|---|---|---|---|
| **Python** | 875 MB/s | 278 MB/s | `python-isal` 183 MB/s |
| **Go** | 1087 MB/s | 294 MB/s | `klauspost/pgzip` 301 MB/s |
| **Rust** | 912 MB/s | 261 MB/s | `gzp` (parallel) 225 MB/s |

Ratios: pigzpp `isal` 2.58, pigzpp `zlib` 2.81. The ISA-L path is fastest in every language panel. Portable zlib-ng is faster than the listed Python and Rust competitors and is within 3% of Go `pgzip`, at an equal-or-better ratio.

**Docker / OCI image layers** — image layers are gzip-compressed tarballs, and BuildKit's gzip writer uses Go's single-threaded `compress/gzip`. This benchmark isolates that compression stage; it is not an end-to-end `docker build` measurement. Go uses its normal optimizing compiler/inliner, and the C++ core is built in Release mode with `-O3`. The pigzpp rows use `CompressOwnedEngine`, consuming the C-backed slice before release so the timing does not include an additional ~225 MB `C.GoBytes` copy. On a **real 637 MB layer** (the largest layer of the official `python:3.12` image), level 6, 8 workers:

| method | MB/s | ratio | vs stdlib |
|---|---:|---:|---:|
| `compress/gzip` (Go stdlib — BuildKit today) | 34 | 2.83 | 1.0× |
| `klauspost/compress` (faster single-thread) | 91 | 2.76 | 2.7× |
| `klauspost/pgzip` (parallel) | 376 | 2.76 | 11.1× |
| **pigzpp `zlib`** (cgo, C-owned) | 381 | 2.83 | **11.2×** |
| **pigzpp `isal`** (cgo, C-owned) | **1437** | 2.61 | **42.3×** |

pigzpp zlib-ng and `pgzip` have similar median throughput here, but pigzpp produces the smaller stream (ratio 2.83 vs 2.76). ISA-L reaches **42×** the stdlib throughput, reducing this isolated stage from about 19.6 seconds to 0.5 seconds. Reproduce: `benchmarks/go-docker/fetch_layer.sh` downloads a real layer from any registry (no Docker daemon needed), then `./dockergzbench -input layer.tar -threads 8 -methods stdlib,pgzip,pigzppcgo-zlib-owned,pigzppcgo-isal-owned`.

**WebAssembly** (zlib-ng + 128-bit SIMD; ISA-L is x86-only, so not available in WASM; Node 22). Single-thread, 16 MB text:

| engine | MB/s | ratio |
|---|---:|---:|
| **pigzpp-wasm** | **42** | 2.81 |
| node-zlib | 31 | 2.84 |
| CompressionStream (native) | 27 | 2.84 |
| fflate (JS) | 15 | 2.75 |
| pako (JS) | 8 | 2.82 |

Even single-threaded, pigzpp-wasm (zlib-ng + SIMD) beats the browser's native `CompressionStream` and the popular JS libraries.

The threaded WASM build (pthreads via Web Workers + `SharedArrayBuffer`) scales across cores. On a **128 MB** corpus (level 6), in this 5-core/10-logical-CPU WSL2 guest allocation (`CompressionStream` and `node-zlib` are single-thread only, ~28 and ~32 MB/s here):

| threads | MB/s | speedup |
|---:|---:|---:|
| 1 | 41 | 1.00x |
| 2 | 81 | 1.97x |
| 4 | 153 | 3.72x |
| 5 | 179 | 4.36x |
| 8 | 242 | 5.89x |

At 8 threads pigzpp-wasm reaches **242 MB/s** (5.9x over single-thread, ~8.9x `CompressionStream` in the single-worker panel). Browsers must be cross-origin isolated (COOP/COEP headers) to enable `SharedArrayBuffer`, and blocking compress calls should run in a Web Worker. Reproduce with `node benchmarks/wasm/scaling.mjs --size 128 --threads 1,2,4,5,8` after `scripts/build_wasm.sh threads`.

**PNG encoding (Python API)** — `pigzpp.png.compress()` vs Pillow and OpenCV, on the 24-image [Kodak](https://r0k.us/graphics/kodak/) true-color set (768×512), median of seven complete passes after one warm-up, round-trips verified. `vs Pillow` is the speedup over Pillow's default; `size` is output size relative to Pillow's default (lower = smaller):

| encoder (preset) | img/s | vs Pillow | size |
|---|---:|---:|---:|
| **pigzpp `fast`** (default) | **87** | **11.8×** | 1.10× |
| cv2 (default) | 53 | 7.2× | 1.09× |
| pigzpp `balanced` | 39 | 5.3× | 1.06× |
| pillow `fast` | 25 | 3.5× | 1.03× |
| pillow (default) | 7.3 | 1.0× | 1.00× |
| pigzpp `small` | 6.7 | 0.9× | 0.99× |

pigzpp's `fast` preset is the **fastest PNG encoder we benchmarked** on true-color images: **1.6× faster than OpenCV** (`cv2.imencode`, itself a hand-optimized C++ encoder) and **11.8× faster than Pillow's default**, at a comparable size. pigzpp applies the PNG line filters itself and then compresses the filtered scanlines with the **ISA-L** backend (x86; zlib-ng elsewhere), which is ~2.2× faster than pigzpp's zlib-ng `balanced` preset in this run. Reproduce: `python benchmarks/png/bench_png.py --image-dir <dir> --mode rgb --verify`.

**ZIP archives (Python API)** — `pigzpp.ZipFile` vs the standard library's `zipfile`, both writing real DEFLATE archives and reading them back (128 MB text, level 6, 8 workers):

| writer | write MB/s | ratio | read MB/s |
|---|---:|---:|---:|
| zipfile (stdlib) | 18 | 2.83 | 198 |
| **pigzpp `isal`** | **485** | 2.58 | 325 |
| pigzpp `zlib` | 212 | 2.81 | 364 |

pigzpp parallelizes each member's DEFLATE, so it writes **12× faster than `zipfile`** at the same ratio (`zlib`) and up to **28× faster** with `isal`, and reads up to 1.8× faster. Reproduce: `python benchmarks/python/bench_zip.py --sizes 128 --members 1,16`.

**ZIP archives (WebAssembly)** — the same `ZipWriter`/`ZipReader` classes vs the two common JS zip libraries, **fflate** and **JSZip** (16 MB corpus, Node 22, single-thread + SIMD, level 6). DOCX/XLSX/EPUB/JAR are all ZIP containers, so "open a document and read its parts" is the *unzip all* row:

| create archive | 50 members | 4 large members | ratio |
|---|---:|---:|---:|
| **pigzpp-wasm** (t=4) | **67** | **109** | 2.78 |
| pigzpp-wasm (t=1) | 42 | 44 | 2.78 |
| fflate | 15 | 15 | 2.72 |
| JSZip | 9 | 9 | 2.80 |

| read + unzip all (MB/s) | 50-member archive | real 6 MB `.docx` |
|---|---:|---:|
| **pigzpp-wasm** | **339** | **253** |
| fflate | 115 | 111 |
| JSZip | 89 | 87 |

Even single-threaded, pigzpp-wasm creates archives ~3–5× faster and reads them ~2–3× faster than the popular JS libraries at an equal-or-better ratio; the threaded build (`t=4`) widens the create gap up to 2.5× more on large members. Reproduce: `node benchmarks/wasm/zip.mjs --file document.docx` (or `--members 50 --threads 1,4` against the threaded build).

Benchmarks live under `benchmarks/` (`core`, `python`, `png`, `go-docker`, `rust`, `wasm`), all reading the shared corpus in `build/bench_data/`. See [notes/05-summary.md](notes/05-summary.md) for the earlier large-core CLI runs (48-core Xeon) and thread-scaling detail.

## Prebuilt releases

Tagged releases publish artifacts to [GitHub Releases](https://github.com/thammegowda/pigzpp/releases):

- One `cp312-abi3` Python wheel for each of Linux, macOS, and Windows on x86_64 and ARM64. Each wheel works with regular CPython 3.12 and newer on that same OS and architecture.
- A native `pigzpp` CLI archive for each of those six OS/architecture targets.
- WebAssembly archives for `baseline`, `simd`, and `threads` variants.

The stable ABI removes the need for separate wheels for CPython 3.12, 3.13, 3.14, and later releases. Free-threaded CPython uses a different ABI and is not currently included. Linux wheels target manylinux/glibc; Alpine/musl wheels are not currently produced.

ISA-L is enabled on supported x86-64 builds. ARM64 artifacts use zlib-ng, which still provides runtime SIMD acceleration. Releases are attached to GitHub only; they are not automatically published to PyPI or npm.

All release artifacts embed zlib-ng, ISA-L (when enabled), Zopfli, and nanobind statically; they never require those libraries to be installed at runtime. Linux CLI archives are fully static. Windows CLI archives and wheels use the static MSVC runtime. Linux wheels also embed the GNU C++ and GCC runtimes. Python itself and operating-system libraries remain dynamic by design; macOS does not support fully static executables and uses the system `libc++`/`libSystem` supplied by every supported macOS release. CI inspects each final archive and repaired wheel and rejects dynamic zlib-ng or ISA-L dependencies.

Every release includes `DIST_SIZES.md` and `dist-sizes.json`, reporting the exact byte size and human-readable binary size of all six native archives, six wheels, and three WebAssembly packages, together with per-category and overall totals. The same table is written to the distribution workflow summary.

## Build

Requires CMake 3.20+, a C++23 compiler (GCC 13+, Clang 17+, or a recent MSVC), and Python 3.12+ (for the Python module). The optional bindings each need their own toolchain: **Emscripten** (WebAssembly), **Go 1.22+** (cgo), or **Rust 1.75+** (FFI) — see [Building the language bindings](#building-the-language-bindings).

```bash
git clone --recursive https://github.com/thammegowda/pigzpp.git
cd pigzpp
make build        # Release build (optimized, static CLI)
```

If you already cloned without `--recursive`, fetch the submodules with:
```bash
git submodule update --init --recursive
```

This produces:
- `build/pigzpp` — CLI binary (drop-in replacement for pigz)
- `build/pigzpp.abi3.so` — Python module for regular CPython 3.12+

Other useful targets:

```bash
make test           # Run C++ and Python tests
make bench          # Run all benchmarks (CLI + Python)
make bench-bin      # Benchmark CLI: gzip vs pigz vs pigzpp vs igzip
make bench-py       # Benchmark Python: gzip vs zlib-ng vs isal vs pigzpp
make bench-png      # Benchmark PNG encoding vs Pillow baseline
make debug          # Debug build with sanitizers
make clean          # Remove build artifacts
```

### Building the language bindings

`make build` produces the CLI (and a Python module you can import from `build/` via `PYTHONPATH`). To install the Python package properly, or to build the WebAssembly / C-ABI surfaces, use the steps below.

**Python** (nanobind, CPython stable ABI) — install the `pigzpp` module into a regular CPython 3.12+ environment:

```bash
pip install .
```

**C ABI shared library** — the Go and Rust bindings link against `libpigzppc.so`:

```bash
cmake -DPIGZPP_BUILD_CAPI=ON -S . -B build
cmake --build build --target pigzppc      # -> build/libpigzppc.so
```

The Go module lives in `src/go` and the Rust crate in `src/rust`; both locate `libpigzppc.so` in `build/` (override with `PIGZPP_BUILD_DIR` for Rust). Run `go test ./...` in `src/go` or `cargo test` in `src/rust` to verify.

**WebAssembly** (Emscripten) — activate the emsdk, then run the build script:

```bash
# One-time: fetch the WASM-relevant submodules and activate Emscripten
git submodule update --init third_party/zlib-ng third_party/zopfli
source /path/to/emsdk/emsdk_env.sh        # provides emcmake/emmake

scripts/build_wasm.sh                      # default: SIMD variant
# or pick a variant explicitly:
scripts/build_wasm.sh baseline             # portable (no SIMD, no threads)
scripts/build_wasm.sh simd                 # 128-bit SIMD (all modern engines)
scripts/build_wasm.sh threads              # SIMD + pthreads (parallel members)
scripts/build_wasm.sh all                  # build all three
```

Each variant emits `build-wasm-<variant>/wasm/pigzpp_wasm.mjs` (plus the `.wasm`), an ES module you `import` (see [WebAssembly usage](#webassembly)). The **simd** build runs in all current browsers and Node 16+; the **threads** build needs cross-origin isolation (COOP/COEP headers) to enable `SharedArrayBuffer`. No Emscripten? `emsdk` installs in a minute:

```bash
git clone https://github.com/emscripten-core/emsdk && cd emsdk
./emsdk install latest && ./emsdk activate latest
source ./emsdk_env.sh
```

## Usage

### CLI (same interface as pigz)

```bash
# Compress
pigzpp -c file.txt > file.gz
cat file.txt | pigzpp > file.gz

# Decompress
pigzpp -d file.gz
pigzpp -dc file.gz > file.txt

# Options
pigzpp -p 8 -6 -c file.txt > file.gz              # 8 threads, level 6
pigzpp -p 8 -6 --engine zlib -c file.txt > file.gz # zlib-ng backend (best ratio)
pigzpp -p 8 -6 --engine isal -c file.txt > file.gz # ISA-L backend (fastest, default)
```

### Python

```python
import pigzpp

# File API (like gzip.open)
with pigzpp.open("data.gz", "w") as f:
    f.write("hello world\n")

with pigzpp.open("data.gz", "r") as f:
    data = f.read()

# Bytes API
compressed = pigzpp.compress(b"raw data", level=6, threads=0)
original = pigzpp.decompress(compressed)

# Select the DEFLATE backend: "auto" (default), "isal" (fastest), or "zlib" (best ratio)
compressed = pigzpp.compress(large_bytes, level=6, engine="zlib")
```

### Go

```go
import pigzpp "github.com/thammegowda/pigzpp/src/go"

gz, _ := pigzpp.Compress(data, 6, 0)              // level 6, all cores, auto engine
gz, _ = pigzpp.CompressEngine(data, 6, 8, pigzpp.EngineZlib)
raw, _ := pigzpp.Decompress(gz, 0)
png, _ := pigzpp.PngEncode(pixels, w, h, channels, 6, "rle", "up")
```

Requires the shared library (`cmake -DPIGZPP_BUILD_CAPI=ON` → `libpigzppc.so`); the binding's `build`/`replace` points at it.

### Rust

```rust
use pigzpp::Engine;

let gz = pigzpp::compress(&data, 6, 0, Engine::Auto)?;   // or Engine::Zlib / Engine::Isal
let raw = pigzpp::decompress(&gz, 0)?;
let png = pigzpp::png_encode(&pixels, w, h, channels, 6, "rle", "up")?;
```

```toml
# Cargo.toml — links libpigzppc (set PIGZPP_BUILD_DIR if not ../../build)
pigzpp = { path = "path/to/pigzpp/src/rust" }
```

### WebAssembly

```js
import createPigzpp from "./pigzpp_wasm.mjs";
const M = await createPigzpp();
const gz = M.gzipCompress(bytes, 6, "default", 1);   // Uint8Array in/out
const raw = M.gzipDecompress(gz, 1);
const png = M.pngEncode(pixels, w, h, channels, 6, "rle", "up");
```

Build the module with `scripts/build_wasm.sh` (Emscripten). The WASM build uses zlib-ng (ISA-L is x86-only); threads require cross-origin isolation.


### ZIP archives

pigzpp has a native, multi-entry ZIP container (STORED + DEFLATE, Zip64 for large
entries/archives) that reuses the parallel compressor per member. The Python API
mirrors a subset of the standard library's `zipfile`, and archives interoperate
with `zipfile`, `unzip`, and other standard tools.

```python
import pigzpp

# Write (parallel DEFLATE per member; engine = "auto"/"isal"/"zlib")
with pigzpp.ZipFile("out.zip", "w", threads=8, engine="isal") as z:
    z.writestr("hello.txt", "hi there")
    z.write("/path/to/photo.png")                       # add a file from disk
    z.writestr("raw.bin", data, compress_type=pigzpp.ZIP_STORED)
    z.setcomment("made by pigzpp")

# Read / list / extract / verify
with pigzpp.ZipFile("out.zip") as z:          # mode "r" (also "a" to append, "x" to create)
    print(z.namelist())
    blob = z.read("hello.txt")
    assert z.testzip() is None                 # CRC-check every member
    z.extractall("out_dir")
    for info in z.infolist():
        print(info.filename, info.file_size, info.compress_size, info.compress_type)
```

The archive type is also available from C++ ([`pigzpp/zip.h`](src/pigzpp/zip.h)) and WebAssembly:

```cpp
// C++
pigzpp::zip::ZipWriter w("out.zip");
w.write_str("a.txt", "hello");
w.close();
pigzpp::zip::ZipReader r("out.zip");
auto bytes = r.read("a.txt");
```

```js
// WebAssembly (Embind)
const w = new M.ZipWriter();
w.add("a.txt", new TextEncoder().encode("hello"), 8, 6, 1);  // name, bytes, method, level, threads
const archive = w.finish(); w.delete();
const r = new M.ZipReader(archive);
const data = r.read("a.txt"); r.delete();
```


### Python PNG

```python
import pigzpp as pig

# Grayscale HxW and grayscale+alpha/RGB/RGBA HxWxC uint8 arrays are accepted directly.
png_bytes = pig.png.compress(image, preset="balanced")
image = pig.png.decompress_array(png_bytes)  # NumPy uint8 array, HxW or HxWxC
image = pig.png.decompress(png_bytes, result="numpy")

pig.png.save("out.png", image, preset="balanced")
image = pig.png.load("out.png")  # result="numpy" is the default for load()

# Bytes API is also available for callers that do not want arrays.
pixels, shape = pig.png.decompress(png_bytes)  # shape is (width, height, channels)
pixels, shape = pig.png.load("out.png", result="bytes")
```

`pigzpp.png` currently targets fast lossless grayscale/grayscale+alpha/RGB/RGBA image buffers. It writes standard 8-bit non-interlaced PNG files, validates chunk CRCs on decode, and supports PNG filters (`none`, `sub`, `up`, `average`, `paeth`, `adaptive-fast`, `adaptive-all`) plus DEFLATE strategies (`default`, `rle`, `huffman`, `fixed`, `filtered`). Presets are available as `fast`, `balanced`, and `small`; `fast` uses `level=1`, `strategy="default"`, and `filter="up"` (so it compresses with ISA-L on x86), `balanced` uses `level=1`, `strategy="rle"`, and `filter="adaptive-fast"`, while `small` uses `level=9`, `strategy="filtered"`, and `filter="adaptive-all"`. Explicit `level`, `strategy`, or `filter` arguments override the selected preset. With ISA-L enabled, PNG decode uses ISA-L when possible; PNG encode uses ISA-L for `strategy="default"` and falls back to zlib-ng for other zlib-style strategies.

PNG benchmarks compare `pigzpp.png`, OpenCV, and Pillow modes against `PIL.Image.save(..., format="PNG")`, the common Python PNG baseline. A `*` in the benchmark output marks each library's default mode. Benchmarks can be run on RGB, grayscale, or binary mask inputs:

```bash
python benchmarks/png/bench_png.py --mode rgb --verify
python benchmarks/png/bench_png.py --mode gray --verify
python benchmarks/png/bench_png.py --mode mask --verify
```

For a load/manipulate/save example, see `scripts/sample_png_rgb_save.py`.

## Tests

```bash
cd build
ctest --output-on-failure
```

## Project Structure

```
pigzpp/
├── src/pigzpp/
│   ├── pigzpp.h          Public API header
│   ├── config.h/cpp      Configuration (replaces global struct g)
│   ├── compress.h/cpp    Parallel compressor (ISA-L / zlib-ng backends)
│   ├── decompress.h/cpp  Decompressor with parallel CRC
│   ├── png.h/cpp         PNG encode/decode helpers
│   ├── zip.h/cpp         Native multi-entry ZIP archives (ZipWriter/ZipReader)
│   ├── crc.h/cpp         CRC-32/Adler-32 with optimized combine
│   ├── pool.h/cpp        Thread-safe buffer pool (RAII)
│   ├── format.h/cpp      Gzip/zlib header/trailer parsing
│   ├── io_utils.h/io.cpp Buffered I/O with EINTR retry
│   ├── capi.h/cpp        C ABI (libpigzppc) for FFI bindings
│   └── main.cpp          CLI entry point
├── src/python/           nanobind stable-ABI bindings
├── src/go/               Go binding (cgo → libpigzppc)
├── src/rust/             Rust binding (FFI → libpigzppc)
├── src/wasm/             WebAssembly binding (Embind)
├── tests/                GoogleTest + pytest
├── benchmarks/           core, python, png, go-docker, rust, wasm suites
├── third_party/          zlib-ng, ISA-L, Zopfli, nanobind
└── notes/                Development notes and blog post
```

## Credits

pigzpp is a rewrite of [pigz](https://zlib.net/pigz/) by **Mark Adler** (co-creator of zlib, gzip, and the DEFLATE format). The original pigz is licensed under the [zlib license](https://zlib.net/zlib_license.html).

This is an **altered version** — a complete rewrite in C++23 with a different architecture (thread-safe library vs monolithic CLI). It is not the original pigz. Per the zlib license terms:

> *"Altered source versions must be plainly marked as such, and must not be misrepresented as being the original software."*

pigzpp uses the same [zlib license](LICENSE) as the original pigz.

### Third-party libraries

- [zlib-ng](https://github.com/zlib-ng/zlib-ng) — SIMD-optimized zlib replacement (zlib license)
- [ISA-L](https://github.com/intel/isa-l) — accelerated DEFLATE/Adler-32 for supported builds (BSD license)
- [nanobind](https://github.com/wjakob/nanobind) — C++/Python bindings (BSD license)
- [zopfli](https://github.com/google/zopfli) — optimal DEFLATE compressor for level 11 (Apache 2.0)
- [GoogleTest](https://github.com/google/googletest) — testing framework (BSD license)

## License

[zlib license](LICENSE) — same as the original pigz. Free for any use including commercial.
