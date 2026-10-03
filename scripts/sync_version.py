#!/usr/bin/env python3
"""Synchronize package metadata that cannot read VERSION.txt directly."""

from __future__ import annotations

import argparse
from pathlib import Path
import re
import sys


ROOT = Path(__file__).resolve().parent.parent
VERSION_FILE = ROOT / "VERSION.txt"
VERSION_PATTERN = re.compile(r"^[0-9]+\.[0-9]+\.[0-9]+$")
TARGETS = {
    ROOT / "src/rust/Cargo.toml": re.compile(
        r'(?ms)(^\[package\]\n.*?^version = ")[^"]+(")'
    ),
    ROOT / "benchmarks/rust/Cargo.lock": re.compile(
        r'(?m)(^\[\[package\]\]\nname = "pigzpp"\nversion = ")[^"]+(")'
    ),
}


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument(
        "--check",
        action="store_true",
        help="report stale generated versions without modifying files",
    )
    args = parser.parse_args()

    version = VERSION_FILE.read_text(encoding="utf-8").strip()
    if not VERSION_PATTERN.fullmatch(version):
        print(
            f"error: {VERSION_FILE.name} must contain a MAJOR.MINOR.PATCH version",
            file=sys.stderr,
        )
        return 1

    stale: list[Path] = []
    for path, pattern in TARGETS.items():
        content = path.read_text(encoding="utf-8")
        updated, replacements = pattern.subn(rf"\g<1>{version}\g<2>", content, count=1)
        if replacements != 1:
            print(f"error: version field not found in {path.relative_to(ROOT)}", file=sys.stderr)
            return 1
        if updated == content:
            continue
        stale.append(path)
        if not args.check:
            path.write_text(updated, encoding="utf-8")
            print(f"updated {path.relative_to(ROOT)} to {version}")

    if args.check and stale:
        for path in stale:
            print(
                f"error: {path.relative_to(ROOT)} is not synchronized with {VERSION_FILE.name}",
                file=sys.stderr,
            )
        print("run: python3 scripts/sync_version.py", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
