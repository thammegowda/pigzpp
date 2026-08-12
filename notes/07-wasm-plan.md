# WebAssembly Port Plan for pigzpp

## 0. Implementation status (verified)

All phases implemented on branch `tg/wasm` and verified under Node 22 (bundled with
Emscripten 6.0.2):

- **Buffer API**: `Compressor::compress_buffer` / `Decompressor::decompress_buffer`.
  Single-threaded gzip/zlib take a **direct in-memory deflate/inflate path** (zlib-ng on
  the buffer, no copies); the parallel pipeline, zopfli (level 11), rsyncable, and zip
  framing fall back to an in-memory temp-fd facade. Native roundtrip + `gunzip` validated.
- **Emscripten CMake**: `EMSCRIPTEN` guards force ISA-L/native/static/LTO/Python/tests off;
  add `-fexceptions`, optional `-msimd128` / `-pthread`.
- **Embind module** `src/wasm/pigzpp_wasm.cpp`: `gzipCompress`, `gzipDecompress`,
  `pngEncode`, `pngDecode`, `version`, `threadsEnabled`.
- **Build script** `scripts/build_wasm.sh [baseline|simd|threads|all]`.
- **Feature-detecting loader** `src/wasm/pigzpp-loader.mjs`.
- **Benchmark harness** `benchmarks/wasm/` (vs `CompressionStream`, node-zlib, fflate, pako).

Verified results (16 MB corpus, single-thread SIMD): gzip validated against Node `zlib`;
PNG encode/decode roundtrips. Threaded variant: 4 threads ≈ 2× the 1-thread speed.

**Gotchas solved during implementation** (each was a hard build/run failure):
1. zlib-ng's runtime CPU-dispatch functable emits `*_stub` calls that fail wasm's strict
   call-signature validation → set `WITH_RUNTIME_CPU_DETECTION=OFF` for Emscripten.
2. Default 64 KB wasm stack overflows (PNG uses a 64 KB stack buffer) → `-sSTACK_SIZE=8MB`.
3. C++ exceptions need `-fexceptions` (Emscripten model) or `wasm-opt` can't parse.
4. LTO forced off for Emscripten for reproducibility.

Sample throughput (Node 22, single-thread, 16 MB semi-compressible corpus):

| engine | MB/s | ratio |
|--------|-----:|------:|
| pigzpp-wasm L1 | 186 | 0.390 |
| pigzpp-wasm L6 | 30 | 0.182 |
| CompressionStream | 21 | 0.185 |
| fflate L6 | 12 | 0.172 |
| pako L6 | 8 | 0.185 |

Thread scaling (Node 22, 10-core host, 128 MB text corpus, gzip level 6, best-of-3;
`benchmarks/wasm/scaling.mjs`):

| threads | MB/s | speedup | time (ms) | ratio |
|--------:|-----:|--------:|----------:|------:|
| 1 | 25.6 | 1.00× | 5253 | 0.181 |
| 2 | 49.6 | 1.94× | 2706 | 0.181 |
| 4 | 82.8 | 3.24× | 1620 | 0.181 |
| 8 | 106.3 | 4.16× | 1263 | 0.181 |

Near-linear at low thread counts, tapering to 4.16× at 8 (serial read/write-ordering
stages + WASM worker overhead). Ratio stays constant across thread counts — cross-block
dictionary linking preserves quality regardless of parallelism. At 8 threads pigzpp-wasm
reaches ~5× native `CompressionStream`'s throughput at a better ratio.

Single-thread parity vs a thin zlib-ng-wasm wrapper (same engine; 32 MB corpus,
`benchmarks/wasm/compare_zlibng.mjs`). The direct in-memory path removes the temp-fd
overhead that previously cost ~30% at level 1:

| level | pigzpp/zlib-ng (before) | pigzpp/zlib-ng (after direct path) |
|------:|:-----------------------:|:----------------------------------:|
| 1 | 0.70× | 0.98× |
| 6 | 0.96× | 1.00× |
| 9 | 0.98× | 0.98× |

Ratios are byte-identical (same zlib-ng engine). So single-threaded, pigzpp-wasm now
matches a minimal zlib-ng binding; its differentiators are the PNG codec, zopfli
max-ratio, multi-format/tunability, and the threaded scaling above.

## 1. Motivation

Web applications routinely compress data (uploads, telemetry, offline caches) and
encode images (canvas exports, generated assets). Today they rely on:

- The browser's built-in `CompressionStream` / `DecompressionStream` (gzip/deflate),
  which is zero-download but single-threaded and offers **no level/strategy control**.
- Pure-JS libraries (`fflate`, `pako`) that add download weight and run single-threaded.
- The browser's `canvas.toBlob('image/png')`, a fixed-setting single-threaded PNG encoder.

