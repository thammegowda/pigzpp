# ISA-L Integration Plan for pigzpp

## 1. Background

### What pigzpp Brings

pigzpp is a modern C++23 parallel gzip implementation derived from pigz. Its core strength is **multi-threaded orchestration** of the DEFLATE pipeline:

- **Parallel block compression**: N worker threads each compress independent blocks via `std::jthread`, with configurable block size (default 128KB) and unlimited thread count (bounded only by `hardware_concurrency`)
- **Dictionary linking**: The tail 32KB of block N is passed as a dictionary to block N+1 via `deflateSetDictionary()`, preserving cross-block LZ77 matches that naive parallel splitters lose
- **Parallel CRC**: Per-block checksums computed in parallel with a pre-computed `crc32_combine_op()` operator for O(1) merging — no serial bottleneck
- **Ordered output pipeline**: A dedicated write thread with sequence numbering guarantees correct output order without blocking compressors
- **Rsyncable mode**: Hash-based block boundaries so incremental file changes produce minimal diff in compressed output
- **Multi-format support**: gzip, zlib, zip, and zip64 with proper headers/trailers
- **Zopfli level 11**: Maximum-ratio DEFLATE for archival use
- **Library API + Python bindings**: Reusable via pybind11

The compression engine is currently **zlib-ng**, which provides SIMD-accelerated DEFLATE via runtime CPU dispatch (SSE/AVX).

### What ISA-L igzip Brings

Intel's Intelligent Storage Acceleration Library (ISA-L) provides a hand-tuned DEFLATE implementation that achieves **1.5-2x higher single-thread throughput** than zlib-ng through:

- **Hand-written x86 NASM assembly** with explicit register scheduling and instruction-level parallelism — not compiler-generated SIMD intrinsics
- **Vectorized hash matching (AVX2/AVX-512)**: `VPGATHERDD` performs 8 hash table lookups in a single instruction; `VPMADDWD` computes 8 hash values simultaneously; `VPSHUFB` extracts overlapping substrings from one vector load
- **Hardware CRC32 instruction** as the hash function (~3 cycles per 4-byte element, fully pipelined)
- **SIMD string comparison**: `VPCMPEQB` matches 32 bytes/cycle (AVX2) or 64 bytes/cycle (AVX-512) vs byte-by-byte loops
- **Two-pass Intermediate Compressed Format (ICF)**: Pass 1 finds LZ77 matches and builds symbol histograms; Pass 2 generates optimal dynamic Huffman trees from those histograms — decoupling produces better compression ratio at the same speed
- **Dual-tier Huffman decoding**: O(1) table lookup (4KB table indexed by 12 bits) eliminates tree traversal during decompression
- **BMI2 bit operations**: `SHLX`/`SHRX` for stall-free variable bit shifts in the output bitstream
- **PCLMULQDQ CRC folding**: Processes 256 bits per iteration at ~20 GB/s for gzip checksums

igzip also has basic multi-threading (max 8 threads, 1MB blocks, `isal_deflate_stateless()` with `FULL_FLUSH`), but it uses **no dictionary linking**, computes CRC **serially on the main thread**, and lacks rsyncable mode, multi-format support, or a library API.

### Benchmark Evidence

Default thread count (benchmark ran igzip single-threaded — no `-T` flag was passed):

| Operation | gzip | pigz | pigzpp (zlib-ng) | igzip |
|-----------|------|------|-------------------|-------|
| Compress 1GB | 124 MB/s | 461 MB/s | 2034 MB/s | 3174 MB/s |
| Decompress 1GB | 202 MB/s | 178 MB/s | 4737 MB/s | 4815 MB/s |
| Compress 128MB | 118 MB/s | 551 MB/s | 1815 MB/s | 2189 MB/s |

pigzpp's thread scaling with zlib-ng (single-thread vs 4 threads):

| Size | 1 thread | 4 threads | Scaling |
|------|----------|-----------|---------|
| 1GB | 1395 MB/s | 2473 MB/s | 1.8x |
| 128MB | 1336 MB/s | 2135 MB/s | 1.6x |

pigzpp with zlib-ng at 4 threads already reaches 78% of igzip's single-thread speed — despite a 2.3x slower per-core engine.

### The Case for Combining

Neither tool alone achieves the goal:

- **igzip alone**: Fast per-core, but limited to 8 threads, no dictionary linking (hurts ratio), no rsyncable mode, no multi-format, serial CRC, single-threaded decompression only
- **pigzpp alone**: Sophisticated parallel architecture, but bottlenecked by zlib-ng's per-core DEFLATE speed

The combination — **ISA-L as the per-thread compression engine inside pigzpp's parallel orchestrator** — yields:

1. **ISA-L's ~3 GB/s per-core throughput** × pigzpp's N-thread parallelism = projected **10,000+ MB/s at 4 threads** on 1GB data
2. **Better compression ratio** than igzip multi-threaded, because pigzpp's dictionary linking preserves cross-block context that igzip's stateless mode discards
3. **Parallel decompression** on pigzpp-created files (FULL_FLUSH block boundaries make blocks independently decompressible) — something igzip cannot do
4. All existing pigzpp features preserved: rsyncable, multi-format, Zopfli, Python bindings

