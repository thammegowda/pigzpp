# pigzpp report — benchmark and plot TODOs

These items require new measurements and/or regenerated figures. Prose-only
review fixes are applied directly in `report/main.tex`; this file tracks the
remaining experimental work.

## Execution priority — what must be done now

The detailed backlog below is intentionally comprehensive. It is **not** all
required for the next revision. To make the evaluation publication-ready with
the least work, complete these tasks in order:

### 1. Freeze one trustworthy result set (critical; do first)

Combine the two P0 sections below into one focused benchmark pass:

- [x] Choose one primary statistic for all headline plots: **median of 7 timed
      runs after one untimed warm-up**.
- [x] Capture every sample plus output bytes/ratio in simple JSON. A small
      per-harness JSON format is acceptable now; do not block on a perfect
      universal schema.
- [x] Record commit SHA/dirty state, exact command, timestamp, CPU/OS/WSL
      topology, library versions, corpus path/size/SHA-256, level, and workers.
- [x] Re-run the nine figures currently in the report: CLI; Python/Go/Rust;
      Docker; WASM single/scaling; PNG; ZIP.
- [x] Generate plot TSVs from those raw files and update the README from the
      same result set.

**Why #1:** this fixes the paper's largest credibility gap: current plots are
reproducible from TSV, but some TSV values cannot be traced to an exact archived
run, and harnesses currently mix median and best-of-N timing.

### 2. Add native thread scaling (critical to the “parallel” claim)

- [x] On the 128 MiB text corpus, run workers `{1, 2, 4, 5, 8, 10}` for
      `pigz`, pigzpp zlib-ng, and pigzpp ISA-L.
- [x] Compression-only is sufficient for this revision; decompression scaling
      can follow later.
- [x] Plot throughput and worker efficiency, marking 5 physical cores and the
      SMT region (8/10 workers).

**Why #2:** WASM currently has a scaling plot, but the native implementation at
the center of the paper does not.

### 3. Add one ARM64 portable-path result — **deferred**

- [ ] Run the same 128 MiB level-6 CLI comparison on one native ARM64 system
      using zlib-ng (Linux ARM64 or Apple Silicon is sufficient).
- [ ] Record the CPU and selected NEON/runtime-dispatch path.
- [ ] Add a compact x86-64-vs-ARM64 portable-zlib-ng comparison or one clearly
      labeled ARM64 panel.

**Status:** deferred because no ARM64 machine is currently available. Keep the
paper's explicit limitation until this can be measured. Distribution builds
prove availability, not performance.

### 4. Run one targeted robustness check (important; keep it small)

- [x] CLI only: benchmark levels `{1, 6, 9}` on multilingual text and one
      incompressible/random corpus for pigz, pigzpp zlib-ng, and pigzpp ISA-L.
- [x] Report throughput and output ratio/bytes together.
- [x] Use a compact speed-vs-size plot or small multiples; do not expand this
      into a large corpus study yet.

**Why #4:** this checks that the headline result is not an artifact of one level
and one compressible corpus.

### 5. Add decompression comparison (active must-do)

- [x] Compare native CLI decompression (`gzip`, `pigz`, pigzpp) on one
      common level-6 gzip stream.
- [x] Compare Python in-memory decompression (stdlib, zlib-ng, python-isal,
      pigzpp) on the same uncompressed corpus size.
- [x] Archive all seven samples and add one compact two-panel figure.

`igzip` was not installed in the reference environment, so the native panel
uses the three available interoperable CLIs and records their exact commands.

### 6. Complete the Go backend comparison (active must-do)

- [x] Add explicit pigzpp zlib-ng and ISA-L cgo methods to the Go run.
- [x] Keep the subprocess point separate because it includes process/pipe cost.
- [x] Regenerate the Go language plot with both native backends.

### If time permits after the active must-do tasks

Ranked next:

1. **Small-buffer/binding overhead** — strengthens the multi-language claim.
2. **ZIP or PNG ablation** — deepens one application-level result.

### Explicitly defer for this revision

These are useful but not blockers once the four tasks above are complete:

- exhaustive corpus matrices;
- detailed PNG filter/backend ablation;
- ZIP member/worker ablation;
- full Linux/macOS/Windows performance matrix;
- exhaustive binding microbenchmarks;
- decompression thread-scaling ablation.

