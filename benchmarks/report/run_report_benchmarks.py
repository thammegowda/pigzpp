#!/usr/bin/env python3
"""Run the benchmark set used by the pigzpp technical report.

This runner provides one reproducible protocol around the existing language-
specific harnesses:

* one untimed warm-up;
* seven independent timed samples;
* median throughput as the primary statistic;
* every sample plus command/environment/corpus metadata archived as JSON;
* report TSV files generated from that JSON, never copied by hand.

The native CLI and Python APIs are timed directly. Go, Rust, WASM, PNG, and ZIP
are invoked through their repository harnesses once per sample; each harness
reports one internally timed sample, and this runner takes the median across
seven independent invocations.
"""
from __future__ import annotations

import argparse
import csv
import datetime as dt
import gzip
import hashlib
import json
import os
import platform
import re
import shutil
import statistics
import subprocess
import sys
import tempfile
import time
from pathlib import Path
from typing import Any, Callable

REPO = Path(__file__).resolve().parents[2]
RESULT_DATE = dt.datetime.now(dt.timezone.utc).date().isoformat()
DEFAULT_RESULTS = REPO / "benchmarks" / "results" / RESULT_DATE / "report-benchmarks.json"
DEFAULT_PLOT_DATA = REPO / "report" / "plots" / "data"

sys.path.insert(0, str(REPO / "benchmarks" / "python"))
sys.path.insert(0, str(REPO / "benchmarks" / "core"))

MiB = 1024 * 1024


def run(cmd: list[str], *, cwd: Path | None = None, check: bool = True,
        capture: bool = True) -> subprocess.CompletedProcess[str]:
    return subprocess.run(
        [str(x) for x in cmd], cwd=cwd or REPO, check=check,
        text=True, capture_output=capture,
    )


def git_info() -> dict[str, Any]:
    sha = run(["git", "rev-parse", "HEAD"]).stdout.strip()
    dirty = bool(run(["git", "status", "--porcelain"]).stdout.strip())
    return {"commit": sha, "dirty": dirty}


def sha256(path: Path) -> str:
    h = hashlib.sha256()
    with path.open("rb") as inp:
        for chunk in iter(lambda: inp.read(4 * MiB), b""):
            h.update(chunk)
    return h.hexdigest()


def version(cmd: list[str]) -> str:
    try:
        cp = run(cmd, check=False)
        return (cp.stdout + cp.stderr).strip().splitlines()[0]
    except Exception as exc:  # pragma: no cover - metadata fallback
        return f"unavailable: {exc}"


def find_go() -> Path:
    candidates = [
        REPO / "tmp" / "go" / "bin" / "go",
        Path(shutil.which("go") or "go"),
    ]
    return next((p for p in candidates if p.exists()), candidates[-1])


def metadata(corpora: list[Path], invocation: str, samples: int) -> dict[str, Any]:
    lscpu = run(["lscpu"], check=False).stdout
    mem = run(["sh", "-c", "grep MemTotal /proc/meminfo"], check=False).stdout.strip()
    return {
        "schema_version": 1,
        "created_utc": dt.datetime.now(dt.timezone.utc).isoformat(),
        "invocation": invocation,
        "cwd": str(REPO),
        **git_info(),
        "platform": platform.platform(),
        "python": sys.version.splitlines()[0],
        "kernel": platform.release(),
        "lscpu": lscpu,
        "memory": mem,
        "visible_logical_cpus": os.cpu_count(),
        "timing_policy": {
            "warmups": 1,
            "timed_samples": samples,
            "primary": "median",
            "affinity_controlled": False,
            "turbo_controlled": False,
            "frequency_controlled": False,
        },
        "versions": {
            "gzip": version(["gzip", "--version"]),
            "pigz": version(["pigz", "--version"]),
            "pigzpp": version([str(REPO / "build" / "pigzpp"), "--version"]),
            "igzip": version(["igzip", "--version"]),
            "node": version([str(find_node()), "--version"]),
            "go": version([str(find_go()), "version"]),
            "rustc": version(["rustc", "--version"]),
        },
        "corpora": [describe_corpus(p) for p in corpora if p.exists()],
    }


def describe_corpus(path: Path) -> dict[str, Any]:
    try:
        label = str(path.resolve().relative_to(REPO.resolve()))
    except ValueError:
        label = str(path.resolve())
    return {"path": label, "bytes": path.stat().st_size, "sha256": sha256(path)}