## 2. Licensing

### License Summary

| Component | License | Copyright |
|-----------|---------|-----------|
| ISA-L | BSD-3-Clause | Intel Corporation, 2011-2024 |
| pigzpp | Zlib-like |  our code, derived from pigz by Adler |
| zlib-ng | Zlib-like | Gailly/Adler, 1995-2024 |
| Zopfli | Apache 2.0 | Google, 2011 |
| pybind11 | BSD-3-Clause | Wenzel Jakob, 2016 |

### Interpretation

All licenses in the dependency chain are **permissive and mutually compatible**. No GPL or copyleft licenses are involved.

**ISA-L (BSD-3-Clause)** permits unrestricted use, modification, and redistribution — including commercial — provided that:
1. The copyright notice and license text are retained in source and binary distributions
2. The Intel name is not used to endorse derived products without permission

There is **no explicit patent grant** in BSD-3-Clause (unlike Zopfli's Apache 2.0, which includes one). This is standard for BSD-licensed software. Intel has actively contributed ISA-L to open source since 2011 and encourages its adoption. The practical patent risk is negligible and identical to the posture of pybind11 (also BSD-3-Clause).

**Action required**: Add ISA-L's copyright notice and license reference to a `THIRD_PARTY_LICENSES` section in the project (alongside existing zlib-ng, Zopfli, and pybind11 entries).

**Conclusion**: No licensing barriers to integration.

## 3. Integration Plan

### Phase 0: Establish Fair Baseline

**Step 0a**: Update `benchmarks/bench_binary.py` to include igzip in thread-scaling benchmarks with `-T 1`, `-T 4`, `-T 8`

**Step 0b**: Add compression ratio columns — record compressed output size for each tool to quantify pigzpp's dictionary-linking advantage

**Step 0c**: Run baseline, record results. Key metrics: throughput (MB/s), ratio (%), thread scaling efficiency

### Phase 1: ISA-L Engine Integration

**Step 1 — Build system** *(no dependencies, parallel with Phase 0)*
- Add `add_subdirectory(third_party/isa-l)` to top-level `CMakeLists.txt`
- Link both `isal` and `zlib-ng` (keep zlib-ng for CRC combine and Zopfli)
- Requires NASM assembler for x86 assembly; ISA-L falls back to C on other architectures
- Files: `CMakeLists.txt`, `src/pigzpp/CMakeLists.txt`

**Step 2 — ISA-L compression backend** *(depends on Step 1)*
- Wrap `isal_deflate()` (stateful API, supports dictionaries) in `compress.cpp`
- Map compression levels:
  - zlib 0-1 → ISA-L 0 (8K hash, fastest)
  - zlib 2-5 → ISA-L 1 (8K hash + ICF two-pass)
  - zlib 6-8 → ISA-L 2 (32K hash + ICF)
  - zlib 9   → ISA-L 3 (32K hash + lazy matching)
  - zlib 11  → Zopfli (unchanged)
- Allocate `level_buf` per thread per ISA-L requirements (148KB-292KB depending on level)
- Map flush types: `Z_FULL_FLUSH` → `FULL_FLUSH`, `Z_SYNC_FLUSH` → `SYNC_FLUSH`
- Set format via `gzip_flag`: `IGZIP_GZIP` / `IGZIP_ZLIB` / `IGZIP_DEFLATE`
- Critical: use `isal_deflate()` (stateful) NOT `isal_deflate_stateless()`, so dictionary linking across blocks is preserved — this is what makes pigzpp produce better ratios than igzip's threading
- Files: `src/pigzpp/compress.cpp`, `src/pigzpp/compress.h`

**Step 3 — ISA-L decompression backend** *(depends on Step 1)*
- Replace `inflateBack()` callback-driven loop with `isal_inflate()` streaming loop
- Set `crc_flag` to `ISAL_GZIP` / `ISAL_ZLIB` for automatic checksum verification
- Handle `ISAL_NEED_DICT` for dictionary streams
- Files: `src/pigzpp/decompress.cpp`, `src/pigzpp/decompress.h`

**Step 4 — ISA-L checksums** *(depends on Step 1)*
- Per-block CRC: replace `crc32()` with ISA-L's `crc32_gzip_refl()` (PCLMULQDQ-accelerated)
- Per-block Adler32: replace `adler32()` with ISA-L's `isal_adler32()` (AVX2-vectorized)
- Keep zlib-ng's `crc32_combine_op()` / `adler32_combine()` for parallel block merging (ISA-L lacks combine)
- Files: `src/pigzpp/crc.cpp`, `src/pigzpp/crc.h`

**Step 5 — Engine selection flag** *(depends on Steps 2-4)*
- Add `--engine=isal|zlib` CLI option, default `isal`
- Fallback to zlib-ng for users who need finer compression level granularity (levels 1-9) or maximum ratio
- Files: `src/pigzpp/config.h`, `src/pigzpp/config.cpp`, `src/pigzpp/main.cpp`

### Phase 2: Pipeline Tuning for Large Datasets

**Step 6 — Block size optimization** *(depends on Phase 1)*
- Current 128KB may be suboptimal for ISA-L's pipeline
- Experiment: 256KB, 512KB, 1MB blocks for files > 16MB
- Measure both throughput and compression ratio impact
- Consider auto-scaling block size based on input size
- Files: `src/pigzpp/config.h`, `src/pigzpp/compress.cpp`

**Step 7 — I/O overhead reduction** *(depends on Phase 1)*
- Profile I/O vs compute time on 1GB+ with ISA-L engine
- Consider `mmap()` for input (avoids `read()` syscalls, leverages kernel readahead)
- Increase output buffer from 131KB to match block size
- Evaluate `io_uring` for async I/O on Linux
- Files: `src/pigzpp/io.cpp`, `src/pigzpp/io.h`

### Phase 3: Advanced Optimizations

**Step 8 — CPU dispatch verification** *(parallel with Phase 2)*
- Confirm ISA-L's runtime CPUID dispatch activates AVX-512 on capable hardware
- Add `-march=native` build option for maximum local performance
- Files: `CMakeLists.txt`

**Step 9 — Parallel decompression** *(depends on Step 3)*
- On pigzpp-created gzip files (FULL_FLUSH block boundaries), decompress blocks in parallel
- Each block is independently inflatable — run N `isal_inflate` instances across cores
- Combine CRC results post-decompression via existing `crc32_combine_op()`
- igzip cannot do this (decompression is always single-threaded)
- Files: `src/pigzpp/decompress.cpp`

**Step 10 — Profile-guided optimization** *(after Phase 2)*
- Collect profiles on representative workloads (1GB+ text, binary, mixed)
- Rebuild with PGO for branch prediction and code layout optimization
- Expected gain: 5-15%
- Files: `CMakeLists.txt`

### Files Modified

| File | Change |
|------|--------|
| `CMakeLists.txt` | Add ISA-L subdirectory, NASM detection |
| `src/pigzpp/CMakeLists.txt` | Link `isal`, conditional zlib-ng |
| `src/pigzpp/compress.cpp` | ISA-L compression backend |
| `src/pigzpp/compress.h` | ISA-L stream state, level_buf |
| `src/pigzpp/decompress.cpp` | ISA-L decompression backend |
| `src/pigzpp/decompress.h` | ISA-L inflate_state |
| `src/pigzpp/crc.cpp` / `crc.h` | ISA-L CRC32/Adler32, keep zlib-ng combine |
| `src/pigzpp/config.h` / `config.cpp` | Engine flag, level mapping, block size |
| `src/pigzpp/main.cpp` | `--engine` CLI option |
| `src/pigzpp/io.cpp` / `io.h` | I/O optimization (later phase) |
| `benchmarks/bench_binary.py` | igzip thread scaling, ratio columns |

### Verification

1. **Phase 0**: Run `igzip -T 1/4/8` to establish the true multi-threaded baseline
2. **Tests**: `cd build && ctest --output-on-failure` — all existing tests must pass
3. **Cross-tool compatibility**: `pigzpp -c file | gzip -d`, `gzip -c file | pigzpp -d`, `igzip -c file | pigzpp -d`
4. **Throughput targets**:
   - After Phase 1: match igzip single-thread throughput per core
   - After Phase 2: exceed `igzip -T N` at the same thread count
   - After Phase 3: 2x+ `igzip -T 4` on 1GB+ data
5. **Compression ratio**: pigzpp ratio ≥ igzip ratio at same thread count
6. **Feature coverage**: all levels (0-9, 11), rsyncable, gzip/zlib/zip formats
7. **Memory**: ISA-L `level_buf` allocation doesn't cause OOM with many threads

### Decisions

- **Hybrid dependency**: ISA-L for the compression/decompression engine; zlib-ng retained for CRC combine operations and Zopfli level 11
- **Stateful API**: Use `isal_deflate()` (supports dictionaries) not `isal_deflate_stateless()` — this preserves pigzpp's compression ratio advantage over igzip's naive parallel approach
- **Level mapping**: zlib 0-9 → ISA-L 0-3 (fewer granularity, but each level is faster)
- **Engine flag**: `--engine=isal|zlib` gives users the choice; ISA-L default
- **Architecture scope**: x86-64 primary target; ARM/RISC-V use ISA-L's C fallback paths

### Open Questions

1. **Stateful vs stateless throughput**: igzip's assembly may be more optimized for the stateless path. Must benchmark `isal_deflate()` vs `isal_deflate_stateless()` to confirm the stateful path doesn't incur significant overhead
2. **igzip multi-threaded baseline**: Must run `igzip -T 4` benchmark before claiming victory — it likely achieves ~8-10 GB/s on 1GB, which is the real target to beat
3. **Block size sweet spot**: Larger blocks improve ISA-L throughput but increase latency and memory; need empirical data to find the optimum for 1GB+ workloads