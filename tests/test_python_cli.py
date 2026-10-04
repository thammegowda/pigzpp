"""Tests for the Python module and console-script CLI entry points."""

from __future__ import annotations

from pathlib import Path
import os
import subprocess
import sys


def run(*arguments: str, input: bytes | None = None) -> subprocess.CompletedProcess[bytes]:
    return subprocess.run(
        [sys.executable, "-m", "pigzpp", *arguments],
        input=input,
        capture_output=True,
        check=False,
    )


def test_module_help():
    result = run("--help")
    assert result.returncode == 0
    assert b"Usage: pigzpp [options] [files ...]" in result.stderr


def test_module_version():
    result = run("--version")
    assert result.returncode == 0
    assert result.stdout.startswith(b"pigzpp ")


def test_module_stdin_roundtrip():
    source = (b"native CLI through python -m pigzpp\n" * 1000)
    compressed = run("-c", input=source)
    assert compressed.returncode == 0
    restored = run("-dc", input=compressed.stdout)
    assert restored.returncode == 0
    assert restored.stdout == source


def test_console_script():
    executable = Path(sys.executable).with_name(
        "pigzpp.exe" if os.name == "nt" else "pigzpp"
    )
    result = subprocess.run(
        [executable, "--version"],
        capture_output=True,
        check=False,
    )
    assert result.returncode == 0
    assert result.stdout.startswith(b"pigzpp ")