def find_node() -> Path:
    candidates = [
        Path.home() / "emsdk" / "node" / "22.16.0_64bit" / "bin" / "node",
        Path(shutil.which("node") or "node"),
    ]
    return next((p for p in candidates if p.exists()), candidates[-1])


def summarize_seconds(samples: list[float], input_bytes: int) -> dict[str, Any]:
    ordered = sorted(samples)
    med = statistics.median(ordered)
    return {
        "samples_seconds": samples,
        "median_seconds": med,
        "min_seconds": min(samples),
        "max_seconds": max(samples),
        "throughput_mbps": (input_bytes / 1e6) / med,
    }


def summarize_mbps(samples: list[float], input_bytes: int) -> dict[str, Any]:
    med_mbps = statistics.median(samples)
    seconds = [(input_bytes / 1e6) / x for x in samples]
    return {
        "samples_mbps": samples,
        "samples_seconds": seconds,
        "median_seconds": statistics.median(seconds),
        "min_mbps": min(samples),
        "max_mbps": max(samples),
        "throughput_mbps": med_mbps,
    }


def time_callable(fn: Callable[[], bytes], samples: int) -> tuple[dict[str, Any], bytes]:
    fn()  # warm-up
    timings: list[float] = []
    out = b""
    for _ in range(samples):
        start = time.perf_counter()
        out = fn()
        timings.append(time.perf_counter() - start)
    return summarize_seconds(timings, 0), out


def time_command(cmd: list[str], input_bytes: int, samples: int) -> dict[str, Any]:
    subprocess.run(cmd, cwd=REPO, check=True, stdout=subprocess.DEVNULL,
                   stderr=subprocess.DEVNULL)  # warm-up
    times: list[float] = []
    for _ in range(samples):
        start = time.perf_counter()
        subprocess.run(cmd, cwd=REPO, check=True, stdout=subprocess.DEVNULL,
                       stderr=subprocess.DEVNULL)
        times.append(time.perf_counter() - start)
    return summarize_seconds(times, input_bytes)


def compressed_size(cmd: list[str]) -> int:
    with tempfile.NamedTemporaryFile() as out:
        subprocess.run(cmd, cwd=REPO, check=True, stdout=out,
                       stderr=subprocess.DEVNULL)
        out.flush()
        return os.fstat(out.fileno()).st_size


def record(*, suite: str, method: str, operation: str, corpus: str,
           input_bytes: int, level: int | None, workers: int | None,
           timing: dict[str, Any], output_bytes: int | None = None,
           extra: dict[str, Any] | None = None) -> dict[str, Any]:
    r: dict[str, Any] = {
        "suite": suite,
        "method": method,
        "operation": operation,
        "corpus": corpus,
        "input_bytes": input_bytes,
        "level": level,
        "workers": workers,
        **timing,
    }
    if output_bytes:
        r["output_bytes"] = output_bytes
        r["ratio"] = input_bytes / output_bytes
    if extra:
        r.update(extra)
    return r


def cli_cmd(method: str, corpus: Path, level: int, workers: int) -> list[str]:
    if method == "gzip":
        return ["gzip", "-n", f"-{level}", "-c", str(corpus)]
    if method == "pigz":
        return ["pigz", "-n", f"-{level}", "-p", str(workers), "-c", str(corpus)]
    if method.startswith("pigzpp "):
        engine = method.split()[1]
        return [str(REPO / "build" / "pigzpp"), "-n", f"-{level}", "-p",
                str(workers), "-E", engine, "-c", str(corpus)]
    raise ValueError(method)


def bench_cli(corpus: Path, samples: int, records: list[dict[str, Any]]) -> None:
    n = corpus.stat().st_size
    methods = ["gzip", "pigz", "pigzpp zlib", "pigzpp isal"]
    for method in methods:
        cmd = cli_cmd(method, corpus, 6, 8)
        timing = time_command(cmd, n, samples)
        out_size = compressed_size(cmd)
        records.append(record(suite="cli", method=method, operation="compress",
                              corpus=corpus.name, input_bytes=n, level=6, workers=8,
                              timing=timing, output_bytes=out_size,
                              extra={"command": cmd}))


