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

## ZIP archives (`zip.mjs`)

`zip.mjs` benchmarks the WASM `ZipWriter`/`ZipReader` classes against the two
common JS zip libraries, **fflate** (`zipSync`/`unzipSync`) and **JSZip** (the
de-facto choice for reading `.docx`/`.xlsx`/`.epub` in the browser). DOCX, XLSX,
PPTX, EPUB, JAR, APK and `.whl` files are all ZIP containers, so "open a document
and read its parts" is exactly the `unzip all members` scenario below.

```bash
npm install                              # fflate + jszip
# Synthetic multi-file "document" archive (many compressible XML-like members):
node zip.mjs --size 16 --members 50
# A real ZIP-based file (Word doc, spreadsheet, epub, jar, wheel, ...):
node zip.mjs --file /path/to/document.docx
```

Sample (16 MB corpus across 50 members, Node 22, single-thread, level 6):

| create zip | MB/s | archive (MB) | ratio |
|---|---:|---:|---:|
| **pigzpp-wasm** | **44.5** | 6.03 | 2.78 |
| fflate | 15.4 | 6.16 | 2.72 |
| JSZip | 8.9 | 6.00 | 2.80 |

| read + unzip all | MB/s |
|---|---:|
| **pigzpp-wasm** | **339** |
| fflate | 115 |
| JSZip | 89 |

Reading a real **6 MB `.docx`** (19 MB of XML across 6 members):

| read + unzip all | MB/s |
|---|---:|
| **pigzpp-wasm** | **253** |
| fflate | 111 |
| JSZip | 87 |

Even single-threaded, pigzpp-wasm creates archives ~3–5× faster and reads them
~2–3× faster than the popular JS libraries, at an equal-or-better ratio.