**Minimum stopping point for this revision:** tasks 1, 2, 4, 5, and 6. ARM64
(task 3) remains deferred with an explicit limitation in the paper.

## P0 — Make every plotted value traceable to a raw run

- [x] Define one machine-readable benchmark-result schema (JSON preferred) with:
  - benchmark name and API/path measured;
  - git commit SHA and dirty-tree flag;
  - UTC timestamp;
  - exact command and working directory;
  - OS/kernel, native vs WSL/container, compiler/runtime versions;
  - CPU model, physical/logical CPUs visible to the process, RAM;
  - compression level, worker count, corpus path/size/SHA-256;
  - every timed sample (not only the aggregate), output size, and ratio;
  - aggregation rule and warm-up count.
- [x] Add `--json-out` (or equivalent) to the CLI, Python, Go, Rust, PNG, ZIP,
      and WASM harnesses.
- [x] Standardize the primary reported statistic to median throughput; preserve
      min/max and sample values. Do not mix median-of-N and best-of-N in the
      same comparison figure.
- [ ] Use at least 7 timed repetitions after one untimed warm-up for primary
      figures; report median and range or a 95% bootstrap confidence interval.
- [x] Store exact paper runs under `benchmarks/results/<date>/<benchmark>.json`.
- [x] Generate `report/plots/data/*.tsv` from those JSON files rather than by
      manually copying numbers.
- [x] Add a validation step that fails if a plotted value has no source run or
      if the TSV and source JSON disagree.

**Acceptance criterion:** from a clean checkout, one command regenerates every
TSV and plot from checked-in raw output, including machine/run metadata.

## P0 — Re-run the current six figures under one protocol

- [x] Record the reference environment accurately as an Ubuntu 22.04 WSL2 guest
      exposing 5 cores / 10 logical CPUs on a Xeon W-2235 host.
- [x] Document CPU-frequency/turbo state, whether affinity is used, and whether
      the machine is otherwise idle. If affinity is not controlled, say so in
      the result metadata.
- [x] Re-run the exact inputs behind:
      - [x] CLI (`gzip`, `pigz`, pigzpp zlib-ng, pigzpp ISA-L);
      - [x] Python, Go, and Rust language panels;
      - [x] Docker-layer gzip stage;
      - [x] WASM single-thread and worker scaling;
      - [x] Kodak PNG;
      - [x] Python ZIP write/read.
- [x] Ensure the paper and README use the same result set.

**Acceptance criterion:** no unexplained mismatch such as `cli.tsv` vs.
`benchmarks/core/results-*.md`; every caption identifies the aggregation rule.

## P1 — Native thread scaling (central “parallel” claim)

- [ ] Run native CLI compression and decompression with workers `{1, 2, 4, 5,
      8, 10}` on the 128 MiB and 1 GiB text corpora.
- [ ] Measure `pigz`, pigzpp zlib-ng, and pigzpp ISA-L separately.
- [ ] Plot throughput and parallel efficiency; distinguish physical cores from
      SMT-only worker counts.
- [ ] Add a native scaling figure next to the WASM scaling result.

**Acceptance criterion:** the report can state where each backend saturates and
quantify scaling efficiency rather than only reporting an 8-worker endpoint.

## P1 — Compression-level sensitivity

- [ ] Benchmark levels `{1, 6, 9}` for CLI and at least Python bytes API.
- [ ] Report throughput and output ratio/bytes together.
- [ ] Verify how levels map to ISA-L’s supported internal levels; avoid implying
      equal effort when backend level semantics differ.
- [ ] Generate a speed-vs-size plot or a three-panel level ablation.

**Acceptance criterion:** headline conclusions remain valid or are narrowed to
specific levels.

## P1 — Corpus diversity

- [ ] Use at least four deterministic corpora with checked-in generation or
      download metadata and SHA-256:
  - multilingual natural-language text;
  - source-code corpus;
  - structured/binary data;
  - incompressible or already-compressed data.
- [ ] Keep the real Docker layer and Kodak data as application-specific cases.
- [ ] Report both throughput and ratio for every corpus/backend pair.
- [ ] Plot a corpus × backend matrix or small multiples.

