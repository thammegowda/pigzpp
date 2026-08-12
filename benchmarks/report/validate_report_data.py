#!/usr/bin/env python3
"""Validate report plot TSVs against an archived raw benchmark JSON file."""
from __future__ import annotations

import argparse
import filecmp
import json
import sys
import tempfile
from collections import Counter
from pathlib import Path

HERE = Path(__file__).resolve().parent
REPO = HERE.parents[1]
sys.path.insert(0, str(HERE))
from run_report_benchmarks import generate_plot_data  # noqa: E402

REQUIRED = {
    "cli": {"gzip", "pigz", "pigzpp zlib", "pigzpp isal"},
    "python": {"gzip (stdlib)", "zlib-ng", "python-isal", "pigzpp zlib", "pigzpp isal"},
    "go": {"compress/gzip", "klauspost/compress", "klauspost/pgzip",
           "pigzpp (exec)", "pigzpp zlib", "pigzpp isal"},
    "docker": {"compress/gzip", "klauspost/compress", "klauspost/pgzip",
               "pigzpp zlib", "pigzpp isal"},
    "rust": {"flate2", "libdeflater", "gzp", "pigzpp zlib", "pigzpp isal"},
    "wasm_single": {"pako", "fflate", "CompressionStream", "node-zlib", "pigzpp-wasm"},
    "wasm_scaling": {"pigzpp-wasm"},
    "png": {"pigzpp fast", "pigzpp balanced", "pigzpp small",
            "pillow default", "cv2 default"},
    "zip": {"zipfile (stdlib)", "pigzpp zlib", "pigzpp isal"},
    "decompression": {"gzip", "pigz", "pigzpp"},
    "python_decompression": {"gzip (stdlib)", "zlib-ng", "python-isal", "pigzpp"},
    "native_scaling": {"pigz", "pigzpp zlib", "pigzpp isal"},
    "robustness": {"pigz", "pigzpp zlib", "pigzpp isal"},
}


def sample_count(record: dict) -> int:
    for key in ("samples_seconds", "samples_mbps", "samples_images_per_s",
                "samples_write_mbps"):
        if key in record:
            return len(record[key])
    return 0


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("json", type=Path)
    parser.add_argument("--plot-data", type=Path, default=REPO / "report" / "plots" / "data")
    parser.add_argument("--samples", type=int, default=7)
    args = parser.parse_args()

    payload = json.loads(args.json.read_text())
    records = payload.get("records", [])
    errors: list[str] = []

    actual: dict[str, set[str]] = {}
    for record in records:
        actual.setdefault(record["suite"], set()).add(record["method"])
        count = sample_count(record)
        if count != args.samples:
            errors.append(f"{record['suite']}/{record['method']}: {count} samples, expected {args.samples}")

    for suite, methods in REQUIRED.items():
        missing = methods - actual.get(suite, set())
        if missing:
            errors.append(f"{suite}: missing methods {sorted(missing)}")

    with tempfile.TemporaryDirectory() as tmp:
        generated = Path(tmp)
        generate_plot_data(records, generated)
        for expected in sorted(generated.glob("*.tsv")):
            checked_in = args.plot_data / expected.name
            if not checked_in.exists():
                errors.append(f"missing checked-in TSV: {checked_in}")
            elif not filecmp.cmp(expected, checked_in, shallow=False):
                errors.append(f"TSV differs from JSON-derived data: {checked_in}")

    if errors:
        print("report data validation FAILED:", file=sys.stderr)
        for error in errors:
            print(f"  - {error}", file=sys.stderr)
        return 1

    counts = Counter(r["suite"] for r in records)
    print(f"validated {len(records)} records from {args.json}")
    print("suites:", ", ".join(f"{k}={v}" for k, v in sorted(counts.items())))
    print(f"all records have {args.samples} timed samples; TSVs match raw JSON")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
