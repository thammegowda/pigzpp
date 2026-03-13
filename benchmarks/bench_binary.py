#!/usr/bin/env python3
"""Benchmark CLI binaries: gzip vs pigz vs pigzpp.

Produces clean tables of throughput (MB/s) with speedup over pigz.

Usage:
    python benchmarks/bench_binary.py
    python benchmarks/bench_binary.py --sizes 1 10 100 --iterations 3
"""

import argparse
import shutil
import subprocess
import tempfile
import time
from pathlib import Path


def find_binaries(pigzpp_bin):
    """Return ordered dict: pigz, gzip, igzip, pigzpp.
    pigz is baseline. gzip included for small sizes only. pigzpp last."""
    bins = {}
    pigz_path = shutil.which("pigz")
    if pigz_path:
        bins["pigz"] = pigz_path
    gzip_path = shutil.which("gzip")
    if gzip_path:
        bins["gzip"] = gzip_path
    igzip_path = shutil.which("igzip")
    if igzip_path:
        bins["igzip"] = igzip_path
    p = Path(pigzpp_bin)
    if p.is_file() and p.stat().st_mode & 0o111:
        bins["pigzpp"] = pigzpp_bin
    return bins


def get_version(name, path):
    """Get version string for a binary."""
    for flag in ["-V", "--version"]:
        try:
            r = subprocess.run([path, flag], capture_output=True, text=True, timeout=5)
            out = (r.stdout + r.stderr).strip().split("\n")[0]
            if out and "invalid" not in out.lower() and "unknown" not in out.lower():
                return out
        except Exception:
            pass
    return "unknown"


