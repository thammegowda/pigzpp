#!/usr/bin/env python3
"""Sanitize exported Copilot / agent chat histories for public sharing.

Removes or redacts:
  - Absolute home-directory paths  (/home/<user>/...)
  - VS Code server internal paths  (.vscode-server/data/...)
  - Copilot workspace-storage IDs and chat-session-resource paths
  - Memory-tool file operations     (Created/Updated/Read memory file ...)
  - file:// URIs that embed local paths
  - Agent tool-call metadata lines  (Read [...], Checked [...], Searched ..., etc.)
  - "Completed with input:" echo lines
  - Terminal command lines with absolute paths (redacted, not removed)

Usage:
    python sanitize_chat.py <input.md> [-o <output.md>] [--dry-run]

If -o is omitted the output goes to <input>_sanitized.md.
With --dry-run, prints a diff-like summary to stdout without writing.
"""

import argparse
import re
import sys
from pathlib import Path


# ---------------------------------------------------------------------------
# Pattern catalogue – extend as needed
# ---------------------------------------------------------------------------

# Lines to DROP entirely (matched against stripped line)
DROP_LINE_PATTERNS: list[re.Pattern] = [
    # Tool bookkeeping noise
    re.compile(r"^(Read|Checked|Searched)\s+\["),
    re.compile(r"^Read\s+(memory|changed files)"),
    re.compile(r"^(Created|Updated|Inserted into)\s+memory\s+file"),
    re.compile(r"^Created\s+\["),
    re.compile(r"^Generating\s+patch\s+\("),
    re.compile(r"^Completed\s+with\s+input:"),
    re.compile(r"^Searched\s+(codebase\s+)?for\b"),
    re.compile(r"^Searched\s+for\s+(text|regex)\b"),
    re.compile(r"^Read changed files"),
    re.compile(r"^Ran terminal command:\s+chmod\b"),   # chmod noise
    re.compile(r"^Inspect\s+\w"),       # "Inspect pigz core", etc.
    re.compile(r"^Explore\s+\w"),       # "Explore tests and benchmarks"
    re.compile(r"^Read\s+memory\b"),
    re.compile(r"^Starting:\s+\*"),     # "Starting: *Add CLI compatibility tests*"
    # Blank markdown link targets that pointed at local files
    re.compile(r"^\[\]\(file:///"),
]

# Inline replacements applied to every surviving line
INLINE_SUBS: list[tuple[re.Pattern, str]] = [
    # Strip file:// URIs with local paths entirely
    (re.compile(r"\[?\]?\(file:///[^)]+\)\s*,?\s*"), ""),
    # Absolute home paths  →  relative or placeholder
    (re.compile(r"/home/\w+/work/repos/me/pigz-claude/pigzpp/"), ""),
    (re.compile(r"/home/\w+/work/repos/me/pigz-claude/"), ""),
    (re.compile(r"/home/\w+/work/repos/me/pigz-gpt/"), ""),
    (re.compile(r"/home/\w+/work/repos/me/[^\s/)]+/"), "<repo>/"),
    (re.compile(r"/home/\w+/"), "~/"),
    # .vscode-server internal paths
    (re.compile(
        r"file:///[^\s)]*\.vscode-server/data/User/workspaceStorage"
        r"/[^\s)]*"
    ), "<vscode-internal>"),
    (re.compile(r"~?/\.vscode-server/[^\s)]+"), "<vscode-internal>"),
    # Copilot session / workspace-storage UUIDs
    (re.compile(
        r"[0-9a-f]{8}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{12}"
    ), "<session-id>"),
    # Base64-ish workspace-storage folder names
    (re.compile(r"workspaceStorage/[A-Za-z0-9_-]{20,}/"), "workspaceStorage/<id>/"),
    # leftover empty markdown links
    (re.compile(r"\[\]\(\s*\)"), ""),
    # collapse runs of blank lines (3+ → 2)
    # (handled in post-processing, not inline)
]


def should_drop(line: str) -> bool:
    """Return True if the entire line should be removed."""
    stripped = line.strip()
    if not stripped:
        return False  # keep blank lines (collapsed later)
    return any(p.search(stripped) for p in DROP_LINE_PATTERNS)


def sanitize_line(line: str) -> str:
    """Apply all inline substitutions to *line*."""
    for pat, repl in INLINE_SUBS:
        line = pat.sub(repl, line)
    return line


def collapse_blank_lines(text: str) -> str:
    """Replace 3+ consecutive blank lines with exactly 2."""
    return re.sub(r"\n{4,}", "\n\n\n", text)


def sanitize(text: str) -> str:
    """Full pipeline: drop → inline-sub → collapse."""
    lines = text.splitlines(keepends=True)
    out: list[str] = []
    for line in lines:
        if should_drop(line):
            continue
        out.append(sanitize_line(line))
    return collapse_blank_lines("".join(out))


# ---------------------------------------------------------------------------
# CLI
# ---------------------------------------------------------------------------

def main() -> None:
    ap = argparse.ArgumentParser(
        description="Sanitize agent chat-history markdown for public sharing."
    )
    ap.add_argument("input", type=Path, help="Input markdown file")
    ap.add_argument(
        "-o", "--output", type=Path, default=None,
        help="Output file (default: <input>_sanitized.md)",
    )
    ap.add_argument(
        "--dry-run", action="store_true",
        help="Print stats to stdout; don't write",
    )
    args = ap.parse_args()

    src = args.input.read_text()
    result = sanitize(src)

    src_lines = src.splitlines()
    res_lines = result.splitlines()
    dropped = len(src_lines) - len(res_lines)

    if args.dry_run:
        print(f"Input:   {args.input}  ({len(src_lines)} lines)")
        print(f"Output:  {len(res_lines)} lines  ({dropped} lines removed)")
        # Show a sample of what was dropped
        kept = set(result.splitlines())
        removed = [l for l in src_lines if l not in kept]
        print(f"\nSample removed lines (first 20):")
        for r in removed[:20]:
            print(f"  - {r[:120]}")
        return

    out_path = args.output or args.input.with_stem(args.input.stem + "_sanitized")
    out_path.write_text(result)
    print(f"Sanitized {args.input} → {out_path}")
    print(f"  {len(src_lines)} → {len(res_lines)} lines  ({dropped} removed)")


if __name__ == "__main__":
    main()
