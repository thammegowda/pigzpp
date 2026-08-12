#!/usr/bin/env bash
# Produce a real Docker image layer tar (layer.tar) to feed dockergzbench.
#
# Usage:
#   ./gen_layer.sh [image] [outfile]
#
# Examples:
#   ./gen_layer.sh                     # uses python:3.12-slim -> layer.tar
#   ./gen_layer.sh node:20 node.tar
#
# Requires docker (or podman via DOCKER=podman) on PATH.
set -euo pipefail

IMAGE="${1:-python:3.12-slim}"
OUT="${2:-layer.tar}"
DOCKER="${DOCKER:-docker}"

tmpdir="$(mktemp -d)"
trap 'rm -rf "$tmpdir"' EXIT

echo "Pulling $IMAGE ..."
"$DOCKER" pull "$IMAGE" >/dev/null

echo "Saving image to a tarball ..."
"$DOCKER" save "$IMAGE" -o "$tmpdir/image.tar"

echo "Extracting layers ..."
tar -xf "$tmpdir/image.tar" -C "$tmpdir"

# OCI/Docker save layouts differ; find the largest layer blob (the interesting one).
# Handles both legacy (<hash>/layer.tar) and OCI (blobs/sha256/<hash>) layouts.
largest="$(find "$tmpdir" -type f \( -name 'layer.tar' -o -path '*/blobs/sha256/*' \) \
	-printf '%s\t%p\n' | sort -rn | head -1 | cut -f2)"

if [[ -z "${largest:-}" ]]; then
	echo "error: no layer found in image tarball" >&2
	exit 1
fi

# The blob may itself be gzip-compressed; decompress to a raw tar if so.
if gzip -t "$largest" 2>/dev/null; then
	echo "Largest layer is gzip-compressed; decompressing to raw tar ..."
	gunzip -c "$largest" > "$OUT"
else
	cp "$largest" "$OUT"
fi

echo "Wrote $OUT ($(du -h "$OUT" | cut -f1))"
echo "Run: ./dockergzbench -input $OUT -iters 5 -threads \$(nproc) -pigzpp ../../build/pigzpp"
