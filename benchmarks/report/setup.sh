#!/usr/bin/env bash
# Prepare all inputs and binaries needed by make bench-report.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
cd "$ROOT"

NPROC="${NPROC:-$(nproc)}"
GO_VERSION="${GO_VERSION:-1.22.5}"
KODAK_DIR="${KODAK_DIR:-/tmp/kodak}"
LAYER="${LAYER:-$ROOT/build/docker_bench/layer.tar}"

echo "==> Building native targets and Python extension"
cmake -S . -B build \
  -DCMAKE_BUILD_TYPE=Release \
  -DPIGZPP_BUILD_CAPI=ON \
  -DPIGZPP_LTO=ON \
  -DPIGZPP_NATIVE=OFF
cmake --build build -j"$NPROC"
pip install . --no-build-isolation -q

echo "==> Preparing shared corpora and Python competitors"
python3 benchmarks/core/gen_data.py --sizes 16 128 --data-dir build/bench_data
pip install -q zlib-ng isal pillow opencv-python-headless pandas matplotlib seaborn

# Repository-local Go keeps this setup unprivileged and version-pinned.
GO="$ROOT/tmp/go/bin/go"
GOFMT="$ROOT/tmp/go/bin/gofmt"
if [[ ! -x "$GO" ]]; then
  echo "==> Installing local Go ${GO_VERSION}"
  mkdir -p tmp/go/bin tmp/go-cache
  curl -fsSL "https://go.dev/dl/go${GO_VERSION}.linux-amd64.tar.gz" | tar -xz -C tmp/go-cache
  ln -sf "$ROOT/tmp/go-cache/go/bin/go" "$GO"
  ln -sf "$ROOT/tmp/go-cache/go/bin/gofmt" "$GOFMT"
fi

echo "==> Building Go and Rust competitors"
"$GOFMT" -w benchmarks/go-docker/main.go
# `go build` enables the optimizing compiler and inliner by default (there is
# no separate Go "release" mode). Clear inherited GOFLAGS so a developer's
# `-gcflags=all=-N -l` cannot silently turn optimizations off.
(cd benchmarks/go-docker && env GOFLAGS= "$GO" build -trimpath -o dockergzbench .)
(cd benchmarks/rust && cargo build --release --offline)

# Build WASM only when the expected artifacts are absent.
if [[ ! -f build-wasm-simd/wasm/pigzpp_wasm.mjs || \
      ! -f build-wasm-threads/wasm/pigzpp_wasm.mjs ]]; then
  echo "==> Building WASM SIMD and threaded modules"
  if [[ -f "$HOME/emsdk/emsdk_env.sh" ]]; then
    # shellcheck disable=SC1090
    source "$HOME/emsdk/emsdk_env.sh" >/dev/null
  fi
  scripts/build_wasm.sh simd
  scripts/build_wasm.sh threads
fi
(cd benchmarks/wasm && npm install --silent)

if [[ ! -f "$KODAK_DIR/kodim24.png" ]]; then
  echo "==> Downloading Kodak Lossless True Color Image Suite"
  mkdir -p "$KODAK_DIR"
  for i in $(seq -w 1 24); do
    curl -fsSL "https://r0k.us/graphics/kodak/kodak/kodim${i}.png" \
      -o "$KODAK_DIR/kodim${i}.png"
  done
fi

if [[ ! -f "$LAYER" ]]; then
  echo "==> Downloading largest python:3.12 amd64 layer"
  mkdir -p "$(dirname "$LAYER")"
  (cd benchmarks/go-docker && ./fetch_layer.sh library/python 3.12 "$LAYER")
fi

echo "==> Report benchmark setup ready"
echo "    Kodak: $KODAK_DIR"
echo "    Docker layer: $LAYER"
