# Python ZIP Benchmark Results - 2026-07-13

Hardware: Intel Xeon W-2235 (5 cores / 10 threads, 3.80 GHz), Ubuntu 22.04.5 (WSL2).

`pigzpp.ZipFile` vs the standard library's `zipfile`, both writing real DEFLATE
ZIP archives to a temp file and reading them back. Corpus: the shared 128 MB
multilingual-text set (`build/bench_data/128MB.txt`). Level 6, 8 threads,
best-of-3. Throughput is over the uncompressed input; `ratio` is input/output.

Reproduce:

```bash
python benchmarks/python/bench_zip.py --sizes 128 --members 1,16 --threads 8
```

## Single large member (128 MB)

| writer | write MB/s | ratio | read MB/s |
|---|---:|---:|---:|
| zipfile (stdlib) | 16.9 | 2.827 | 208.5 |
| pigzpp:isal | 405.0 | 2.580 | 349.8 |
| pigzpp:zlib | 220.1 | 2.811 | 375.6 |

## 16 members (~8 MB each)

| writer | write MB/s | ratio | read MB/s |
|---|---:|---:|---:|
| zipfile (stdlib) | 17.7 | 2.826 | 243.4 |
| pigzpp:isal | 554.0 | 2.579 | 474.0 |
| pigzpp:zlib | 218.7 | 2.810 | 532.7 |

Notes:
- `pigzpp:zlib` matches the stdlib ratio (2.81) while writing **~13x faster** and
  reading ~2x faster.
- `pigzpp:isal` writes **24-31x faster** than the stdlib, trading ~9% ratio for speed.
- pigzpp parallelizes each member's DEFLATE across threads; the stdlib is
  single-threaded, so the gap widens with larger members.
