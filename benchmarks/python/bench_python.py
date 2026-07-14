#!/usr/bin/env python3
"""Benchmark pigzpp vs Python gzip/zlib-ng/isal libraries.

Compares in-memory (bytes) and file-based compression/decompression across:
  - gzip        (stdlib, bundled zlib)
  - zlib_ng     (pip install zlib-ng)
  - isal        (pip install isal, Intel ISA-L accelerated)
  - pigzpp      (this project, parallel zlib-ng)

Usage:
    python benchmarks/python/bench_python.py
    python benchmarks/python/bench_python.py --sizes 1 10 100
    python benchmarks/python/bench_python.py --iterations 5
    python benchmarks/python/bench_python.py --bytes-only
    python benchmarks/python/bench_python.py --file-only
"""

import argparse
import gzip
import os
import sys
import tempfile
import time
from pathlib import Path

# Make the core benchmark suite importable (shared realistic dataset generator
# and the pigz_sp helper both live in benchmarks/core).
sys.path.insert(0, str(Path(__file__).resolve().parent.parent / "core"))

import pigzpp
import gen_data  # realistic multilingual/random datasets (benchmarks/core)

DEFAULT_DATA_DIR = "build/bench_data"

# Optional libraries — gracefully degrade if not installed
try:
    from zlib_ng import gzip_ng
    HAS_ZLIB_NG = True
except ImportError:
    HAS_ZLIB_NG = False

try:
    from isal import igzip
    HAS_ISAL = True
except ImportError:
    HAS_ISAL = False

try:
    from benchmarks.pigz_sp import pigz as pigz_sp
    HAS_PIGZ_SP = pigz_sp.is_available()
except ImportError:
    try:
        from pigz_sp import pigz as pigz_sp
        HAS_PIGZ_SP = pigz_sp.is_available()
    except ImportError:
        HAS_PIGZ_SP = False


def generate_text(size_mb: int) -> str:
    """Generate repeating text data of approximately size_mb megabytes."""
    line = "The quick brown fox jumps over the lazy dog. " * 2 + "\n"
    count = (size_mb * 1024 * 1024) // len(line)
    return line * count


def load_corpus(data_dir, size_mb: int, kind: str = "text") -> bytes:
    """Load a realistic benchmark corpus from the shared core dataset dir.

    Reuses benchmarks/core/gen_data.py: kind="text" -> {N}MB.txt (multilingual
    Wikipedia), kind="random" -> {N}MB.bin (incompressible). The file is
    generated and cached on first use, so all benchmark suites share one corpus.
    """
    data_dir = Path(data_dir)
    ext = ".bin" if kind == "random" else ".txt"
    path = data_dir / f"{size_mb}MB{ext}"
    if not path.is_file():
        data_dir.mkdir(parents=True, exist_ok=True)
        if kind == "random":
            gen_data.gen_random(path, size_mb)
        else:
            seed = gen_data.fetch_multilingual_seed(data_dir)
            gen_data.gen_text(path, size_mb, seed)
    return path.read_bytes()


