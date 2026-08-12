#!/usr/bin/env python3
"""Generate benchmark data files (multilingual text and random) for pigzpp benchmarks.

Text data is sourced from real English and Chinese Wikipedia articles, downloaded
as parquet files from HuggingFace and cached locally as a UTF-8 seed.  The seed
is truncated or repeated to meet the requested target size.

Sources (all CC BY-SA):
  - English: Salesforce/wikitext  (Wikitext-103, Merity et al.)
  - Chinese: wikimedia/wikipedia  (Chinese Wikipedia, 2023-11-01 dump)
"""

import argparse
import io
import subprocess
import sys
import tempfile
import urllib.request
from pathlib import Path

# Direct-download parquet URLs (no auth, no datasets library needed).
SOURCES = [
    {
        "label": "English (Wikitext-103)",
        "url": (
            "https://huggingface.co/datasets/Salesforce/wikitext/resolve/main/"
            "wikitext-103-raw-v1/train-00000-of-00002.parquet"
        ),
        "text_col": "text",
    },
    {
        "label": "Chinese (Wikipedia)",
        "url": (
            "https://huggingface.co/datasets/wikimedia/wikipedia/resolve/"
            "refs%2Fconvert%2Fparquet/20231101.zh/train/0000.parquet"
        ),
        "text_col": "text",
    },
]

SEED_FILENAME = "multilingual_seed.txt"


def _download(url: str) -> bytes:
    """Download *url* with a progress indicator."""
    req = urllib.request.Request(url, headers={"User-Agent": "pigzpp-bench/1.0"})
    resp = urllib.request.urlopen(req, timeout=120)
    total = int(resp.headers.get("Content-Length", 0))
    chunks: list[bytes] = []
    downloaded = 0
    while True:
        chunk = resp.read(1 << 20)  # 1 MB
        if not chunk:
            break
        chunks.append(chunk)
        downloaded += len(chunk)
        if total:
            pct = downloaded * 100 // total
            print(f"\r  {downloaded / (1 << 20):.0f}/{total / (1 << 20):.0f} MB ({pct}%)",
                  end="", flush=True)
    print()
    return b"".join(chunks)


def _extract_text(parquet_bytes: bytes, text_col: str) -> list[str]:
    """Read *text_col* from an in-memory parquet file."""
    import pyarrow.parquet as pq

    table = pq.read_table(io.BytesIO(parquet_bytes), columns=[text_col])
    return [
        s for s in table.column(text_col).to_pylist()
        if isinstance(s, str) and s.strip()
    ]


def fetch_multilingual_seed(cache_dir: Path) -> bytes:
    """Download English + Chinese text and cache as a single UTF-8 seed file."""
    seed_path = cache_dir / SEED_FILENAME
    if seed_path.is_file() and seed_path.stat().st_size > 0:
        size_mb = seed_path.stat().st_size / (1 << 20)
        print(f"Using cached multilingual seed: {seed_path} ({size_mb:.1f} MB)")
        return seed_path.read_bytes()

    lines: list[str] = []
    for src in SOURCES:
        print(f"Downloading {src['label']} ...")
        raw = _download(src["url"])
        texts = _extract_text(raw, src["text_col"])
        lines.extend(texts)
        print(f"  → {len(texts)} text fragments")

    seed = "\n".join(lines).encode("utf-8")
    seed_path.write_bytes(seed)
    size_mb = len(seed) / (1 << 20)
    print(f"Cached multilingual seed: {seed_path} ({size_mb:.1f} MB, "
          f"{len(lines)} fragments)")
    return seed


def gen_text(path: Path, size_mb: int, seed: bytes) -> None:
    """Write multilingual text data, repeating the seed to reach *size_mb*."""
    target = size_mb * 1024 * 1024
    with open(path, "wb") as f:
        written = 0
        while written < target:
            chunk = seed[: target - written]
            f.write(chunk)
            written += len(chunk)


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

    # Fetch (or load cached) multilingual seed before generating text files
    seed = fetch_multilingual_seed(args.data_dir)

    for size in args.sizes:
        for ext, label, gen_fn in [
            (".txt", "text", lambda p, s: gen_text(p, s, seed)),
            (".bin", "random", gen_random),
        ]:
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
