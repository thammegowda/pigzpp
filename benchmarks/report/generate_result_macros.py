#!/usr/bin/env python3
"""Generate LaTeX result macros from an archived report benchmark JSON file."""
from __future__ import annotations

import argparse
import json
from pathlib import Path

REPO = Path(__file__).resolve().parents[2]


def method(records, suite, name):
    return next(r for r in records if r["suite"] == suite and r["method"] == name)


def fmt(x, digits=0):
    return f"{x:.{digits}f}"


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("json", type=Path)
    parser.add_argument("--out", type=Path, default=REPO / "report" / "generated-results.tex")
    args = parser.parse_args()

    payload = json.loads(args.json.read_text())
    rows = payload["records"]
    cli_gzip = method(rows, "cli", "gzip")
    cli_pigz = method(rows, "cli", "pigz")
    cli_zlib = method(rows, "cli", "pigzpp zlib")
    cli_isal = method(rows, "cli", "pigzpp isal")
    py_gzip = method(rows, "python", "gzip (stdlib)")
    py_isal_competitor = method(rows, "python", "python-isal")
    py_zlib = method(rows, "python", "pigzpp zlib")
    py_isal = method(rows, "python", "pigzpp isal")
    py_dec_gzip = method(rows, "python_decompression", "gzip (stdlib)")
    py_dec_zlib = method(rows, "python_decompression", "zlib-ng")
    py_dec_isal = method(rows, "python_decompression", "python-isal")
    py_dec_pigzpp = method(rows, "python_decompression", "pigzpp")
    go_zlib = method(rows, "go", "pigzpp zlib")
    go_isal = method(rows, "go", "pigzpp isal")
    rust_zlib = method(rows, "rust", "pigzpp zlib")
    rust_isal = method(rows, "rust", "pigzpp isal")
    docker_std = method(rows, "docker", "compress/gzip")
    docker_pgzip = method(rows, "docker", "klauspost/pgzip")
    docker_zlib = method(rows, "docker", "pigzpp zlib")
    docker_isal = method(rows, "docker", "pigzpp isal")
    wasm_single = method(rows, "wasm_single", "pigzpp-wasm")
    wasm_8 = next(r for r in rows if r["suite"] == "wasm_scaling" and r["workers"] == 8)
    wasm_1 = next(r for r in rows if r["suite"] == "wasm_scaling" and r["workers"] == 1)
    png_fast = method(rows, "png", "pigzpp fast")
    png_pillow = method(rows, "png", "pillow default")
    png_cv = method(rows, "png", "cv2 default")
    zip_std = method(rows, "zip", "zipfile (stdlib)")
    zip_zlib = method(rows, "zip", "pigzpp zlib")
    zip_isal = method(rows, "zip", "pigzpp isal")

    values = {
        "ResultDate": payload["metadata"]["created_utc"][:10],
        "ResultCommit": payload["metadata"]["commit"][:12],
        "CliPigzMBps": fmt(cli_pigz["throughput_mbps"]),
        "CliZlibMBps": fmt(cli_zlib["throughput_mbps"]),
        "CliIsalMBps": fmt(cli_isal["throughput_mbps"]),
        "CliZlibVsPigz": fmt(cli_zlib["throughput_mbps"] / cli_pigz["throughput_mbps"], 1),
        "CliIsalVsPigz": fmt(cli_isal["throughput_mbps"] / cli_pigz["throughput_mbps"], 1),
        "CliZlibRatio": fmt(cli_zlib["ratio"], 2),
        "CliIsalRatio": fmt(cli_isal["ratio"], 2),
        "CliIsalSizePenalty": fmt((cli_zlib["ratio"] / cli_isal["ratio"] - 1) * 100),
        "PythonGzipMBps": fmt(py_gzip["throughput_mbps"]),
        "PythonIsalCompetitorMBps": fmt(py_isal_competitor["throughput_mbps"]),
        "PythonZlibMBps": fmt(py_zlib["throughput_mbps"]),
        "PythonIsalMBps": fmt(py_isal["throughput_mbps"]),
        "PythonZlibVsGzip": fmt(py_zlib["throughput_mbps"] / py_gzip["throughput_mbps"]),
        "PythonIsalVsGzip": fmt(py_isal["throughput_mbps"] / py_gzip["throughput_mbps"]),
        "PythonDecompGzipMBps": fmt(py_dec_gzip["throughput_mbps"]),
        "PythonDecompZlibMBps": fmt(py_dec_zlib["throughput_mbps"]),
        "PythonDecompIsalMBps": fmt(py_dec_isal["throughput_mbps"]),
        "PythonDecompPigzppMBps": fmt(py_dec_pigzpp["throughput_mbps"]),
        "GoZlibMBps": fmt(go_zlib["throughput_mbps"]),
        "GoIsalMBps": fmt(go_isal["throughput_mbps"]),
        "RustZlibMBps": fmt(rust_zlib["throughput_mbps"]),
        "RustIsalMBps": fmt(rust_isal["throughput_mbps"]),
        "DockerStdlibMBps": fmt(docker_std["throughput_mbps"]),
        "DockerPgzipMBps": fmt(docker_pgzip["throughput_mbps"]),
        "DockerZlibMBps": fmt(docker_zlib["throughput_mbps"]),
        "DockerIsalMBps": fmt(docker_isal["throughput_mbps"]),
        "DockerZlibVsStdlib": fmt(docker_zlib["throughput_mbps"] / docker_std["throughput_mbps"]),
        "DockerIsalVsStdlib": fmt(docker_isal["throughput_mbps"] / docker_std["throughput_mbps"]),
        "DockerZlibVsPgzip": fmt(docker_zlib["throughput_mbps"] / docker_pgzip["throughput_mbps"], 2),
        "DockerStdlibSeconds": fmt(docker_std["median_seconds"], 1),
        "DockerIsalSeconds": fmt(docker_isal["median_seconds"], 1),
        "WasmSingleMBps": fmt(wasm_single["throughput_mbps"]),
        "WasmEightMBps": fmt(wasm_8["throughput_mbps"]),
        "WasmEightSpeedup": fmt(wasm_8["throughput_mbps"] / wasm_1["throughput_mbps"], 1),
        "PngFastImages": fmt(png_fast["throughput_images_per_s"]),
        "PngVsPillow": fmt(png_fast["throughput_images_per_s"] / png_pillow["throughput_images_per_s"], 1),
        "PngVsOpenCV": fmt(png_fast["throughput_images_per_s"] / png_cv["throughput_images_per_s"], 1),
        "ZipZlibVsStdlib": fmt(zip_zlib["write_mbps"] / zip_std["write_mbps"]),
        "ZipIsalVsStdlib": fmt(zip_isal["write_mbps"] / zip_std["write_mbps"]),
    }
    lines = ["% Generated from raw benchmark JSON; do not edit by hand."]
    lines.extend(f"\\newcommand{{\\{name}}}{{{value}}}" for name, value in values.items())
    args.out.parent.mkdir(parents=True, exist_ok=True)
    args.out.write_text("\n".join(lines) + "\n")
    print(f"wrote {args.out}")


if __name__ == "__main__":
    main()