pigzpp's differentiators — **block-parallel DEFLATE with dictionary linking**,
**parallel CRC**, tunable **level/strategy/filter**, and a purpose-built **PNG encoder** —
could give webapps faster and/or smaller output than any of the above, *if* the
parallelism survives the move to WebAssembly.

## 2. Does the parallelism carry over?

### What pigzpp uses today

| Primitive | Location | Purpose |
|-----------|----------|---------|
| `std::jthread` workers | `compress.cpp`, `decompress.cpp`, `format.cpp` | Per-block compress + dedicated write thread |
| `std::mutex` / `std::condition_variable` | `pool.h`, `compress.cpp` | Producer/consumer buffer handoff |
| `std::thread::hardware_concurrency()` | `config.h` | Sizes the worker pool |
| `BufferPool` (shared_ptr + custom deleter) | `pool.h` | Memory reuse across blocks |

This is a classic pthread-style producer/consumer model.

### Mapping to WASM

Emscripten implements POSIX threads on top of **Web Workers + `SharedArrayBuffer`**.
Every primitive pigzpp uses maps directly:

| pigzpp uses | WASM backing | Works? |
|-------------|--------------|--------|
| `std::jthread` / `std::thread` | pthread → Web Worker | ✅ with `-pthread` |
| `std::mutex`, `std::condition_variable` | futex on shared memory | ✅ |
| `std::thread::hardware_concurrency()` | `navigator.hardwareConcurrency` | ✅ |
| buffer pool / shared_ptr handoff | normal linear memory | ✅ |

**Conclusion: the threading model carries over unchanged.** Nothing in pigzpp is
fundamentally incompatible with WASM threads.

## 3. WASM-specific restrictions (the real constraints)

1. **`SharedArrayBuffer` requires cross-origin isolation.**
   The hosting page must send:
   ```
   Cross-Origin-Opener-Policy: same-origin
   Cross-Origin-Embedder-Policy: require-corp
   ```
   Without these headers there is **no `SharedArrayBuffer` → no threads**, only a
   single-thread fallback. This is the biggest adoption blocker: many hosts/CDNs and
   third-party/embedded contexts cannot set these headers, and enabling them can break
   other cross-origin resources. `CompressionStream` needs none of this — its key edge.

2. **Never block the browser main thread.**
   pigzpp's caller blocks on a condition variable waiting for workers. On the browser
   main thread, blocking futex waits are illegal/deadlock-prone. Fix: build with
   **`-sPROXY_TO_PTHREAD`** so the entry point runs on a worker and can block freely.
   The JS-facing API must therefore be **async**.

3. **Thread creation is not free.**
   Spawning a pthread spins up a Web Worker (fetch + module instantiate). Use a
   **pre-warmed pool** via `-sPTHREAD_POOL_SIZE=N` and create the pool once, reusing it
   across calls. pigzpp's create-pool-once design already fits this.

4. **Shared, growable memory quirks.**
   Threaded builds need `-sSHARED_MEMORY` and a carefully-set `MAXIMUM_MEMORY`, since
   growing shared memory is more constrained than non-shared. Large-buffer allocation
   may need tuning.

5. **Node.js is much easier.**
   Under Node, `SharedArrayBuffer` and `worker_threads` are available **without**
   COOP/COEP headers. A threaded pigzpp-WASM "just works" server-side / in build tools /
   Electron — arguably the strongest near-term use case.

## 4. Portability across OS and CPU architecture

The whole point of WASM: a compiled `.wasm` module is **portable bytecode**, not machine
code. The **same artifact runs unchanged** on macOS, Windows, Linux (and iOS/Android
browsers) and on x86-64, ARM64/aarch64, and RISC-V. The host engine (V8, JavaScriptCore,
SpiderMonkey, Wasmtime/Wasmer) compiles the bytecode to the local ISA at load time. There
is **no per-OS or per-arch build matrix** — the opposite of shipping native binaries.

Portability cost moves from a build-time `{os} × {arch}` matrix to **one toolchain
producing one (or a few) `.wasm` files.**

### What we need

1. **Emscripten toolchain (build-time only).** `emsdk install/activate latest` on any dev
   OS; build once in CI. Emits `.wasm` + a `.mjs`/`.js` glue loader that runs everywhere.

2. **Portable dependencies.** The engine choice is critical:
   - **zlib-ng** — has WASM support; runtime dispatch falls back to a generic/SIMD-128
     path. ✅ Use this as the WASM DEFLATE backend.
   - **ISA-L (igzip)** — hand-written **x86 NASM assembly** (see `notes/06`). It has **no
     WASM backend and cannot compile to WASM**. The threaded-igzip path is **x86-native
     only** and must be **excluded from WASM builds**. ❌
   - **zopfli**, **libpng/zlib** — pure C, compile to WASM fine. ✅

