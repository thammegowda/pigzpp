#!/usr/bin/env python3
"""Reject release artifacts that depend on dynamic zlib-ng or ISA-L.

Archives and wheels are unpacked automatically, and every native binary they
contain is inspected. The banned dependencies (zlib-ng, zlib, ISA-L) are always
rejected regardless of platform, since they are always embedded statically.

The optional flags encode the per-platform runtime policy:

* ``--fully-static`` (Linux CLI archives): the ELF binary must have no dynamic
  dependencies at all. Linux CLI archives are linked fully static.
* ``--static-cpp-runtime`` (Linux wheels): the ELF extension must not pull in a
  dynamic ``libstdc++``/``libgcc_s``; those are statically linked. ``libc``,
  ``libm``, and the loader remain dynamic by design.
* ``--static-windows-crt`` (Windows CLI/wheels): the PE binary must not import a
  dynamic MSVC/UCRT runtime; the static ``/MT`` runtime is used instead.

macOS artifacts only get the compression-library check: the platform does not
support fully static executables, so the system ``libc++``/``libSystem`` shipped
with every supported macOS release are permitted.
"""

from __future__ import annotations

import argparse
import glob
import os
from pathlib import Path
import re
import shutil
import subprocess
import sys
import tarfile
import tempfile
import zipfile

BANNED_COMPRESSION = re.compile(
    r"(?:^|[/\\])(?:libz(?:-ng)?(?:\.[^/\\]+)?|zlib(?:1|-ng[^/\\]*)?\.dll|"
    r"lib(?:isal|isa-l)(?:\.[^/\\]+)?|isa-l\.dll|isal\.dll)$",
    re.IGNORECASE,
)
BANNED_WINDOWS_RUNTIME = re.compile(
    r"^(?:vcruntime\d+(?:_\d+)?|msvcp\d+|ucrtbase)\.dll$", re.IGNORECASE
)
BANNED_ELF_CPP_RUNTIME = re.compile(
    r"^(?:libstdc\+\+|libgcc_s)\.so(?:\..*)?$", re.IGNORECASE
)
MACH_MAGICS = {
    b"\xfe\xed\xfa\xce",
    b"\xce\xfa\xed\xfe",
    b"\xfe\xed\xfa\xcf",
    b"\xcf\xfa\xed\xfe",
    b"\xca\xfe\xba\xbe",
    b"\xbe\xba\xfe\xca",
    b"\xca\xfe\xba\xbf",
    b"\xbf\xba\xfe\xca",
}


def run(*args: str) -> str:
    result = subprocess.run(args, check=True, text=True, capture_output=True)
    return result.stdout


def binary_kind(path: Path) -> str | None:
    try:
        with path.open("rb") as stream:
            magic = stream.read(4)
    except (OSError, IsADirectoryError):
        return None
    if magic == b"\x7fELF":
        return "elf"
    if magic[:2] == b"MZ":
        return "pe"
    if magic in MACH_MAGICS:
        return "mach"
    return None


def elf_dependencies(path: Path) -> list[str]:
    output = run("readelf", "-d", str(path))
    return re.findall(r"Shared library: \[([^]]+)]", output)


def mach_dependencies(path: Path) -> list[str]:
    output = run("otool", "-L", str(path))
    dependencies = []
    for line in output.splitlines()[1:]:
        line = line.strip()
        if line:
            dependencies.append(line.split(" (compatibility version", 1)[0])
    return dependencies


def pe_dependencies(path: Path) -> list[str]:
    try:
        import pefile
    except ImportError as error:
        raise RuntimeError(
            "pefile is required to inspect Windows artifacts: python -m pip install pefile"
        ) from error

    image = pefile.PE(str(path), fast_load=True)
    image.parse_data_directories(
        directories=[pefile.DIRECTORY_ENTRY["IMAGE_DIRECTORY_ENTRY_IMPORT"]]
    )
    return [entry.dll.decode("ascii", "replace") for entry in image.DIRECTORY_ENTRY_IMPORT]