def bench_decompression(corpus: Path, samples: int,
                        records: list[dict[str, Any]]) -> None:
    n = corpus.stat().st_size
    with tempfile.NamedTemporaryFile(suffix=".gz") as gz:
        subprocess.run(["gzip", "-n", "-6", "-c", str(corpus)], cwd=REPO,
                       check=True, stdout=gz)
        gz.flush()
        methods: list[tuple[str, list[str]]] = [
            ("gzip", ["gzip", "-dc", gz.name]),
            ("pigz", ["pigz", "-dc", gz.name]),
            ("pigzpp", [str(REPO / "build" / "pigzpp"), "-dc", gz.name]),
        ]
        if shutil.which("igzip"):
            methods.append(("igzip", ["igzip", "-d", "-c", gz.name]))
        for method, cmd in methods:
            timing = time_command(cmd, n, samples)
            records.append(record(suite="decompression", method=method,
                                  operation="decompress", corpus=corpus.name,
                                  input_bytes=n, level=6, workers=None,
                                  timing=timing, extra={"command": cmd,
                                  "source_stream": "gzip -n -6"}))


def bench_native_scaling(corpus: Path, samples: int,
                         records: list[dict[str, Any]]) -> None:
    n = corpus.stat().st_size
    for workers in [1, 2, 4, 5, 8, 10]:
        for method in ["pigz", "pigzpp zlib", "pigzpp isal"]:
            cmd = cli_cmd(method, corpus, 6, workers)
            timing = time_command(cmd, n, samples)
            records.append(record(suite="native_scaling", method=method,
                                  operation="compress", corpus=corpus.name,
                                  input_bytes=n, level=6, workers=workers,
                                  timing=timing, extra={"command": cmd}))


def bench_robustness(text: Path, random: Path, samples: int,
                     records: list[dict[str, Any]]) -> None:
    for corpus in [text, random]:
        n = corpus.stat().st_size
        for level in [1, 6, 9]:
            for method in ["pigz", "pigzpp zlib", "pigzpp isal"]:
                cmd = cli_cmd(method, corpus, level, 8)
                timing = time_command(cmd, n, samples)
                out_size = compressed_size(cmd)
                records.append(record(suite="robustness", method=method,
                                      operation="compress", corpus=corpus.name,
                                      input_bytes=n, level=level, workers=8,
                                      timing=timing, output_bytes=out_size,
                                      extra={"command": cmd}))


def bench_python(corpus: Path, samples: int,
                 records: list[dict[str, Any]]) -> None:
    import pigzpp
    from isal import igzip
    from zlib_ng import gzip_ng

    raw = corpus.read_bytes()
    n = len(raw)
    methods: dict[str, Callable[[], bytes]] = {
        "gzip (stdlib)": lambda: gzip.compress(raw, compresslevel=6, mtime=0),
        "zlib-ng": lambda: gzip_ng.compress(raw, compresslevel=6, mtime=0),
        "python-isal": lambda: igzip.compress(raw, compresslevel=3, mtime=0),
        "pigzpp zlib": lambda: pigzpp.compress(raw, level=6, engine="zlib", threads=8),
        "pigzpp isal": lambda: pigzpp.compress(raw, level=6, engine="isal", threads=8),
    }
    for method, fn in methods.items():
        fn()
        times: list[float] = []
        out = b""
        for _ in range(samples):
            start = time.perf_counter(); out = fn(); times.append(time.perf_counter() - start)
        records.append(record(suite="python", method=method, operation="compress",
                              corpus=corpus.name, input_bytes=n, level=6, workers=8,
                              timing=summarize_seconds(times, n), output_bytes=len(out)))

    source = gzip.compress(raw, compresslevel=6, mtime=0)
    decoders: dict[str, Callable[[], bytes]] = {
        "gzip (stdlib)": lambda: gzip.decompress(source),
        "zlib-ng": lambda: gzip_ng.decompress(source),
        "python-isal": lambda: igzip.decompress(source),
        "pigzpp": lambda: pigzpp.decompress(source),
    }
    for method, fn in decoders.items():
        fn(); times = []
        for _ in range(samples):
            start = time.perf_counter(); out = fn(); times.append(time.perf_counter() - start)
        if out != raw:
            raise RuntimeError(f"Python decompression mismatch: {method}")
        records.append(record(suite="python_decompression", method=method,
                              operation="decompress", corpus=corpus.name,
                              input_bytes=n, level=6, workers=None,
                              timing=summarize_seconds(times, n),
                              extra={"source_stream": "Python stdlib gzip level 6"}))


