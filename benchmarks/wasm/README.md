# pigzpp WebAssembly benchmarks

Compares `pigzpp-wasm` gzip compression against native and pure-JS options.

## Prerequisites

1. Activate Emscripten and build a module variant:
   ```bash
   source <emsdk>/emsdk_env.sh
   git submodule update --init third_party/zlib-ng third_party/zopfli
   scripts/build_wasm.sh simd     # produces build-wasm-simd/wasm/pigzpp_wasm.mjs
   ```
2. Install the optional JS competitors (fflate, pako):
   ```bash
   cd benchmarks/wasm && npm install
   ```
3. Use a Node with `CompressionStream` (Node 18+; the emsdk bundles Node 22).

## Run

```bash
node bench.mjs --size 16 --iters 5
# or point at a specific module:
node bench.mjs --module ../../build-wasm-simd/wasm/pigzpp_wasm.mjs
```

## Competitors

| Engine | Kind | Notes |
|--------|------|-------|
| pigzpp-wasm | WASM | this project (zlib-ng core, single-thread + SIMD) |
| CompressionStream | Native | built into Node/browsers; no level/strategy control |
| node-zlib | Native | Node's libz reference |
| fflate | Pure JS | fastest JS deflate |
| pako | Pure JS | de-facto standard zlib port |

## Sample result (16 MB semi-compressible corpus, Node 22, single-thread)

| engine | MB/s | ratio |
|---|---:|---:|
| pigzpp-wasm L1 | 186 | 0.390 |
| pigzpp-wasm L6 | 30 | 0.182 |
| CompressionStream | 21 | 0.185 |
| fflate L6 | 12 | 0.172 |
| pako L6 | 8 | 0.185 |

Numbers vary by machine and corpus; treat as relative. pigzpp-wasm's advantage
grows with the threaded variant on large payloads (see `notes/07-wasm-plan.md`).