def dependencies(path: Path, kind: str) -> list[str]:
    if kind == "elf":
        return elf_dependencies(path)
    if kind == "mach":
        return mach_dependencies(path)
    if kind == "pe":
        return pe_dependencies(path)
    raise AssertionError(kind)


def extract(path: Path, destination: Path) -> Path:
    if path.suffix.lower() in {".whl", ".zip"}:
        with zipfile.ZipFile(path) as archive:
            archive.extractall(destination)
        return destination
    if tarfile.is_tarfile(path):
        with tarfile.open(path) as archive:
            archive.extractall(destination, filter="data")
        return destination
    target = destination / path.name
    shutil.copy2(path, target)
    return destination


def inspect_artifact(
    path: Path,
    fully_static: bool,
    static_windows_crt: bool,
    static_cpp_runtime: bool,
) -> int:
    failures = 0
    with tempfile.TemporaryDirectory(prefix="pigzpp-deps-") as temporary:
        root = extract(path, Path(temporary))
        binaries = []
        for candidate in root.rglob("*"):
            kind = binary_kind(candidate)
            if kind:
                binaries.append((candidate, kind))

        if not binaries:
            print(f"error: no native binaries found in {path}", file=sys.stderr)
            return 1

        for binary, kind in binaries:
            deps = dependencies(binary, kind)
            relative = binary.relative_to(root)
            print(f"{path.name}: {relative} [{kind}]")
            print(f"  dependencies: {', '.join(deps) if deps else '(none)'}")

            forbidden = [dep for dep in deps if BANNED_COMPRESSION.search(dep)]
            if forbidden:
                failures += 1
                print(
                    f"error: dynamic compression dependency: {', '.join(forbidden)}",
                    file=sys.stderr,
                )

            if fully_static and kind == "elf" and deps:
                failures += 1
                print("error: Linux CLI is not fully static", file=sys.stderr)

            if static_windows_crt and kind == "pe":
                runtime = [
                    dep for dep in deps if BANNED_WINDOWS_RUNTIME.match(os.path.basename(dep))
                ]
                if runtime:
                    failures += 1
                    print(
                        f"error: Windows CLI uses dynamic C/C++ runtime: {', '.join(runtime)}",
                        file=sys.stderr,
                    )

            if static_cpp_runtime and kind == "elf":
                runtime = [
                    dep for dep in deps if BANNED_ELF_CPP_RUNTIME.match(os.path.basename(dep))
                ]
                if runtime:
                    failures += 1
                    print(
                        f"error: ELF binary uses dynamic C++/GCC runtime: {', '.join(runtime)}",
                        file=sys.stderr,
                    )
    return failures


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("artifacts", nargs="+", help="Artifact paths or glob patterns")
    parser.add_argument(
        "--fully-static",
        action="store_true",
        help="Require ELF binaries to have no dynamic dependencies",
    )
    parser.add_argument(
        "--static-windows-crt",
        action="store_true",
        help="Reject dynamic MSVC and Universal CRT dependencies in PE binaries",
    )
    parser.add_argument(
        "--static-cpp-runtime",
        action="store_true",
        help="Reject dynamic GNU C++ and GCC support libraries in ELF binaries",
    )
    args = parser.parse_args()

    paths: list[Path] = []
    for pattern in args.artifacts:
        matches = [Path(item) for item in glob.glob(pattern)]
        if not matches:
            print(f"error: artifact pattern matched nothing: {pattern}", file=sys.stderr)
            return 2
        paths.extend(matches)

    failures = 0
    for path in paths:
        failures += inspect_artifact(
            path,
            args.fully_static,
            args.static_windows_crt,
            args.static_cpp_runtime,
        )
    return 1 if failures else 0


if __name__ == "__main__":
    raise SystemExit(main())
