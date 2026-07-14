# pigzpp — Parallel gzip, rewritten in C++23

> **Note:** This project is an experiment in AI-assisted software modernization. The goal was to study how capable coding agents are at rewriting real-world tools — not to take credit away from the original authors. pigz is the work of [Mark Adler](https://en.wikipedia.org/wiki/Mark_Adler), co-creator of zlib, gzip, and the DEFLATE format, who invested countless hours building and maintaining it. pigzpp exists because of that foundation.
>
> Read the full writeup: **[I Let Two AI Agents Race to Modernize pigz](https://gowda.ai/posts/2026/03/pigzpp-with-agents/)**

**pigzpp** is a clean-room C++23 rewrite of [pigz](https://zlib.net/pigz/) (Parallel Implementation of GZip) by Mark Adler. It is a drop-in replacement for `pigz`/`gzip` that is **faster**, **thread-safe**, and usable as both a CLI tool and a library with **Python, Go, Rust, and WebAssembly** bindings. It also includes a compact PNG encoder/decoder for grayscale, grayscale+alpha, RGB, and RGBA `uint8` images, using the same accelerated DEFLATE stack for PNG `IDAT` data.

## Why

pigz is one of those essential tools — if you've ever compressed GBs to TBs of data, you've probably used it. But pigz was written as a monolithic C program with a single global state (`struct g`, ~60 mutable fields), making it impossible to use as a library. pigzpp fixes this:

- **Thread-safe library** — no global state, multiple compress/decompress operations in one process
- **Faster** — [zlib-ng](https://github.com/zlib-ng/zlib-ng) (SIMD) and [ISA-L](https://github.com/intel/isa-l) (hand-tuned assembly) DEFLATE backends
- **Selectable backend** — `auto` (ISA-L, fastest), `zlib` (zlib-ng, best ratio), or `isal`, via API and the `--engine` CLI flag
- **Modern C++23** — `std::jthread`, exceptions, RAII, no `setjmp`/`longjmp`
- **Bindings for many languages** — Python (pybind11), Go (cgo), Rust (FFI), and WebAssembly (Embind), all sharing one accelerated core
- **PNG helpers** — encode/decode grayscale, grayscale+alpha, RGB, and RGBA image buffers
- **Fully compatible** — compress with pigzpp, decompress with gzip/pigz, and vice versa

## Performance

Benchmarked on an **Intel Xeon W-2235 (5 cores / 10 threads, 3.80 GHz)**, Ubuntu 22.04, on a **128 MB multilingual-text corpus** (English + Chinese Wikipedia; see `benchmarks/core/gen_data.py`) at **level 6, 8 threads**. Throughput is best-of-3.

pigzpp ships two DEFLATE backends: **ISA-L** (`auto`/`isal`, fastest) and **zlib-ng** (`zlib`, best ratio). `ratio` is input/output (higher = smaller output).

**CLI — pigzpp vs gzip and pigz:**

| tool | MB/s | ratio |
|---|---|---|
| gzip 1.12 (`-6`) | 16 | 2.83 |
| pigz 2.6 (`-6 -p8`) | 103 | 2.83 |
| pigzpp (`--engine zlib`) | 244 | 2.81 |
| pigzpp (`--engine isal`, default) | **721** | 2.58 |

pigzpp's zlib-ng backend is **2.4× faster than pigz** at the same ratio; ISA-L is **7× faster** (trading ~9% ratio for speed).

**Language bindings — pigzpp vs the best competitor in each ecosystem** (128 MB text, L6, 8 threads):

| language | pigzpp `isal` | pigzpp `zlib` | best competitor |
|---|---|---|---|
| **Python** | 959 MB/s | 279 MB/s | `gzip` (stdlib) 12 MB/s |
| **Go** | 771 MB/s | 272 MB/s | `klauspost/pgzip` 281 MB/s |
| **Rust** | 745 MB/s | 261 MB/s | `gzp` (parallel) 194 MB/s |

Ratios: pigzpp `isal` 2.58, pigzpp `zlib` 2.81. In every language pigzpp is fastest, and its parallel **zlib-ng** backend matches or beats the best parallel competitor (`pgzip`, `gzp`) at an *equal or better* ratio.

**WebAssembly** (zlib-ng + 128-bit SIMD; ISA-L is x86-only, so not available in WASM; Node 22). Single-thread, 16 MB text:

| engine | MB/s | ratio |
|---|---:|---:|
| **pigzpp-wasm** | **45** | 2.81 |
| node-zlib | 33 | 2.84 |
| CompressionStream (native) | 27 | 2.84 |
| fflate (JS) | 15 | 2.75 |
| pako (JS) | 9 | 2.83 |

Even single-threaded, pigzpp-wasm (zlib-ng + SIMD) beats the browser's native `CompressionStream` and the popular JS libraries.

The threaded WASM build (pthreads via Web Workers + `SharedArrayBuffer`) scales across cores. On a **128 MB** corpus (level 6), on this 5-physical-core WSL2 host (`CompressionStream` and `node-zlib` are single-thread only, ~28 and ~32 MB/s here):

| threads | MB/s | speedup |
|---:|---:|---:|
| 1 | 38 | 1.00x |
| 2 | 74 | 1.93x |
| 4 | 127 | 3.32x |
| 5 | 152 | 3.96x |
| 8 | 179 | 4.66x |

At 8 threads pigzpp-wasm reaches **~179 MB/s** (4.7x over single-thread, ~6x native `CompressionStream`). Browsers must be cross-origin isolated (COOP/COEP headers) to enable `SharedArrayBuffer`, and blocking compress calls should run in a Web Worker. Reproduce with `node benchmarks/wasm/scaling.mjs --size 128 --threads 1,2,4,5,8` after `scripts/build_wasm.sh threads`.

Benchmarks live under `benchmarks/` (`core`, `python`, `go-docker`, `rust`, `wasm`), all reading the shared corpus in `build/bench_data/`. See [notes/05-summary.md](notes/05-summary.md) for the earlier large-core CLI runs (48-core Xeon) and thread-scaling detail.

## Build

Requires CMake 3.20+, a C++23 compiler (GCC 13+ or Clang 17+), and Python 3.10+ (for bindings).

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
- `build/pigzpp.cpython-*.so` — Python module

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

`pigzpp.png` currently targets fast lossless grayscale/grayscale+alpha/RGB/RGBA image buffers. It writes standard 8-bit non-interlaced PNG files, validates chunk CRCs on decode, and supports PNG filters (`none`, `sub`, `up`, `average`, `paeth`, `adaptive-fast`, `adaptive-all`) plus DEFLATE strategies (`default`, `rle`, `huffman`, `fixed`, `filtered`). Presets are available as `fast`, `balanced`, and `small`; `fast` uses `level=1`, `strategy="rle"`, and `filter="up"`, `balanced` uses `level=1`, `strategy="rle"`, and `filter="adaptive-fast"`, while `small` uses `level=9`, `strategy="filtered"`, and `filter="adaptive-all"`. Explicit `level`, `strategy`, or `filter` arguments override the selected preset. With ISA-L enabled, PNG decode uses ISA-L when possible; PNG encode uses ISA-L for `strategy="default"` and falls back to zlib-ng for other zlib-style strategies.

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
│   ├── crc.h/cpp         CRC-32/Adler-32 with optimized combine
│   ├── pool.h/cpp        Thread-safe buffer pool (RAII)
│   ├── format.h/cpp      Gzip/zlib header/trailer parsing
│   ├── io.h/cpp          Buffered I/O with EINTR retry
│   ├── capi.h/cpp        C ABI (libpigzppc) for FFI bindings
│   └── main.cpp          CLI entry point
├── src/python/           pybind11 bindings
├── src/go/               Go binding (cgo → libpigzppc)
├── src/rust/             Rust binding (FFI → libpigzppc)
├── src/wasm/             WebAssembly binding (Embind)
├── tests/                GoogleTest + pytest
├── benchmarks/           core, python, png, go-docker, rust, wasm suites
├── third_party/          zlib-ng, ISA-L, pybind11, zopfli
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
- [pybind11](https://github.com/pybind/pybind11) — C++/Python bindings (BSD license)
- [zopfli](https://github.com/google/zopfli) — optimal DEFLATE compressor for level 11 (Apache 2.0)
- [GoogleTest](https://github.com/google/googletest) — testing framework (BSD license)

## License

[zlib license](LICENSE) — same as the original pigz. Free for any use including commercial.
