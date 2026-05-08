# pigzpp — Parallel gzip, rewritten in C++23

> **Note:** This project is an experiment in AI-assisted software modernization. The goal was to study how capable coding agents are at rewriting real-world tools — not to take credit away from the original authors. pigz is the work of [Mark Adler](https://en.wikipedia.org/wiki/Mark_Adler), co-creator of zlib, gzip, and the DEFLATE format, who invested countless hours building and maintaining it. pigzpp exists because of that foundation.
>
> Read the full writeup: **[I Let Two AI Agents Race to Modernize pigz](https://gowda.ai/posts/2026/03/pigzpp-with-agents/)**

**pigzpp** is a clean-room C++23 rewrite of [pigz](https://zlib.net/pigz/) (Parallel Implementation of GZip) by Mark Adler. It is a drop-in replacement for `pigz`/`gzip` that is **faster**, **thread-safe**, and usable as both a CLI tool and a library with Python bindings. It also includes a compact PNG encoder/decoder for grayscale, grayscale+alpha, RGB, and RGBA `uint8` images, using the same accelerated DEFLATE stack for PNG `IDAT` data.

## Why

pigz is one of those essential tools — if you've ever compressed GBs to TBs of data, you've probably used it. But pigz was written as a monolithic C program with a single global state (`struct g`, ~60 mutable fields), making it impossible to use as a library. pigzpp fixes this:

- **Thread-safe library** — no global state, multiple compress/decompress operations in one process
- **Faster** — uses [zlib-ng](https://github.com/zlib-ng/zlib-ng) with SIMD optimizations instead of vanilla zlib
- **Modern C++23** — `std::jthread`, exceptions, RAII, no `setjmp`/`longjmp`
- **Python bindings** — `pigzpp.open()` / `pigzpp.compress()` via pybind11
- **PNG helpers** — `pigzpp.png.compress()` / `pigzpp.png.decompress()` / `pigzpp.png.save()` / `pigzpp.png.load()` for grayscale, grayscale+alpha, RGB, and RGBA image buffers
- **Fully compatible** — compress with pigzpp, decompress with gzip/pigz, and vice versa

## Performance

Benchmarked on a 48-core Intel Xeon, Ubuntu 24.04.3 LTS:

**CLI compression throughput (MB/s):**

| Size | gzip 1.12 | pigz 2.8 | pigzpp | igzip (ISA-L) |
|---|---|---|---|---|
| 16 MB | 145 | 772 | 954 | **1824** |
| 128 MB | 189 | 1597 | **2487** | 2542 |
| 1024 MB | 199 | 1862 | **3365** | 2989 |

pigzpp is **1.8x faster** than pigz on compression and **2.4x faster** on decompression at default settings. At single-thread, the gap widens to **5.7x** (pure zlib-ng vs zlib).

See [notes/05-summary.md](notes/05-summary.md) for full benchmarks including Python bindings, decompression, thread scaling, and comparison with zlib-ng and ISA-L.

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
pigzpp -p 8 -6 -c file.txt > file.gz   # 8 threads, level 6
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

`pigzpp.png` currently targets fast lossless grayscale/grayscale+alpha/RGB/RGBA image buffers. It writes standard 8-bit non-interlaced PNG files, validates chunk CRCs on decode, and supports PNG filters (`none`, `sub`, `up`, `average`, `paeth`, `adaptive-fast`, `adaptive-all`) plus DEFLATE strategies (`default`, `rle`, `huffman`, `fixed`, `filtered`). Presets are available as `fast`, `balanced`, and `small`; `fast` uses `level=1`, `strategy="rle"`, and `filter="up"`, `balanced` uses `level=1`, `strategy="rle"`, and `filter="adaptive-fast"`, while `small` uses `level=9`, `strategy="filtered"`, and `filter="adaptive-all"`. Explicit `level`, `strategy`, or `filter` arguments override the selected preset. With ISA-L enabled, PNG decode uses ISA-L when possible; PNG encode uses ISA-L for `strategy="default"` and falls back to zlib-ng for other zlib-style strategies.

PNG benchmarks compare `pigzpp.png`, OpenCV, and Pillow modes against `PIL.Image.save(..., format="PNG")`, the common Python PNG baseline. A `*` in the benchmark output marks each library's default mode. Benchmarks can be run on RGB, grayscale, or binary mask inputs:

```bash
python benchmarks/bench_png.py --mode rgb --verify
python benchmarks/bench_png.py --mode gray --verify
python benchmarks/bench_png.py --mode mask --verify
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
│   ├── compress.h/cpp    Parallel compressor
│   ├── decompress.h/cpp  Decompressor with parallel CRC
│   ├── png.h/cpp         PNG encode/decode helpers
│   ├── crc.h/cpp         CRC-32/Adler-32 with optimized combine
│   ├── pool.h/cpp        Thread-safe buffer pool (RAII)
│   ├── format.h/cpp      Gzip/zlib header/trailer parsing
│   ├── io.h/cpp          Buffered I/O with EINTR retry
│   └── main.cpp          CLI entry point
├── src/python/            pybind11 bindings
├── tests/                 GoogleTest + pytest
├── benchmarks/            Performance benchmarks
├── third_party/           zlib-ng, pybind11, zopfli
└── notes/                 Development notes and blog post
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