def bench(func, iterations: int) -> float:
    """Run func() for iterations, return median time in seconds."""
    times = []
    for _ in range(iterations):
        t0 = time.perf_counter()
        func()
        times.append(time.perf_counter() - t0)
    times.sort()
    return times[len(times) // 2]


def _available_libs():
    """Return list of (name, has_bytes, has_file) for available libraries."""
    libs = [("gzip", True, True)]
    if HAS_PIGZ_SP:
        libs.append(("pigz_sp", False, True))
    if HAS_ZLIB_NG:
        libs.append(("zlib_ng", True, True))
    libs.append(("pigzpp", True, True))
    if HAS_ISAL:
        libs.append(("isal", True, True))
    return libs


def _bytes_compress(name, raw):
    if name == "gzip":
        return gzip.compress(raw)
    elif name == "pigzpp":
        return pigzpp.compress(raw)
    elif name == "zlib_ng":
        return gzip_ng.compress(raw)
    elif name == "isal":
        return igzip.compress(raw)


def _bytes_decompress(name, data):
    if name == "gzip":
        return gzip.decompress(data)
    elif name == "pigzpp":
        return pigzpp.decompress(data)
    elif name == "zlib_ng":
        return gzip_ng.decompress(data)
    elif name == "isal":
        return igzip.decompress(data)


def _file_compress(name, path, text):
    if name == "gzip":
        with gzip.open(path, "wt") as f:
            f.write(text)
    elif name == "pigzpp":
        with pigzpp.open(path, "w") as f:
            f.write(text)
    elif name == "zlib_ng":
        with gzip_ng.open(path, "wt") as f:
            f.write(text)
    elif name == "isal":
        with igzip.open(path, "wt") as f:
            f.write(text)
    elif name == "pigz_sp":
        with pigz_sp(path, "w") as f:
            f.write(text)


def _file_decompress(name, path):
    if name == "gzip":
        with gzip.open(path, "rt") as f:
            return f.read()
    elif name == "pigzpp":
        with pigzpp.open(path, "r") as f:
            return f.read()
    elif name == "zlib_ng":
        with gzip_ng.open(path, "rt") as f:
            return f.read()
    elif name == "isal":
        with igzip.open(path, "rt") as f:
            return f.read()
    elif name == "pigz_sp":
        with pigz_sp(path, "r") as f:
            return "".join(f)


def _fmt_cell(data_bytes, t, base_t=None):
    mbs = data_bytes / t / 1e6
    if base_t:
        sp = base_t / t
        return f"{mbs:.0f} ({sp:.1f}x)"
    return f"{mbs:.0f} (1.0x)"


def _print_table(title, sizes_mb, names, timings, data_sizes, baseline="gzip"):
    """Print table with MB/s and speedup vs baseline (gzip)."""
    COL = 11
    print(f"\n=== {title} (MB/s, xGzip) ===\n")
    hdr = f"{'Size':>10}"
    for n in names:
        hdr += f"  {n:>{COL}}"
    print(hdr)
    print("-" * (12 + (COL + 2) * len(names)))

    for size_mb in sizes_mb:
        row = f"{size_mb:>7} MB"
        base_t = timings.get((size_mb, baseline))
        for name in names:
            t = timings[(size_mb, name)]
            row += f"  {_fmt_cell(data_sizes[size_mb], t, base_t if name != baseline else None):>{COL}}"
        print(row)


def bench_file_api(sizes_mb: list[int], iterations: int, data_dir):
    """Benchmark file-based compress/decompress across all available libraries."""
    libs = _available_libs()
    names = [n for n, _, _ in libs]
    comp_times = {}
    decomp_times = {}
    data_sizes = {}

    for size_mb in sizes_mb:
        text = load_corpus(data_dir, size_mb).decode("utf-8", "replace")
        data_sizes[size_mb] = len(text.encode("utf-8"))

        with tempfile.TemporaryDirectory() as tmpdir:
            for name in names:
                path = os.path.join(tmpdir, f"{name}.gz")
                comp_times[(size_mb, name)] = bench(
                    lambda n=name, p=path: _file_compress(n, p, text), iterations)

            # Create compressed files for decompression
            paths = {}
            for name in names:
                path = os.path.join(tmpdir, f"{name}.gz")
                _file_compress(name, path, text)
                paths[name] = path

            for name in names:
                decomp_times[(size_mb, name)] = bench(
                    lambda n=name: _file_decompress(n, paths[n]), iterations)

    _print_table("File API: Compression", sizes_mb, names, comp_times, data_sizes)
    _print_table("File API: Decompression", sizes_mb, names, decomp_times, data_sizes)


def bench_bytes_api(sizes_mb: list[int], iterations: int, data_dir):
    """Benchmark in-memory compress/decompress across all available libraries."""
    libs = _available_libs()
    names = [n for n, has_bytes, _ in libs if has_bytes]
    comp_times = {}
    decomp_times = {}
    data_sizes = {}

    for size_mb in sizes_mb:
        raw = load_corpus(data_dir, size_mb)
        data_sizes[size_mb] = len(raw)

        for name in names:
            comp_times[(size_mb, name)] = bench(
                lambda n=name: _bytes_compress(n, raw), iterations)

        compressed = {}
        for name in names:
            compressed[name] = _bytes_compress(name, raw)

        for name in names:
            decomp_times[(size_mb, name)] = bench(
                lambda n=name: _bytes_decompress(n, compressed[n]), iterations)

    _print_table("Bytes API: Compression", sizes_mb, names, comp_times, data_sizes)
    _print_table("Bytes API: Decompression", sizes_mb, names, decomp_times, data_sizes)


def main():
    parser = argparse.ArgumentParser(description="Benchmark pigzpp vs gzip/zlib-ng/isal")
    parser.add_argument("--sizes", type=int, nargs="+", default=[16, 128, 1024],
                        help="Data sizes in MB (default: 16 128 1024)")
    parser.add_argument("--iterations", type=int, default=3,
                        help="Iterations per benchmark (default: 3, median reported)")
    parser.add_argument("--file-only", action="store_true",
                        help="Only run file API benchmarks")
    parser.add_argument("--bytes-only", action="store_true",
                        help="Only run bytes API benchmarks")
    parser.add_argument("--data-dir", default=DEFAULT_DATA_DIR,
                        help="Shared dataset dir (default: build/bench_data); "
                             "realistic corpora are generated here on first use")
    args = parser.parse_args()

    import zlib
    print("pigzpp Python benchmark")
    print(f"Python: {sys.version.split()[0]}")
    print(f"Libraries:")
    print(f"  gzip:    stdlib (zlib {zlib.ZLIB_RUNTIME_VERSION})")
    if HAS_PIGZ_SP:
        import shutil
        pigz_path = shutil.which("pigz")
        print(f"  pigz_sp: subprocess ({pigz_path})")
    else:
        print(f"  pigz_sp: not available (pigz not in PATH)")
    if HAS_ZLIB_NG:
        import zlib_ng
        print(f"  zlib_ng: {zlib_ng.__version__}")
    else:
        print(f"  zlib_ng: not installed (pip install zlib-ng)")
    print(f"  pigzpp:  {pigzpp.__version__ if hasattr(pigzpp, '__version__') else 'installed'}")
    if HAS_ISAL:
        import isal
        print(f"  isal:    {isal.__version__}")
    else:
        print(f"  isal:    not installed (pip install isal)")
    print(f"Sizes: {args.sizes} MB | Iterations: {args.iterations} | CPUs: {os.cpu_count()}")

    run_file = not args.bytes_only
    run_bytes = not args.file_only

    if run_file:
        bench_file_api(args.sizes, args.iterations, args.data_dir)
    if run_bytes:
        bench_bytes_api(args.sizes, args.iterations, args.data_dir)


if __name__ == "__main__":
    main()
