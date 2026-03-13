#!/usr/bin/env python3
"""Generate benchmark data files (text and random) for pigzpp benchmarks."""

import argparse
import subprocess
import sys
from pathlib import Path


def gen_text(path: Path, size_mb: int) -> None:
    """Generate repetitive text data by streaming line-by-line."""
    target = size_mb * 1024 * 1024
    line = ("The quick brown fox jumps over the lazy dog. " * 2 + "\n").encode()
    n = target // len(line)
    with open(path, "wb") as f:
        for _ in range(n):
            f.write(line)


def gen_random(path: Path, size_mb: int) -> None:
    """Generate random (incompressible) data via dd."""
    subprocess.run(
        ["dd", "if=/dev/urandom", f"of={path}", "bs=1M", f"count={size_mb}"],
        check=True, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
    )


def main() -> None:
    parser = argparse.ArgumentParser(description="Generate benchmark data files")
    parser.add_argument("--sizes", type=int, nargs="+", required=True,
                        help="Sizes in MB (e.g. 16 128 1024 8192)")
    parser.add_argument("--data-dir", type=Path, required=True,
                        help="Output directory for data files")
    args = parser.parse_args()

    args.data_dir.mkdir(parents=True, exist_ok=True)

    for size in args.sizes:
        for ext, label, gen_fn in [(".txt", "text", gen_text),
                                    (".bin", "random", gen_random)]:
            path = args.data_dir / f"{size}MB{ext}"
            if path.is_file():
                h = subprocess.check_output(["du", "-h", str(path)]).decode().split()[0]
                print(f"{path} already exists ({h})")
                continue
            print(f"Generating {size}MB {label} data → {path}")
            gen_fn(path, size)

    print("✓ Data generation complete")


if __name__ == "__main__":
    main()