def parse_go(stdout: str) -> dict[str, dict[str, float]]:
    rows: dict[str, dict[str, float]] = {}
    pattern = re.compile(r"^(\S+)\s+\S+\s+([\d.]+) MB/s\s+([\d.]+)\s+([\d.]+)x")
    for line in stdout.splitlines():
        m = pattern.match(line.strip())
        if m:
            rows[m.group(1)] = {"mbps": float(m.group(2)),
                                "out_mib": float(m.group(3)),
                                "ratio": float(m.group(4))}
    return rows


def external_samples(cmd: list[str], parser: Callable[[str], dict[str, dict[str, float]]],
                     samples: int) -> tuple[dict[str, list[dict[str, float]]], list[str]]:
    run(cmd)  # independent warm-up invocation
    collected: dict[str, list[dict[str, float]]] = {}
    raw_outputs: list[str] = []
    for _ in range(samples):
        out = run(cmd).stdout
        raw_outputs.append(out)
        for name, values in parser(out).items():
            collected.setdefault(name, []).append(values)
    return collected, raw_outputs


def append_external_records(*, suite: str, corpus: Path, level: int,
                            workers: int | None, collected: dict[str, list[dict[str, float]]],
                            mapping: dict[str, str], records: list[dict[str, Any]],
                            command: list[str], raw_outputs: list[str]) -> None:
    n = corpus.stat().st_size
    for source_name, values in collected.items():
        if source_name not in mapping or len(values) == 0:
            continue
        samples_mbps = [v["mbps"] for v in values]
        ratio = statistics.median(v.get("ratio", 0) for v in values)
        output_bytes = round(n / ratio) if ratio else None
        records.append(record(
            suite=suite, method=mapping[source_name], operation="compress",
            corpus=corpus.name, input_bytes=n, level=level, workers=workers,
            timing=summarize_mbps(samples_mbps, n), output_bytes=output_bytes,
            extra={"command": command, "raw_outputs": raw_outputs},
        ))


def bench_go(corpus: Path, samples: int, records: list[dict[str, Any]],
             suite: str = "go") -> None:
    binary = REPO / "benchmarks" / "go-docker" / "dockergzbench"
    methods = "stdlib,kp,pgzip,pigzpp,pigzppcgo-zlib,pigzppcgo-isal" if suite == "go" \
        else "stdlib,kp,pgzip,pigzppcgo-zlib-owned,pigzppcgo-isal-owned"
    mapping = {
        "stdlib": "compress/gzip", "kp": "klauspost/compress",
        "pgzip": "klauspost/pgzip", "pigzpp": "pigzpp (exec)",
        "pigzppcgo-zlib": "pigzpp zlib", "pigzppcgo-isal": "pigzpp isal",
        "pigzppcgo-zlib-owned": "pigzpp zlib",
        "pigzppcgo-isal-owned": "pigzpp isal",
    }
    with tempfile.NamedTemporaryFile(suffix=".json") as json_out:
        cmd = [str(binary), "-input", str(corpus), "-iters", str(samples),
               "-level", "6", "-threads", "8", "-pigzpp",
               str(REPO / "build" / "pigzpp"), "-methods", methods,
               "-json-out", json_out.name]
        stdout = run(cmd).stdout
        payload = json.load(json_out)
    n = corpus.stat().st_size
    for item in payload["results"]:
        if item["method"] not in mapping:
            continue
        seconds = [ms / 1000 for ms in item["samples_ms"]]
        records.append(record(
            suite=suite, method=mapping[item["method"]], operation="compress",
            corpus=corpus.name, input_bytes=n, level=6, workers=8,
            timing=summarize_seconds(seconds, n), output_bytes=item["output_bytes"],
            extra={"command": cmd, "raw_output": stdout,
                   "output_ownership": "C-owned; consumed before release"
                       if item["method"].endswith("-owned") else "Go-owned copy"},
        ))


def parse_rust(stdout: str) -> dict[str, dict[str, float]]:
    rows: dict[str, dict[str, float]] = {}
    pattern = re.compile(r"^(\S+)\s+([\d.]+)\s+([\d.]+)\s+([\d.]+)$")
    for line in stdout.splitlines():
        m = pattern.match(line.strip())
        if m and m.group(1) != "method":
            rows[m.group(1)] = {"mbps": float(m.group(2)),
                                "ratio": float(m.group(3)),
                                "out_mib": float(m.group(4))}
    return rows


