#!/usr/bin/env python3
"""Create machine-readable and Markdown size reports for distribution packages."""

from __future__ import annotations

import argparse
from collections import defaultdict
import json
from pathlib import Path
import sys


def category(name: str) -> str | None:
    if name.endswith(".whl"):
        return "Python wheel"
    if name.startswith("pigzpp-wasm-") and name.endswith(".tar.gz"):
        return "WebAssembly"
    if name.startswith("pigzpp-") and (
        name.endswith(".tar.gz") or name.endswith(".zip")
    ):
        return "Native CLI"
    return None


def human_size(size: int) -> str:
    value = float(size)
    units = ("B", "KiB", "MiB", "GiB", "TiB")
    for unit in units:
        if value < 1024 or unit == units[-1]:
            if unit == "B":
                return f"{size} B"
            return f"{value:.2f} {unit}"
        value /= 1024
    raise AssertionError("unreachable")


def collect(root: Path) -> list[dict[str, object]]:
    packages: list[dict[str, object]] = []
    basenames: set[str] = set()
    for path in sorted(item for item in root.rglob("*") if item.is_file()):
        package_category = category(path.name)
        if package_category is None:
            continue
        if path.name in basenames:
            raise ValueError(f"duplicate distribution package name: {path.name}")
        basenames.add(path.name)
        size = path.stat().st_size
        packages.append(
            {
                "name": path.name,
                "category": package_category,
                "size_bytes": size,
                "size_human": human_size(size),
            }
        )
    order = {"Native CLI": 0, "Python wheel": 1, "WebAssembly": 2}
    packages.sort(key=lambda package: (order[str(package["category"])], str(package["name"])))
    return packages


def markdown(packages: list[dict[str, object]]) -> str:
    lines = [
        "# Distribution package sizes",
        "",
        "Sizes use binary units (1 KiB = 1024 bytes).",
        "",
        "| Package | Type | Size | Bytes |",
        "|---|---:|---:|---:|",
    ]
    for package in packages:
        lines.append(
            f"| `{package['name']}` | {package['category']} | "
            f"{package['size_human']} | {int(package['size_bytes']):,} |"
        )

    grouped: dict[str, list[int]] = defaultdict(list)
    for package in packages:
        grouped[str(package["category"])].append(int(package["size_bytes"]))

    lines.extend(
        [
            "",
            "## Totals",
            "",
            "| Type | Packages | Total size | Bytes |",
            "|---|---:|---:|---:|",
        ]
    )
    for name in ("Native CLI", "Python wheel", "WebAssembly"):
        sizes = grouped.get(name, [])
        total = sum(sizes)
        lines.append(f"| {name} | {len(sizes)} | {human_size(total)} | {total:,} |")
    total = sum(int(package["size_bytes"]) for package in packages)
    lines.append(f"| **All distributions** | **{len(packages)}** | **{human_size(total)}** | **{total:,}** |")
    lines.append("")
    return "\n".join(lines)


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("root", type=Path, help="Directory containing downloaded artifacts")
    parser.add_argument("--markdown", type=Path, default=Path("DIST_SIZES.md"))
    parser.add_argument("--json", type=Path, default=Path("dist-sizes.json"))
    parser.add_argument("--expect", type=int, help="Fail unless this many packages are found")
    args = parser.parse_args()

    try:
        packages = collect(args.root)
    except ValueError as error:
        print(f"error: {error}", file=sys.stderr)
        return 1

    if args.expect is not None and len(packages) != args.expect:
        print(
            f"error: expected {args.expect} distribution packages, found {len(packages)}",
            file=sys.stderr,
        )
        return 1

    total = sum(int(package["size_bytes"]) for package in packages)
    report = {
        "package_count": len(packages),
        "total_size_bytes": total,
        "total_size_human": human_size(total),
        "packages": packages,
    }
    args.markdown.write_text(markdown(packages), encoding="utf-8")
    args.json.write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")
    print(markdown(packages), end="")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
