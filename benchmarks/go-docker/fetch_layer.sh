#!/usr/bin/env bash
# Download a real image layer straight from a container registry (no Docker
# daemon required) and decompress it to a raw tar for dockergzbench.
#
# Usage:
#   ./fetch_layer.sh [repo] [ref] [outfile]
#
# Examples:
#   ./fetch_layer.sh                      # library/python:3.12 -> layer.tar
#   ./fetch_layer.sh library/node 20 node.tar
#
# Picks the amd64/linux manifest and the largest layer blob. Requires curl and
# python3.
set -euo pipefail

REPO="${1:-library/python}"
REF="${2:-3.12}"
OUT="${3:-layer.tar}"
REG="https://registry-1.docker.io"

echo "Authenticating for ${REPO}:${REF} ..."
TOKEN=$(curl -s "https://auth.docker.io/token?service=registry.docker.io&scope=repository:${REPO}:pull" \
  | python3 -c "import sys,json;print(json.load(sys.stdin)['token'])")

echo "Resolving amd64 manifest ..."
IDX=$(curl -s -H "Authorization: Bearer $TOKEN" \
  -H "Accept: application/vnd.docker.distribution.manifest.list.v2+json" \
  -H "Accept: application/vnd.oci.image.index.v1+json" \
  "${REG}/v2/${REPO}/manifests/${REF}")
AMD64=$(echo "$IDX" | python3 -c "import sys,json;d=json.load(sys.stdin);print([m['digest'] for m in d['manifests'] if m.get('platform',{}).get('architecture')=='amd64' and m['platform'].get('os')=='linux'][0])")

MAN=$(curl -s -H "Authorization: Bearer $TOKEN" \
  -H "Accept: application/vnd.docker.distribution.manifest.v2+json" \
  -H "Accept: application/vnd.oci.image.manifest.v1+json" \
  "${REG}/v2/${REPO}/manifests/${AMD64}")
read -r DIGEST SIZE < <(echo "$MAN" | python3 -c "import sys,json;d=json.load(sys.stdin);L=sorted(d['layers'],key=lambda x:x['size'])[-1];print(L['digest'],L['size'])")
echo "Largest layer: ${DIGEST} ($((SIZE/1024/1024)) MB compressed)"

echo "Downloading ..."
curl -sL -H "Authorization: Bearer $TOKEN" "${REG}/v2/${REPO}/blobs/${DIGEST}" -o "${OUT}.gz"
gunzip -f "${OUT}.gz"
echo "Wrote ${OUT} ($(du -h "$OUT" | cut -f1) raw tar)"
echo "Run: ./dockergzbench -input ${OUT} -iters 3 -threads 8 -methods stdlib,pgzip,pigzppcgo-zlib,pigzppcgo-isal"