def bench_rust(corpus: Path, samples: int, records: list[dict[str, Any]]) -> None:
    binary = REPO / "benchmarks" / "rust" / "target" / "release" / "pigzpp-bench"
    cmd = [str(binary), "--input", str(corpus), "--iters", "1", "--level", "6",
           "--threads", "8"]
    collected, raw = external_samples(cmd, parse_rust, samples)
    mapping = {
        "flate2": "flate2", "libdeflater": "libdeflater", "gzp": "gzp",
        "pigzpp:zlib": "pigzpp zlib", "pigzpp:isal": "pigzpp isal",
    }
    append_external_records(suite="rust", corpus=corpus, level=6, workers=8,
                            collected=collected, mapping=mapping, records=records,
                            command=cmd, raw_outputs=raw)


def parse_wasm(stdout: str) -> dict[str, dict[str, float]]:
    rows: dict[str, dict[str, float]] = {}
    pattern = re.compile(r"^\|\s*([^|]+?)\s*\|\s*([\d.]+)\s*\|\s*([\d.]+)\s*\|")
    for line in stdout.splitlines():
        m = pattern.match(line.strip())
        if m and m.group(1).strip().lower() != "engine":
            rows[m.group(1).strip()] = {"mbps": float(m.group(2)),
                                        "ratio_output_over_input": float(m.group(3))}
    return rows


def bench_wasm(samples: int, records: list[dict[str, Any]], corpus16: Path,
               corpus128: Path) -> None:
    node = str(find_node())
    bench = REPO / "benchmarks" / "wasm" / "bench.mjs"
    module = REPO / "build-wasm-simd" / "wasm" / "pigzpp_wasm.mjs"
    cmd = [node, str(bench), "--module", str(module), "--size", "16", "--iters", "1"]
    collected, raw = external_samples(cmd, parse_wasm, samples)
    mapping = {
        "pigzpp-wasm L6": "pigzpp-wasm", "CompressionStream": "CompressionStream",
        "node-zlib L6": "node-zlib", "fflate L6": "fflate", "pako L6": "pako",
    }
    n = corpus16.stat().st_size
    for source_name, values in collected.items():
        if source_name not in mapping:
            continue
        samples_mbps = [v["mbps"] for v in values]
        out_over_in = statistics.median(v["ratio_output_over_input"] for v in values)
        records.append(record(
            suite="wasm_single", method=mapping[source_name], operation="compress",
            corpus=corpus16.name, input_bytes=n, level=6, workers=1,
            timing=summarize_mbps(samples_mbps, n),
            output_bytes=round(n * out_over_in),
            extra={"command": cmd, "raw_outputs": raw},
        ))

    scaling = REPO / "benchmarks" / "wasm" / "scaling.mjs"
    cmd2 = [node, str(scaling), "--size", "128", "--iters", "1", "--level", "6",
            "--threads", "1,2,4,5,8"]
    def parse_scaling(stdout: str) -> dict[str, dict[str, float]]:
        rows: dict[str, dict[str, float]] = {}
        pat = re.compile(r"^\|\s*(\d+)\s*\|\s*([\d.]+)\s*\|\s*([\d.]+)x")
        for line in stdout.splitlines():
            m = pat.match(line.strip())
            if m:
                rows[m.group(1)] = {"mbps": float(m.group(2)), "speedup": float(m.group(3))}
        return rows
    collected2, raw2 = external_samples(cmd2, parse_scaling, samples)
    n2 = corpus128.stat().st_size
    for workers, values in collected2.items():
        samples_mbps = [v["mbps"] for v in values]
        records.append(record(
            suite="wasm_scaling", method="pigzpp-wasm", operation="compress",
            corpus=corpus128.name, input_bytes=n2, level=6, workers=int(workers),
            timing=summarize_mbps(samples_mbps, n2),
            extra={"command": cmd2, "raw_outputs": raw2},
        ))


def parse_png_csv(path: Path) -> dict[str, dict[str, float]]:
    rows: dict[str, dict[str, float]] = {}
    with path.open(newline="") as inp:
        for row in csv.DictReader(inp):
            name = row["name"].replace("*", "")
            rows[name] = {"mbps": float(row["img_s"]),
                          "ratio": float(row["size_vs_pillow"]),
                          "avg_bytes": float(row["avg_bytes"])}
    return rows