3. **SIMD (performance, not correctness).** WASM has a standardized **128-bit SIMD**
   extension. Build with `-msimd128` → one binary, accelerated on **both** x64 (SSE/AVX)
   and ARM64 (NEON) — the engine maps the ops. Caveat: WASM SIMD is **128-bit only**, so
   no AVX2/AVX-512-width ops; expect WASM throughput to be a fraction of native.

4. **Threads (host-gated, not arch/OS-gated).** The same threaded `.wasm` runs identically
   on x64 and aarch64; the engine maps pthreads to OS threads. Availability depends on the
   host (browser needs COOP/COEP; Node/Deno/Bun do not) — see section 3.

### Portability matrix

| Concern | Portable? | Action |
|---------|-----------|--------|
| OS (mac/win/linux) | ✅ Fully | None — one `.wasm` |
| CPU arch (x64/aarch64) | ✅ Fully | None — engine handles it |
| zlib-ng engine | ✅ | Use as WASM DEFLATE backend |
| **ISA-L / igzip** | ❌ | **Exclude from WASM builds** (x86 asm) |
| zopfli / libpng | ✅ | Compile with Emscripten |
| SIMD accel | ✅ (128-bit) | `-msimd128`; no AVX-width |
| Threads | ⚠️ host-gated | `-pthread`; browser needs COOP/COEP |

The only real portability work: **(a) drop ISA-L in favor of zlib-ng for the WASM engine**,
and **(b) decide how many SIMD/threads variants to ship** (see section 5).

## 5. Competitive landscape (benchmark targets)

**gzip / DEFLATE:**

| Target | Kind | Notes |
|--------|------|-------|
| `CompressionStream("gzip")` | Native | Zero download, single-thread, no tuning |
| fflate | Pure JS | Fastest JS; small; has async/worker API |
| pako | Pure JS | zlib port; de-facto standard; slower |
| zlib-ng-wasm | WASM | Single-thread reference for our own engine |
| **pigzpp-wasm** | WASM | Single-thread + threaded variants |

**PNG encode:**

| Target | Kind | Notes |
|--------|------|-------|
| `canvas.toBlob('image/png')` | Native | Fixed settings, single-thread |
| UPNG.js | Pure JS | Popular encoder/decoder |
| @jsquash/oxipng | WASM | Rust OxiPNG, strong optimizer |
| **pigzpp-png-wasm** | WASM | Tunable filter/strategy/level |

**Metrics:** throughput (MB/s), compressed size (ratio), and **cold-start cost**
(WASM download + instantiate — a fixed penalty native `CompressionStream` avoids).

## 6. Proposed build variants

All variants use the **zlib-ng** engine (ISA-L excluded — see section 4) and are OS- and
arch-agnostic. Drive selection with *runtime feature detection*, not per-platform builds:

1. **Baseline WASM** — no SIMD, no threads. Runs literally everywhere. Widest reach.
2. **SIMD WASM** — `-msimd128`. Accelerated on x64 (SSE/AVX) and ARM64 (NEON) from one
   binary. Competes on **size + quality + tunability** vs `CompressionStream`/fflate.
3. **SIMD + threaded WASM** — adds `-pthread -sPROXY_TO_PTHREAD
   -sPTHREAD_POOL_SIZE=<procs> -sSHARED_MEMORY -sMAXIMUM_MEMORY=<cap>`. Requires
   cross-origin isolation (browser) / Node. Competes on **throughput** for large payloads.

A tiny JS loader feature-detects WASM-SIMD support and `crossOriginIsolated`, then picks
the best available variant and falls back gracefully.

## 7. Where pigzpp can win / lose

- **Win:** large payloads (multi-MB gzip, big PNG encodes) where block-parallelism
  amortizes worker overhead; PNG quality/speed if the filter+strategy pipeline beats
  `canvas.toBlob`.
- **Lose / neutral:** small payloads (WASM load + copy + `postMessage` overhead dominates);
  and it must justify its download size against the *zero-byte* native `CompressionStream`.

## 8. Suggested next steps

1. Build a **benchmark harness** (`benchmarks/wasm/`) for Node + browser comparing
   `CompressionStream`, fflate, pako, and a placeholder for pigzpp-wasm — establishes the
   measurement framework before toolchain investment.
2. Prototype the **Emscripten single-thread build** of `pigzpp::Compressor` and
   `pigzpp::png::encode` exposing a minimal buffer-in / buffer-out API.
3. Add the **threaded build** and measure scaling vs the single-thread baseline and the
   competitors, explicitly documenting the COOP/COEP requirement as a deployment cost.
