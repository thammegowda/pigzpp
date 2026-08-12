#!/usr/bin/env python3
"""Benchmark pigzpp.ZipFile against Python's standard-library zipfile.

Both write real ZIP archives (DEFLATE members) to a temp file and read them
back. pigzpp compresses each member with the parallel zlib-ng / ISA-L engines;
the stdlib uses single-threaded zlib. Uses the shared realistic corpus from
benchmarks/core (build/bench_data/{N}MB.txt)."""

from __future__ import annotations

import argparse
import io
import os
import sys
import tempfile
import time
import zipfile
from pathlib import Path

# Reuse the shared corpus loader/generator from the python suite.
sys.path.insert(0, str(Path(__file__).resolve().parent))
sys.path.insert(0, str(Path(__file__).resolve().parent.parent / "core"))

import pigzpp
from bench_python import load_corpus, DEFAULT_DATA_DIR


def best_of(func, iterations: int):
    """Run func() iterations times; return (best_seconds, last_result)."""
    func()  # warmup
    best = float("inf")
    result = None
    for _ in range(iterations):
        t0 = time.perf_counter()
        result = func()
        best = min(best, time.perf_counter() - t0)
    return best, result


def split_members(data: bytes, members: int) -> list[tuple[str, bytes]]:
    """Split a blob into `members` roughly equal named chunks."""
    n = max(1, members)
    step = len(data) // n
    out = []
    for i in range(n):
        start = i * step
        end = len(data) if i == n - 1 else start + step
        out.append((f"member_{i:03d}.txt", data[start:end]))
    return out


# ---- Writers -------------------------------------------------------------

def write_stdlib(path: str, parts, level: int):
    with zipfile.ZipFile(path, "w", zipfile.ZIP_DEFLATED, compresslevel=level) as z:
        for name, blob in parts:
            z.writestr(name, blob)
    return os.path.getsize(path)


def write_pigzpp(path: str, parts, level: int, threads: int, engine: str):
    with pigzpp.ZipFile(path, "w", compresslevel=level, threads=threads, engine=engine) as z:
        for name, blob in parts:
            z.writestr(name, blob)
    return os.path.getsize(path)


# ---- Readers -------------------------------------------------------------

def read_stdlib(path: str) -> int:
    total = 0
    with zipfile.ZipFile(path) as z:
        for name in z.namelist():
            total += len(z.read(name))
    return total


def read_pigzpp(path: str) -> int:
    total = 0
    with pigzpp.ZipFile(path) as z:
        for name in z.namelist():
            total += len(z.read(name))
    return total


def fmt(x, d=1):
    return f"{x:.{d}f}"


def run_scenario(data: bytes, members: int, level: int, threads: int,
                 engines: list[str], iterations: int, tmpdir: str):
    parts = split_members(data, members)
    nbytes = len(data)
    mb = nbytes / 1e6

    writers = [("zipfile (stdlib)", lambda p: write_stdlib(p, parts, level))]
    for eng in engines:
        writers.append((f"pigzpp:{eng} (t={threads})",
                        lambda p, e=eng: write_pigzpp(p, parts, level, threads, e)))

    print(f"\n### {mb:.0f} MB corpus, {members} member(s), level {level}, "
          f"{threads} threads (best of {iterations})\n")
    print("| writer | write MB/s | ratio | read MB/s |")
    print("|---|---:|---:|---:|")

    for label, wfn in writers:
        path = os.path.join(tmpdir, "bench.zip")
        wsec, size = best_of(lambda: wfn(path), iterations)
        is_pigzpp = label.startswith("pigzpp")
        reader = read_pigzpp if is_pigzpp else read_stdlib
        rsec, got = best_of(lambda: reader(path), iterations)
        assert got == nbytes, f"{label}: read {got} != {nbytes}"
        ratio = nbytes / size
        print(f"| {label} | {fmt(mb / wsec)} | {fmt(ratio, 3)} | {fmt(mb / rsec)} |")
        os.remove(path)


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("--sizes", default="128", help="Comma-separated corpus sizes in MB")
    p.add_argument("--members", default="1,16", help="Comma-separated member counts")
    p.add_argument("--level", type=int, default=6)
    p.add_argument("--threads", type=int, default=8)
    p.add_argument("--engines", default="isal,zlib", help="pigzpp engines to test")
    p.add_argument("--iterations", type=int, default=3)
    p.add_argument("--data-dir", default=DEFAULT_DATA_DIR)
    args = p.parse_args()

    sizes = [int(s) for s in args.sizes.split(",")]
    member_counts = [int(m) for m in args.members.split(",")]
    engines = [e.strip() for e in args.engines.split(",") if e.strip()]

    print(f"# pigzpp.ZipFile vs zipfile (stdlib)\n\npigzpp: {pigzpp.__doc__.splitlines()[0] if pigzpp.__doc__ else 'pigzpp'}")

    with tempfile.TemporaryDirectory() as tmpdir:
        for size_mb in sizes:
            data = load_corpus(args.data_dir, size_mb, kind="text")
            for members in member_counts:
                run_scenario(data, members, args.level, args.threads,
                             engines, args.iterations, tmpdir)


if __name__ == "__main__":
    main()
