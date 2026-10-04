"""Run the native pigzpp command-line interface."""

from __future__ import annotations

import sys

from . import _native


def main() -> int:
    return _native._cli_main(sys.argv)


if __name__ == "__main__":
    raise SystemExit(main())