def bench_png(samples: int, records: list[dict[str, Any]], image_dir: Path) -> None:
    script = REPO / "benchmarks" / "png" / "bench_png.py"
    runs: list[dict[str, dict[str, float]]] = []
    raw_commands: list[list[str]] = []
    for index in range(samples + 1):
        with tempfile.TemporaryDirectory() as tmp:
            cmd = [sys.executable, str(script), "--image-dir", str(image_dir),
                   "--limit", "24", "--loops", "1", "--mode", "rgb", "--verify",
                   "--out", tmp]
            run(cmd)
            if index > 0:
                runs.append(parse_png_csv(Path(tmp) / "png-bench.csv"))
                raw_commands.append(cmd)
    raw_bytes_per_image = 768 * 512 * 3
    names = sorted(set.intersection(*(set(x) for x in runs)))
    for name in names:
        imgs_per_s = [r[name]["mbps"] for r in runs]
        size_vs_pillow = statistics.median(r[name]["ratio"] for r in runs)
        avg_bytes = round(statistics.median(r[name]["avg_bytes"] for r in runs))
        records.append(record(
            suite="png", method=name.replace(".", " "), operation="compress",
            corpus="kodak-rgb-24", input_bytes=raw_bytes_per_image,
            level=None, workers=None,
            timing={"samples_images_per_s": imgs_per_s,
                    "throughput_images_per_s": statistics.median(imgs_per_s),
                    "min_images_per_s": min(imgs_per_s),
                    "max_images_per_s": max(imgs_per_s)},
            output_bytes=avg_bytes,
                 extra={"size_vs_pillow": size_vs_pillow, "image_count": 24,
                     "commands": raw_commands},
        ))


def parse_zip(stdout: str) -> dict[str, dict[str, float]]:
    rows: dict[str, dict[str, float]] = {}
    pattern = re.compile(r"^\|\s*([^|]+?)\s*\|\s*([\d.]+)\s*\|\s*([\d.]+)\s*\|\s*([\d.]+)\s*\|")
    for line in stdout.splitlines():
        m = pattern.match(line.strip())
        if m and m.group(1).strip().lower() != "writer":
            rows[m.group(1).strip()] = {"write_mbps": float(m.group(2)),
                                        "ratio": float(m.group(3)),
                                        "read_mbps": float(m.group(4))}
    return rows


def bench_zip(corpus: Path, samples: int, records: list[dict[str, Any]]) -> None:
    script = REPO / "benchmarks" / "python" / "bench_zip.py"
    cmd = [sys.executable, str(script), "--sizes", "128", "--members", "1",
           "--level", "6", "--threads", "8", "--engines", "isal,zlib",
           "--iterations", "1", "--data-dir", str(corpus.parent)]
    collected, raw = external_samples(cmd, parse_zip, samples)
    n = corpus.stat().st_size
    mapping = {
        "zipfile (stdlib)": "zipfile (stdlib)",
        "pigzpp:isal (t=8)": "pigzpp isal",
        "pigzpp:zlib (t=8)": "pigzpp zlib",
    }
    for source_name, values in collected.items():
        if source_name not in mapping:
            continue
        write_samples = [v["write_mbps"] for v in values]
        read_samples = [v["read_mbps"] for v in values]
        ratio = statistics.median(v["ratio"] for v in values)
        records.append(record(
            suite="zip", method=mapping[source_name], operation="write_read",
            corpus=corpus.name, input_bytes=n, level=6, workers=8,
            timing={"samples_write_mbps": write_samples,
                    "write_mbps": statistics.median(write_samples),
                    "samples_read_mbps": read_samples,
                    "read_mbps": statistics.median(read_samples)},
            output_bytes=round(n / ratio),
            extra={"command": cmd, "raw_outputs": raw},
        ))


def write_tsv(path: Path, header: list[str], rows: list[list[Any]]) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    with path.open("w", newline="") as out:
        writer = csv.writer(out, delimiter="\t", lineterminator="\n")
        writer.writerow(header)
        writer.writerows(rows)


def by_suite(records: list[dict[str, Any]], suite: str) -> list[dict[str, Any]]:
    return [r for r in records if r["suite"] == suite]


