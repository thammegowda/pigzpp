$ make bench-bin 
build/bench_data/16MB.txt already exists (16M)
build/bench_data/128MB.txt already exists (129M)
build/bench_data/1024MB.txt already exists (1.1G)

Installing Python benchmark packages...
pip install --quiet zlib-ng isal pytest 2>/dev/null || \
        echo "Warning: some packages failed to install (optional)"
✓ Benchmark setup complete
python benchmarks/bench_binary.py --sizes 16 128 1024 --iterations 3 \
        --threads 1 4 16 --pigzpp build/pigzpp --data-dir build/bench_data
Binaries:
      gzip: /usr/bin/gzip -- gzip 1.12
      pigz: /usr/bin/pigz -- pigz 2.8
    pigzpp: build/pigzpp -- pigzpp 1.0.0 (based on pigz 2.8)
     igzip: /usr/bin/igzip -- igzip command line interface 2.31.0
Sizes: [16, 128, 1024] MB | Iterations: 3 | Threads: [1, 4, 16]

=== Compression (MB/s, speedup vs gzip) ===

      Size            gzip            pigz          pigzpp           igzip
--------------------------------------------------------------------------
     16 MB     145 (1.0x)     772 (5.3x)     954 (6.6x)    1824 (12.6x)
    128 MB     189 (1.0x)    1597 (8.5x)    2487 (13.2x)    2542 (13.5x)
   1024 MB     199 (1.0x)    1862 (9.4x)    3365 (16.9x)    2989 (15.0x)

=== Decompression (MB/s, speedup vs gzip) ===

      Size            gzip            pigz          pigzpp           igzip
--------------------------------------------------------------------------
     16 MB     219 (1.0x)     798 (3.6x)    1318 (6.0x)    2107 (9.6x)
    128 MB     299 (1.0x)     965 (3.2x)    2132 (7.1x)    4568 (15.3x)
   1024 MB     301 (1.0x)    1112 (3.7x)    2658 (8.8x)    5325 (17.7x)

=== Thread Scaling: Compression (MB/s) ===

      Size  threads          pigz        pigzpp     speedup
-----------------------------------------------------------
     16 MB        1        168 MB/s        812 MB/s        4.8x
     16 MB        4        445 MB/s       1561 MB/s        3.5x
     16 MB       16        970 MB/s       1522 MB/s        1.6x
    128 MB        1        176 MB/s       1011 MB/s        5.7x
    128 MB        4        535 MB/s       2628 MB/s        4.9x
    128 MB       16       1797 MB/s       2695 MB/s        1.5x


make bench-py
build/bench_data/16MB.txt already exists (16M)
build/bench_data/128MB.txt already exists (129M)
build/bench_data/1024MB.txt already exists (1.1G)

Installing Python benchmark packages...
pip install --quiet zlib-ng isal pytest 2>/dev/null || \
        echo "Warning: some packages failed to install (optional)"
✓ Benchmark setup complete
PYTHONPATH=/mnt/home/tg/work/repos/me/pigz-claude/pigzpp/build python benchmarks/bench_python.py --sizes 16 128 1024 --iterations 3
pigzpp Python benchmark
Python: 3.14.3
Libraries:
  gzip:    stdlib (zlib 1.3.1)
  pigz_sp: subprocess (/usr/bin/pigz)
  zlib_ng: 1.0.0
  pigzpp:  installed
  isal:    1.8.0
Sizes: [16, 128, 1024] MB | Iterations: 3 | CPUs: 48

=== File API: Compression (MB/s, xGzip) ===

      Size         gzip      pigz_sp      zlib_ng       pigzpp         isal
-----------------------------------------------------------------------------
     16 MB   150 (1.0x)   497 (3.3x)   820 (5.5x)  1033 (6.9x)  2313 (15.4x)
    128 MB   151 (1.0x)   465 (3.1x)   530 (3.5x)   748 (5.0x)  1072 (7.1x)
   1024 MB   150 (1.0x)   497 (3.3x)   705 (4.7x)   749 (5.0x)  1078 (7.2x)

=== File API: Decompression (MB/s, xGzip) ===

      Size         gzip      pigz_sp      zlib_ng       pigzpp         isal
-----------------------------------------------------------------------------
     16 MB   359 (1.0x)   222 (0.6x)   660 (1.8x)   444 (1.2x)   666 (1.9x)
    128 MB   322 (1.0x)   231 (0.7x)   448 (1.4x)   444 (1.4x)   441 (1.4x)
   1024 MB   327 (1.0x)   238 (0.7x)   557 (1.7x)   483 (1.5x)   563 (1.7x)

=== Bytes API: Compression (MB/s, xGzip) ===

      Size         gzip      zlib_ng       pigzpp         isal
----------------------------------------------------------------
     16 MB   173 (1.0x)   468 (2.7x)  1889 (10.9x)   215 (1.2x)
    128 MB   168 (1.0x)   459 (2.7x)  1805 (10.7x)   213 (1.3x)
   1024 MB   170 (1.0x)   458 (2.7x)  1787 (10.5x)   213 (1.2x)

=== Bytes API: Decompression (MB/s, xGzip) ===

      Size         gzip      zlib_ng       pigzpp         isal
----------------------------------------------------------------
     16 MB  1092 (1.0x)  2937 (2.7x)  4982 (4.6x)  2946 (2.7x)
    128 MB  1058 (1.0x)  2431 (2.3x)  4184 (4.0x)  2461 (2.3x)
   1024 MB   584 (1.0x)  1063 (1.8x)  1340 (2.3x)  1071 (1.8x)