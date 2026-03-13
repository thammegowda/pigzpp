# I Let Two AI Agents Race to Modernize pigz

Compression is everywhere -- from PNG/JPG images to reducing network bandwidth for faster transfers. It's also one of those fascinating areas where you learn about trade-offs: you can find a method that's great at reducing size but painfully slow (gzip level 9), or one that's "okay" but fast (e.g. Gzip level 3), or a trade-off (e.g. Gzip level 6)

## The Itch

Ever since I started working on machine translation, our datasets have been kept compressed. Initially I just used gzip/gunzip/zcat directly or via tar. But the datasets kept growing, and gzip is single-threaded.

Then I found [pigz](https://zlib.net/pigz/) -- Parallel Implementation of GZip. It uses multithreading to parallelize compression. Over the past few years I've compressed GBs to TBs of datasets with pigz at work. What takes hours to days with gzip can be reduced to minutes on a beefy server with 40+ cores. pigz is great -- easy to install, clean CLI, just pipe data in and out.

But then enter Python. I build data orchestration frameworks in Python (e.g., [mtdata](https://github.com/thammegowda/mtdata)). Python's stdlib `gzip` module is convenient but painfully slow at scale. There are many Python libs promising faster gzip, but figuring out which ones are actually compatible with pigz/gzip output is its own headache. My go-to has been [subprocess pigz](https://github.com/thammegowda/mtdata/blob/main/mtdata/pigz.py) -- just fork the process and stream I/O. It works, but there's overhead from process forking and kernel-level copies of data through STDIN/STDOUT.

I learned how to bind Python APIs to C/C++ code while working on [Pymarian](https://pypi.org/project/pymarian/). Naturally I thought -- why not do the same for pigz? But pigz was written as a standalone CLI tool. It's not thread-safe. It has one single global context (`struct g`, ~60 mutable fields) for all state, so having a pigz API call for reading and another for writing in the same process is asking for trouble. I looked at refactoring it into a library... and gave up. Too much work for my brain.

## The Agent Benchmark

I keep an eye on coding agents. I refer to benchmarks like Terminal Bench and SWE-bench to assess which one is better, but whenever people claim agents will automate coding forever, I want to test it on a *real* task -- something I actually need done.

Modernizing pigz became my go-to litmus test. The task is well-defined and easy to verify:
1. Must produce compatible outputs (compress with pigz, decompress with pigzpp, or vice versa)
2. Performance must be at least as good as original pigz

I asked agents to rewrite pigz in C++23, using modern language features -- threads, RAII, exceptions -- and restructure it as a thread-safe library.

**2024 (GPT/Codex):** Nowhere close. I tried, failed, wasted hours. [Commits here.](https://github.com/thammegowda/pigz/commits/master/)

**2025:** Agents could rewrite pigz, but performance was degraded. They couldn't fix the regression -- still too much work.

**2026:** A different story entirely.

## The Race

This weekend I launched two parallel Copilot agent sessions in two VS Code workspaces -- one with Claude Opus 4.6, and the other with GPT 5.4 -- and let them loose on the same [requirements document](./01-requirements.md).

Both got the same goals: rewrite pigz in C++23 as a thread-safe library called `pigzpp`, with at least performance parity and full gzip compatibility.

I shared the requirements, let them plan, clarified their questions, and then let them implement.

**How they planned it:**

Both agents read the full pigz source (~5,200 lines of C) and produced a detailed plan before writing code. The plans were architecturally similar -- both proposed class-based APIs, `std::jthread` replacing `yarn.c`, exceptions replacing `try.c`'s `setjmp`/`longjmp`, and a staged approach. But they diverged sharply on strategy:

- **Claude** planned a **clean-room rewrite**: new `pigzpp/` directory, build everything from scratch, the original pigz stays as a reference for benchmarking only. Direct, bold.
- **GPT** planned an **incremental migration**: keep the existing C engine buildable as `pigz-ref`, wrap it in a C++ "facade" with a process-local mutex, and replace internals piece by piece. Conservative, risk-averse.

**How they executed it:**

- **Claude** went mostly autopilot. Created the directory structure and started writing `Config`, `BufferPool`, `Compressor`, `Decompressor`, CRC utilities, format helpers, CLI -- the whole stack from scratch. I only had to nudge it **2 times** during the initial implementation. It produced a genuinely new C++23 codebase with no dependency on the original C code.

- **GPT** kept pausing to announce what it would do next and wait for approval. I had to say "continue" or "yes, go ahead" **16 times** during the session. Its first milestone was a "bootstrap bridge" -- a C++ wrapper that called `pigz_call()` (the original C entry point) behind a mutex. The `pigzpp` binary existed, but the actual compression/decompression still ran through the legacy C code. It called this a "migration scaffold" and kept the real rewrite as future work.

**After ~70 minutes, both delivered working binaries.** Both passed compatibility tests -- compress with pigzpp, decompress with gzip, and vice versa.

**But the performance story was different than it first appeared:**

| | Compression (L1) | Compression (L6) | Compression (L9) | Decompression |
|---|---|---|---|---|
| **Claude** | **34% faster** | ~equal | ~7% slower | ~43% slower |
| **GPT** | ±5% | ±5% | ±5% | ±5% |

Claude's rewrite was a genuinely new implementation -- faster compression at low levels (new code, no legacy overhead), but decompression was initially 43% slower because it hadn't yet implemented parallel I/O threads for inflate. Real gains, real gaps.

GPT's ±5% across the board looks like "parity," but it's misleading. Its `pigzpp` binary was routing through the original C `pigz_call()` behind a facade. It was benchmarking the *old engine through a new wrapper*, not a new implementation. Parity was guaranteed by construction.

**Code quality:**

Claude's output was a proper library: `Config` value type (no globals), `BufferPool` with RAII, `Compressor`/`Decompressor` classes, separate CLI. You could `#include <pigzpp/compress.h>` and use it in another program.

GPT's output was build infrastructure + test scaffolding + a thin C++ API that called the legacy C code. Good engineering practice for a migration -- but not what I asked for. The actual rewrite was still TODO.

I picked Claude's version to push further.

I have captured the chats with Claude and GPT:
* [My Chat with Claude](./02-claude-chat.md)
* [My Chat with GPT](./02-gpt-chat.md)


Look for `User:` substring for user inputs. Note that the chat history is sanitized to remove private info such as local paths and URIs, so it is not 100% complete.


## Beating pigz

I asked: *"We've matched pigz. Now can you beat it?"*

The agent identified two key optimizations:
1. **Eliminating unnecessary memory copies** in the compression pipeline
2. **Replacing zlib with [zlib-ng](https://github.com/zlib-ng/zlib-ng)**, which exploits SIMD instructions on modern CPUs

**Result: up to 1.8x faster compression, 2.4x faster decompression** vs original pigz (at default thread count). At single-thread, the gap is even wider -- 5.7x faster compression thanks to zlib-ng's SIMD-optimized DEFLATE.

The final pigzpp:
- **3,744 lines of C++23** (vs. ~5,200 lines of C in the original)
- **35 GoogleTest cases** (original had zero automated tests)
- **Zero global mutable state** -- fully thread-safe, multiple simultaneous compress/decompress operations in one process
- Replaces hand-rolled pthreads with `std::jthread`, `setjmp`/`longjmp` with proper exceptions, manual ref-counting with `shared_ptr` + RAII

### CLI Benchmarks

**Environment:** 48-core Intel Xeon, Ubuntu 24.04.3 LTS. All tools use default thread counts.

**Tools tested:**
- **gzip 1.12** -- the classic single-threaded compressor, our baseline
- **pigz 2.8** -- Mark Adler's parallel gzip (uses zlib, multithreaded compression)
- **pigzpp 1.0.0** -- our C++23 rewrite of pigz (uses zlib-ng, multithreaded compression)
- **igzip 2.31.0** -- Intel ISA-L's native CLI ([`apt install isal`](https://github.com/intel/isa-l)), using hand-tuned x86 assembly for DEFLATE

All produce gzip-compatible output. Throughput in MB/s, speedup relative to gzip:

**Compression:**

| Size | gzip | pigz | pigzpp | igzip |
|---|---|---|---|---|
| 16 MB | 145 (1.0x) | 772 (5.3x) | 954 (6.6x) | **1824 (12.6x)** |
| 128 MB | 189 (1.0x) | 1597 (8.5x) | **2487 (13.2x)** | 2542 (13.5x) |
| 1024 MB | 199 (1.0x) | 1862 (9.4x) | **3365 (16.9x)** | 2989 (15.0x) |

**Decompression:**

| Size | gzip | pigz | pigzpp | igzip |
|---|---|---|---|---|
| 16 MB | 219 (1.0x) | 798 (3.6x) | 1318 (6.0x) | **2107 (9.6x)** |
| 128 MB | 299 (1.0x) | 965 (3.2x) | 2132 (7.1x) | **4568 (15.3x)** |
| 1024 MB | 301 (1.0x) | 1112 (3.7x) | 2658 (8.8x) | **5325 (17.7x)** |

**Analysis:** pigzpp and igzip are both dramatically faster than pigz. For **compression**, they're neck-and-neck at large sizes -- pigzpp edges ahead at 1 GB (16.9x vs 15.0x) thanks to multithreaded parallel compression, while igzip leads at 16 MB where its raw single-core speed dominates before threading overhead amortizes. For **decompression**, igzip wins decisively (up to 17.7x) -- Intel's hand-written assembly for inflate is hard to beat. DEFLATE decompression is inherently sequential, so pigzpp's multithreading advantage doesn't help here; its gains come purely from zlib-ng's SIMD optimizations.

**Thread scaling (pigz vs pigzpp):**

| Size | Threads | pigz | pigzpp | Speedup |
|---|---|---|---|---|
| 16 MB | 1 | 168 MB/s | 812 MB/s | **4.8x** |
| 16 MB | 4 | 445 MB/s | 1561 MB/s | **3.5x** |
| 16 MB | 16 | 970 MB/s | 1522 MB/s | **1.6x** |
| 128 MB | 1 | 176 MB/s | 1011 MB/s | **5.7x** |
| 128 MB | 4 | 535 MB/s | 2628 MB/s | **4.9x** |
| 128 MB | 16 | 1797 MB/s | 2695 MB/s | **1.5x** |

At single-thread, pigzpp is **4.8-5.7x faster** than pigz -- this is pure zlib-ng vs zlib. As thread count increases, pigz narrows the gap but never catches up.

## Python Bindings

I then had the agent build Python bindings via pybind11. Unlike my old approach of forking a subprocess, pigzpp calls the C++ library directly -- zero fork/exec overhead.

I benchmarked against every relevant Python compression library. All speedups below are relative to stdlib gzip (1.0x baseline).

**Libraries tested:**
- **gzip** -- Python stdlib, backed by system zlib (1.3.1 on this machine)
- **pigz (subprocess)** -- forking the `pigz` binary via `subprocess.Popen`, my old workaround
- **zlib-ng** (`pip install zlib-ng`) -- Python bindings for SIMD-optimized zlib-ng, single-threaded
- **pigzpp** -- our pybind11 bindings, calling the C++ library in-process with parallel compression
- **isal** (`pip install isal`) -- Python bindings for Intel ISA-L, single-threaded but with hand-tuned assembly

**Benchmarks on a 48-core Xeon, Ubuntu 24.04.3 LTS, Python 3.14.3** (verified identical on 3.13.11):

*Note: Python 3.14 bundles zlib-ng for Windows/macOS binary releases ([cpython#91349](https://github.com/python/cpython/issues/91349)), which should make stdlib `gzip` significantly faster on those platforms. On Linux, both 3.13 and 3.14 use the system zlib, so results are identical.*

### File API (MB/s, speedup vs gzip)

**Compression:**

| Size | gzip | pigz (subprocess) | zlib-ng | pigzpp | isal |
|---|---|---|---|---|---|
| 16 MB | 150 (1.0x) | 497 (3.3x) | 820 (5.5x) | 1033 (6.9x) | **2313 (15.4x)** |
| 128 MB | 151 (1.0x) | 465 (3.1x) | 530 (3.5x) | 748 (5.0x) | **1072 (7.1x)** |
| 1024 MB | 150 (1.0x) | 497 (3.3x) | 705 (4.7x) | 749 (5.0x) | **1078 (7.2x)** |

**Decompression:**

| Size | gzip | pigz (subprocess) | zlib-ng | pigzpp | isal |
|---|---|---|---|---|---|
| 16 MB | 359 (1.0x) | 222 (0.6x) | **660 (1.8x)** | 444 (1.2x) | 666 (1.9x) |
| 128 MB | 322 (1.0x) | 231 (0.7x) | 448 (1.4x) | 444 (1.4x) | **441 (1.4x)** |
| 1024 MB | 327 (1.0x) | 238 (0.7x) | 557 (1.7x) | 483 (1.5x) | **563 (1.7x)** |

### Bytes API (MB/s, speedup vs gzip)

**Compression:**

| Size | gzip | zlib-ng | pigzpp | isal |
|---|---|---|---|---|
| 16 MB | 173 (1.0x) | 468 (2.7x) | **1889 (10.9x)** | 215 (1.2x) |
| 128 MB | 168 (1.0x) | 459 (2.7x) | **1805 (10.7x)** | 213 (1.3x) |
| 1024 MB | 170 (1.0x) | 458 (2.7x) | **1787 (10.5x)** | 213 (1.2x) |

**Decompression:**

| Size | gzip | zlib-ng | pigzpp | isal |
|---|---|---|---|---|
| 16 MB | 1092 (1.0x) | 2937 (2.7x) | **4982 (4.6x)** | 2946 (2.7x) |
| 128 MB | 1058 (1.0x) | 2431 (2.3x) | **4184 (4.0x)** | 2461 (2.3x) |
| 1024 MB | 584 (1.0x) | 1063 (1.8x) | **1340 (2.3x)** | 1071 (1.8x) |

**Analysis:** The two APIs tell very different stories.

For the **file API**, each library writes/reads one file at a time. ISA-L dominates compression (up to 15.4x) because its hand-tuned assembly is simply the fastest single-stream DEFLATE implementation available. pigzpp's multithreading helps less here because the file API triggers one background thread per open file. For decompression, zlib-ng and isal converge around 1.4-1.9x -- DEFLATE inflate is sequential, so all single-threaded SIMD libraries perform similarly.

For the **bytes API**, pigzpp pulls far ahead on compression (10-11x) because it fires up all 48 cores to compress the in-memory buffer in parallel chunks. zlib-ng and isal are limited to single-threaded compression in their Python bindings, so despite having faster per-core DEFLATE, they can't compete with 48 cores working in parallel. Same story for decompression: pigzpp at 4.6x vs isal/zlib-ng at 2.7x.

Full interoperability verified: `pigzpp.open()` ↔ `gzip.open()`, `pigzpp.compress()` ↔ `gzip.decompress()`, and vice versa.

## Lessons Learned

**Agents can modernize real tools -- and make them faster.** This isn't a toy demo. pigz is a widely-used utility written by Mark Adler, co-creator of zlib. A coding agent rewrote it from ~scratch~ in 70 minutes, matched performance, then beat it by 2x with targeted optimizations. That's remarkable.

**The Python ecosystem has quietly gotten fast.** Python 3.14 will bundle zlib-ng in stdlib for Windows/macOS ([cpython#91349](https://github.com/python/cpython/issues/91349)). ISA-L is even faster on Intel CPUs -- its `igzip` CLI hits 5.3 GB/s decompression, and the Python `isal` library dominates single-threaded file compression. The case for pigzpp Python bindings is strongest for the bytes API where multithreading gives a 10x advantage -- but for file-at-a-time use, isal is the better choice. The CLI pigzpp remains useful as a faster drop-in replacement for pigz.

**Agent style matters.** Claude's autopilot approach -- just doing the work without asking permission at every step -- was noticeably more productive than GPT's pause-and-announce style. For a task with clear requirements, I want an agent that executes, not one that narrates.

**Well-defined tasks make agents shine.** The key was having clear, verifiable success criteria: compatibility and performance. If I couldn't objectively measure whether the output was correct and fast, I wouldn't have trusted either agent's result.

---

*pigzpp is open source: [github.com/thammegowda/pigzpp](https://github.com/thammegowda/pigzpp)*