def bench_cmd(cmd, iterations):
    """Run a shell command, return median wall-clock seconds."""
    # warmup
    subprocess.run(cmd, shell=True, check=True, stdout=subprocess.DEVNULL,
                   stderr=subprocess.DEVNULL)
    times = []
    for _ in range(iterations):
        t0 = time.perf_counter()
        subprocess.run(cmd, shell=True, check=True, stdout=subprocess.DEVNULL,
                       stderr=subprocess.DEVNULL)
        times.append(time.perf_counter() - t0)
    times.sort()
    return times[len(times) // 2]


def make_gz(f, gz):
    subprocess.run(f"pigz -c {f} > {gz}", shell=True, check=True)


def print_table(title, sizes, names, results, data_sizes):
    """Print a table with throughput and speedup vs pigz."""
    print(f"\n=== {title} ===\n")
    hdr = f"{'Size':>10}"
    for n in names:
        hdr += f"  {n:>14}"
    print(hdr)
    print("-" * len(hdr))

    for size in sizes:
        row = f"{size:>7} MB"
        base_mbs = None
        for name in names:
            if (size, name) not in results:
                row += f"  {'—':>14}"
                continue
            t = results[(size, name)]
            mbs = data_sizes[size] / t / 1e6
            if base_mbs is None:
                base_mbs = mbs  # first column (pigz) is the baseline
            sp = mbs / base_mbs if base_mbs > 0 else 0
            row += f"  {mbs:>6.0f} ({sp:.1f}x)"
        print(row)


# Max size (MB) for including gzip in benchmarks (too slow for large files)
GZIP_MAX_SIZE_MB = 256


def run_default_benchmarks(bins, data_dir, sizes, iterations):
    """Default thread count benchmarks for each data type."""
    all_names = list(bins.keys())

    for dtype, ext, label in [("text", ".txt", "Text Data"),
                               ("random", ".bin", "Random Data")]:
        data_sizes = {}
        compress_results = {}
        decompress_results = {}
        valid_sizes = []

        for size in sizes:
            f = Path(data_dir) / f"{size}MB{ext}"
            if not f.is_file():
                continue
            valid_sizes.append(size)
            data_sizes[size] = f.stat().st_size

            # Skip gzip for large files (too slow)
            names_for_size = [n for n in all_names
                              if n != "gzip" or size <= GZIP_MAX_SIZE_MB]

            for name in names_for_size:
                cmd = f"{bins[name]} -c {f} > /dev/null"
                compress_results[(size, name)] = bench_cmd(cmd, iterations)

            gz = Path(tempfile.mktemp(suffix=".gz"))
            try:
                make_gz(f, gz)
                for name in names_for_size:
                    cmd = f"{bins[name]} -dc {gz} > /dev/null"
                    decompress_results[(size, name)] = bench_cmd(cmd, iterations)
            finally:
                gz.unlink(missing_ok=True)

        if not valid_sizes:
            continue

        print_table(f"Compression — {label} (MB/s, speedup vs pigz)",
                    valid_sizes, all_names, compress_results, data_sizes)
        print_table(f"Decompression — {label} (MB/s, speedup vs pigz)",
                    valid_sizes, all_names, decompress_results, data_sizes)


def run_thread_scaling(bins, data_dir, sizes, iterations, thread_counts):
    """Benchmark pigz and pigzpp at specific thread counts."""
    parallel_bins = {k: v for k, v in bins.items() if k in ("pigz", "pigzpp")}
    if len(parallel_bins) < 2:
        return
    pnames = list(parallel_bins.keys())

    for dtype, ext, label in [("text", ".txt", "Text Data"),
                               ("random", ".bin", "Random Data")]:
        data_sizes = {}
        valid_sizes = []
        for size in sizes:
            f = Path(data_dir) / f"{size}MB{ext}"
            if not f.is_file():
                continue
            valid_sizes.append(size)
            data_sizes[size] = f.stat().st_size

        if not valid_sizes:
            continue

        for mode, mlabel in [("compress", "Compression"), ("decompress", "Decompression")]:
            print(f"\n=== Thread Scaling: {mlabel} — {label} (MB/s) ===\n")
            hdr = f"{'Size':>10}  {'threads':>7}"
            for n in pnames:
                hdr += f"  {n:>12}"
            if "pigz" in pnames and "pigzpp" in pnames:
                hdr += f"  {'speedup':>10}"
            print(hdr)
            print("-" * len(hdr))

            for size in valid_sizes:
                f = Path(data_dir) / f"{size}MB{ext}"
                gz = None
                if mode == "decompress":
                    gz = Path(tempfile.mktemp(suffix=".gz"))
                    make_gz(f, gz)
                try:
                    for t in thread_counts:
                        row = f"{size:>7} MB  {t:>7}"
                        mbs_by_name = {}
                        for name in pnames:
                            if mode == "compress":
                                cmd = f"{parallel_bins[name]} -p {t} -c {f} > /dev/null"
                            else:
                                cmd = f"{parallel_bins[name]} -p {t} -dc {gz} > /dev/null"
                            med = bench_cmd(cmd, iterations)
                            mbs = data_sizes[size] / med / 1e6
                            mbs_by_name[name] = mbs
                            row += f"  {mbs:>9.0f} MB/s"
                        if "pigz" in mbs_by_name and "pigzpp" in mbs_by_name:
                            sp = mbs_by_name["pigzpp"] / mbs_by_name["pigz"]
                            row += f"  {sp:>9.1f}x"
                        print(row)
                finally:
                    if gz:
                        gz.unlink(missing_ok=True)


def main():
    parser = argparse.ArgumentParser(description="Benchmark pigzpp vs gzip/pigz binaries")
    parser.add_argument("--sizes", type=int, nargs="+", default=[16, 128, 1024])
    parser.add_argument("--iterations", type=int, default=3)
    parser.add_argument("--threads", type=int, nargs="+", default=[1, 4, 16])
    parser.add_argument("--pigzpp", default="build/pigzpp")
    parser.add_argument("--data-dir", default="build/bench_data")
    args = parser.parse_args()

    bins = find_binaries(args.pigzpp)
    if not bins:
        print("No binaries found")
        return 1

    print("Binaries:")
    for name, path in bins.items():
        print(f"  {name:>8}: {path} -- {get_version(name, path)}")
    print(f"Sizes: {args.sizes} MB | Iterations: {args.iterations} | Threads: {args.threads}")

    run_default_benchmarks(bins, args.data_dir, args.sizes, args.iterations)
    run_thread_scaling(bins, args.data_dir, args.sizes, args.iterations, args.threads)


if __name__ == "__main__":
    main()
