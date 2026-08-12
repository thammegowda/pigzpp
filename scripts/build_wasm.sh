#!/usr/bin/env bash
# Build pigzpp WebAssembly modules.
#
# Produces up to three portable (OS/arch-agnostic) variants under build-wasm-*/wasm/:
#   baseline  — no SIMD, no threads (runs everywhere)
#   simd      — 128-bit WASM SIMD (all modern engines)
#   threads   — SIMD + pthreads (needs cross-origin isolation in browsers)
#
# Prerequisites:
#   - Emscripten SDK activated:  source /path/to/emsdk/emsdk_env.sh
#   - Submodules initialized:    git submodule update --init third_party/zlib-ng third_party/zopfli
#
# Usage:
#   scripts/build_wasm.sh [baseline|simd|threads|all]   (default: simd)

set -euo pipefail

cd "$(dirname "$0")/.."
ROOT="$PWD"

if ! command -v emcmake >/dev/null 2>&1; then
    echo "error: emcmake not found. Activate Emscripten: source <emsdk>/emsdk_env.sh" >&2
    exit 1
fi

build_variant() {
    local name="$1"; shift
    local dir="build-wasm-${name}"
    echo "=== Building variant: ${name} (${dir}) ==="
    rm -rf "${dir}"
    mkdir -p "${dir}"
    ( cd "${dir}" && emcmake cmake .. -DCMAKE_BUILD_TYPE=Release \
        -DPIGZPP_DISTRIBUTION_BUILD=ON "$@" >/dev/null )
    ( cd "${dir}" && emmake make pigzpp_wasm -j"$(nproc 2>/dev/null || echo 4)" )
    echo "--> ${ROOT}/${dir}/wasm/pigzpp_wasm.mjs"
}

target="${1:-simd}"
case "${target}" in
    baseline) build_variant baseline -DPIGZPP_WASM_SIMD=OFF -DPIGZPP_WASM_THREADS=OFF ;;
    simd)     build_variant simd     -DPIGZPP_WASM_SIMD=ON  -DPIGZPP_WASM_THREADS=OFF ;;
    threads)  build_variant threads  -DPIGZPP_WASM_SIMD=ON  -DPIGZPP_WASM_THREADS=ON ;;
    all)
        build_variant baseline -DPIGZPP_WASM_SIMD=OFF -DPIGZPP_WASM_THREADS=OFF
        build_variant simd     -DPIGZPP_WASM_SIMD=ON  -DPIGZPP_WASM_THREADS=OFF
        build_variant threads  -DPIGZPP_WASM_SIMD=ON  -DPIGZPP_WASM_THREADS=ON
        ;;
    *) echo "usage: $0 [baseline|simd|threads|all]" >&2; exit 2 ;;
esac

echo "Done."