def generate_plot_data(records: list[dict[str, Any]], out: Path) -> None:
    cli = {r["method"]: r for r in by_suite(records, "cli")}
    if cli:
        write_tsv(out / "cli.tsv", ["tool", "mbps", "ratio"], [
            ["gzip", cli["gzip"]["throughput_mbps"], cli["gzip"]["ratio"]],
            ["pigz", cli["pigz"]["throughput_mbps"], cli["pigz"]["ratio"]],
            ["pigzpp (zlib)", cli["pigzpp zlib"]["throughput_mbps"], cli["pigzpp zlib"]["ratio"]],
            ["pigzpp (isal)", cli["pigzpp isal"]["throughput_mbps"], cli["pigzpp isal"]["ratio"]],
        ])

    for suite, filename in [("python", "language_python.tsv"),
                            ("go", "language_go.tsv"),
                            ("rust", "language_rust.tsv")]:
        rows = by_suite(records, suite)
        if rows:
            write_tsv(out / filename, ["method", "mbps", "ratio"], [
                [r["method"], r["throughput_mbps"], r.get("ratio", "")] for r in rows
            ])

    go = by_suite(records, "docker")
    if go:
        slow = next(r["throughput_mbps"] for r in go if r["method"] == "compress/gzip")
        write_tsv(out / "docker.tsv", ["method", "mbps", "ratio", "speedup"], [
            [r["method"], r["throughput_mbps"], r.get("ratio", ""), r["throughput_mbps"] / slow]
            for r in go if r["method"] != "pigzpp (exec)"
        ])

    wasm_single = by_suite(records, "wasm_single")
    if wasm_single:
        write_tsv(out / "wasm_single.tsv", ["engine", "mbps", "ratio"], [
            [r["method"], r["throughput_mbps"], r.get("ratio", "")]
            for r in wasm_single
        ])
    scale = sorted(by_suite(records, "wasm_scaling"), key=lambda r: r["workers"])
    if scale:
        base = scale[0]["throughput_mbps"]
        write_tsv(out / "wasm_scaling.tsv", ["threads", "mbps", "speedup"], [
            [r["workers"], r["throughput_mbps"], r["throughput_mbps"] / base] for r in scale
        ])

    png = by_suite(records, "png")
    if png:
        pillow = next(r["throughput_images_per_s"] for r in png if r["method"] == "pillow default")
        write_tsv(out / "png.tsv", ["encoder", "imgs_per_s", "vs_pillow", "size"], [
            [r["method"], r["throughput_images_per_s"], r["throughput_images_per_s"] / pillow,
             r.get("size_vs_pillow", "")] for r in png
        ])

    zip_rows = by_suite(records, "zip")
    if zip_rows:
        write_tsv(out / "zip.tsv", ["writer", "write_mbps", "read_mbps", "ratio"], [
            [r["method"], r["write_mbps"], r["read_mbps"], r.get("ratio", "")]
            for r in zip_rows
        ])

    decomp = by_suite(records, "decompression") + by_suite(records, "python_decompression")
    if decomp:
        write_tsv(out / "decompression.tsv", ["scope", "method", "mbps"], [
            ["CLI" if r["suite"] == "decompression" else "Python", r["method"], r["throughput_mbps"]]
            for r in decomp
        ])

    scaling = by_suite(records, "native_scaling")
    baselines: dict[str, float] = {}
    for r in scaling:
        if r["workers"] == 1:
            baselines[r["method"]] = r["throughput_mbps"]
    if scaling:
        write_tsv(out / "native_scaling.tsv", ["method", "workers", "mbps", "speedup", "efficiency"], [
            [r["method"], r["workers"], r["throughput_mbps"],
             r["throughput_mbps"] / baselines[r["method"]],
             (r["throughput_mbps"] / baselines[r["method"]]) / r["workers"]]
            for r in scaling
        ])

    robust = by_suite(records, "robustness")
    if robust:
        write_tsv(out / "robustness.tsv", ["corpus", "level", "method", "mbps", "ratio"], [
            [r["corpus"].replace("128MB.", ""), r["level"], r["method"],
             r["throughput_mbps"], r.get("ratio", "")] for r in robust
        ])


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--results", type=Path, default=DEFAULT_RESULTS)
    parser.add_argument("--plot-data", type=Path, default=DEFAULT_PLOT_DATA)
    parser.add_argument("--samples", type=int, default=7)
    parser.add_argument("--image-dir", type=Path, default=Path("/tmp/kodak"))
    parser.add_argument("--docker-layer", type=Path,
                        default=REPO / "build" / "docker_bench" / "layer.tar")
    parser.add_argument("--skip-docker", action="store_true")
    parser.add_argument("--only", choices=["all", "current", "python", "go",
                                            "docker", "native", "robustness"],
                        default="all")
    parser.add_argument("--update-existing", action="store_true",
                        help="replace the selected suites in an existing result file")
    args = parser.parse_args()

    text128 = REPO / "build" / "bench_data" / "128MB.txt"
    random128 = REPO / "build" / "bench_data" / "128MB.bin"
    text16 = REPO / "build" / "bench_data" / "16MB.txt"
    for path in [text128, random128, text16]:
        if not path.exists():
            raise SystemExit(f"missing corpus: {path}")

    replaced_suites = {
        "python": {"python", "python_decompression"},
        "go": {"go"},
        "docker": {"docker"},
        "native": {"native_scaling"},
        "robustness": {"robustness"},
    }.get(args.only, set())
    previous: dict[str, Any] | None = None
    records: list[dict[str, Any]] = []
    if args.update_existing:
        if args.only in ("all", "current"):
            raise SystemExit("--update-existing requires one selected suite")
        if not args.results.exists():
            raise SystemExit(f"cannot update missing result file: {args.results}")
        previous = json.loads(args.results.read_text())
        records = [r for r in previous["records"]
                   if r["suite"] not in replaced_suites]

    if args.only in ("all", "current"):
        print("[1/8] CLI headline + decompression", flush=True)
        bench_cli(text128, args.samples, records)
        bench_decompression(text128, args.samples, records)
        print("[2/8] Python compression + decompression", flush=True)
        bench_python(text128, args.samples, records)
        print("[3/8] Go language panel", flush=True)
        bench_go(text128, args.samples, records, "go")
        if not args.skip_docker:
            print("[4/8] Docker layer", flush=True)
            if not args.docker_layer.exists():
                raise SystemExit(f"missing Docker layer: {args.docker_layer}")
            bench_go(args.docker_layer, args.samples, records, "docker")
        print("[5/8] Rust language panel", flush=True)
        bench_rust(text128, args.samples, records)
        print("[6/8] WASM single + scaling", flush=True)
        bench_wasm(args.samples, records, text16, text128)
        print("[7/8] Kodak PNG", flush=True)
        if not args.image_dir.exists():
            raise SystemExit(f"missing Kodak image directory: {args.image_dir}")
        bench_png(args.samples, records, args.image_dir)
        print("[8/8] Python ZIP", flush=True)
        bench_zip(text128, args.samples, records)
    if args.only == "python":
        print("[python] compression + decompression", flush=True)
        bench_python(text128, args.samples, records)
    if args.only == "go":
        print("[go] language panel", flush=True)
        bench_go(text128, args.samples, records, "go")
    if args.only == "docker":
        print("[docker] layer compression", flush=True)
        if not args.docker_layer.exists():
            raise SystemExit(f"missing Docker layer: {args.docker_layer}")
        bench_go(args.docker_layer, args.samples, records, "docker")
    if args.only in ("all", "native"):
        print("[native] thread scaling", flush=True)
        bench_native_scaling(text128, args.samples, records)
    if args.only in ("all", "robustness"):
        print("[robustness] levels × text/random", flush=True)
        bench_robustness(text128, random128, args.samples, records)

    corpora = [text16, text128, random128]
    if args.docker_layer.exists() and not args.skip_docker:
        corpora.append(args.docker_layer)
    if args.image_dir.exists():
        corpora.extend(sorted(args.image_dir.glob("*.png")))
    run_metadata = metadata(corpora, " ".join(sys.argv), args.samples)
    if previous is None:
        payload = {"metadata": run_metadata, "records": records}
    else:
        payload = previous
        payload["records"] = records
        payload.setdefault("updates", []).append({
            "suites": sorted(replaced_suites),
            **run_metadata,
        })
    args.results.parent.mkdir(parents=True, exist_ok=True)
    args.results.write_text(json.dumps(payload, indent=2) + "\n")
    generate_plot_data(records, args.plot_data)
    print(f"wrote {args.results}")
    print(f"updated {args.plot_data}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