**Acceptance criterion:** “fastest” and ratio claims identify the data classes
for which they hold.

## P1 — ARM64 portable-path measurements

- [ ] Run native CLI, Python bytes, ZIP, and PNG on Linux ARM64 and/or macOS
      arm64 using zlib-ng.
- [ ] Record CPU model and SIMD path (NEON) selected at runtime.
- [ ] Compare portable zlib-ng behavior across x86-64 and ARM64 without mixing it
      with ISA-L.
- [ ] Add an ARM64 panel or compact cross-architecture figure.

**Acceptance criterion:** the “portable” title is supported by measured native
ARM64 throughput, not packaging alone.

## P1 — Decompression evaluation

- [ ] Re-run CLI and Python decompression with matching corpora and archives.
- [ ] Include `gzip`/zlib, pigz, zlib-ng, ISA-L/igzip where applicable, and
      pigzpp.
- [ ] Record which decoder produced each input stream to catch backend-dependent
      effects.
- [ ] Add one decompression figure; keep ZIP-read and PNG-decode as separate
      application-level results if included.

**Acceptance criterion:** the paper evaluates both halves of the advertised
compress/decompress stack.

## P1 — Binding overhead and small buffers

- [ ] Measure Python, Go, and Rust at buffer sizes `{64 KiB, 1 MiB, 16 MiB,
      128 MiB}` with the same core backend.
- [ ] Include direct C++/C-ABI calls as the baseline to isolate nanobind, cgo,
      and Rust FFI overhead.
- [ ] For Python, document GIL release and measure repeated small calls.
- [ ] For Go, include both copied output and owned/zero-copy output paths.
- [ ] Plot throughput or per-call latency vs. input size.

**Acceptance criterion:** language-binding claims distinguish core speed from
FFI and allocation overhead.

## P1 — Complete both-backend coverage

- [x] Extend the Go benchmark with explicit pigzpp `zlib` and `isal` methods;
      the current language figure lacks a Go zlib-ng-only data point.
- [x] Ensure CLI, Python, Go, Rust, Docker, ZIP, and PNG identify backend per bar.
- [x] Keep plot colors invariant: green = ISA-L, terracotta = zlib-ng, gray =
      competitors; use neutral colors where a series represents an operation
      rather than a backend.

**Acceptance criterion:** portable and x86-only results can be compared in each
native ecosystem where both are supported.

## P2 — PNG backend/filter ablation

- [ ] Benchmark filters `{none, up, adaptive-fast, adaptive-all}` × applicable
      backends/strategies on RGB, grayscale, and 1-bit mask data.
- [ ] Separate line-filter time from DEFLATE time.
- [ ] Quantify the known mask trade-off: ISA-L `fast` is faster but may produce
      materially larger files than zlib-ng RLE/filtered presets.
- [ ] Add a speed-vs-size plot only if it remains readable; otherwise keep the
      full result in benchmark documentation and summarize one finding in the
      report.

## P2 — ZIP parallelism ablation

- [ ] Measure one large member vs. many members and workers `{1, 2, 4, 8}`.
- [ ] Separate gains from backend choice and member-level parallelism.
- [ ] Include write throughput, read throughput, archive size, and Zip64 cases.

## P2 — Cross-platform validation

- [ ] Repeat portable zlib-ng benchmarks on Linux, macOS, and Windows for at
      least x86-64; add ARM64 where runners are available.
- [ ] Keep this separate from distribution-build success: packaging validates
      availability, not performance equivalence.

## Plot and paper regeneration checklist

After each accepted benchmark batch:

- [x] Generate/update `report/plots/data/*.tsv` from raw JSON.
- [x] Run `make -C report plots`.
- [x] Run `make -C report clean pdf` with no overfull/underfull boxes, undefined
      references, or LaTeX errors (the bundled monospace font emits known
      fallback-shape warnings).
- [x] Inspect all figure labels, slow→fast ordering, backend colors, and captions.
- [x] Update README benchmark summaries from the same generated data.
- [x] Recalculate every textual multiplier from source values (no hand-rounded
      stale claims).
- [x] Record the result-set date and commit SHA in the paper’s methodology.
