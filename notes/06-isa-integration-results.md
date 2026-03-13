make bench-bin
python3 benchmarks/gen_data.py --sizes 16 128 1024 8192 --data-dir build/bench_data
Generating 16MB text data → build/bench_data/16MB.txt
Generating 16MB random data → build/bench_data/16MB.bin
Generating 128MB text data → build/bench_data/128MB.txt
Generating 128MB random data → build/bench_data/128MB.bin
Generating 1024MB text data → build/bench_data/1024MB.txt
Generating 1024MB random data → build/bench_data/1024MB.bin
Generating 8192MB text data → build/bench_data/8192MB.txt
Generating 8192MB random data → build/bench_data/8192MB.bin
✓ Data generation complete

Installing Python benchmark packages...
pip install --quiet zlib-ng isal pytest 2>/dev/null || \
        echo "Warning: some packages failed to install (optional)"
✓ Benchmark setup complete
python benchmarks/bench_binary.py --sizes 16 128 1024 8192 --iterations 1 \
        --threads 1 4 --pigzpp build/pigzpp --data-dir build/bench_data
Binaries:
      pigz: /usr/bin/pigz -- pigz 2.6
      gzip: /usr/bin/gzip -- gzip 1.10
     igzip: /usr/bin/igzip -- igzip command line interface 2.30.0
    pigzpp: build/pigzpp -- pigzpp 1.0.0 (based on pigz 2.8)
Sizes: [16, 128, 1024, 8192] MB | Iterations: 1 | Threads: [1, 4]

=== Compression — Text Data (MB/s, speedup vs pigz) ===

      Size            pigz            gzip           igzip          pigzpp
--------------------------------------------------------------------------
     16 MB     485 (1.0x)     143 (0.3x)    1596 (3.3x)    1842 (3.8x)
    128 MB     909 (1.0x)     211 (0.2x)    4698 (5.2x)    5687 (6.3x)
   1024 MB    1000 (1.0x)               —    5648 (5.6x)    7680 (7.7x)
   8192 MB     850 (1.0x)               —    5692 (6.7x)    7935 (9.3x)

=== Decompression — Text Data (MB/s, speedup vs pigz) ===

      Size            pigz            gzip           igzip          pigzpp
--------------------------------------------------------------------------
     16 MB     315 (1.0x)     266 (0.8x)    2170 (6.9x)    1589 (5.0x)
    128 MB     387 (1.0x)     373 (1.0x)    7439 (19.2x)    5719 (14.8x)
   1024 MB     374 (1.0x)               —    9914 (26.5x)    5706 (15.2x)
   8192 MB     394 (1.0x)               —   10339 (26.2x)    5763 (14.6x)

=== Compression — Random Data (MB/s, speedup vs pigz) ===

      Size            pigz            gzip           igzip          pigzpp
--------------------------------------------------------------------------
     16 MB     165 (1.0x)      41 (0.3x)     824 (5.0x)    1279 (7.8x)
    128 MB     143 (1.0x)      38 (0.3x)    1132 (7.9x)    2613 (18.3x)
   1024 MB     172 (1.0x)               —     925 (5.4x)    3845 (22.4x)
   8192 MB     154 (1.0x)               —    1124 (7.3x)    3472 (22.5x)

=== Decompression — Random Data (MB/s, speedup vs pigz) ===

      Size            pigz            gzip           igzip          pigzpp
--------------------------------------------------------------------------
     16 MB     381 (1.0x)     224 (0.6x)    2169 (5.7x)    2686 (7.0x)
    128 MB     394 (1.0x)     252 (0.6x)    4651 (11.8x)    5295 (13.5x)
   1024 MB     378 (1.0x)               —    5326 (14.1x)    4789 (12.7x)
   8192 MB     382 (1.0x)               —    5534 (14.5x)    6288 (16.5x)

=== Thread Scaling: Compression — Text Data (MB/s) ===

      Size  threads          pigz        pigzpp     speedup
-----------------------------------------------------------
     16 MB        1        268 MB/s       2463 MB/s        9.2x
     16 MB        4        667 MB/s       2014 MB/s        3.0x
    128 MB        1        287 MB/s       3700 MB/s       12.9x
    128 MB        4        737 MB/s       3805 MB/s        5.2x
   1024 MB        1        266 MB/s       4457 MB/s       16.8x
   1024 MB        4        662 MB/s       6170 MB/s        9.3x
   8192 MB        1        255 MB/s       5668 MB/s       22.2x
   8192 MB        4        798 MB/s       6146 MB/s        7.7x

=== Thread Scaling: Decompression — Text Data (MB/s) ===

      Size  threads          pigz        pigzpp     speedup
-----------------------------------------------------------
     16 MB        1        851 MB/s       4036 MB/s        4.7x
     16 MB        4        343 MB/s       3657 MB/s       10.7x
    128 MB        1        836 MB/s       6210 MB/s        7.4x
    128 MB        4        388 MB/s       4531 MB/s       11.7x
   1024 MB        1        927 MB/s       7124 MB/s        7.7x
   1024 MB        4        393 MB/s       5484 MB/s       14.0x
   8192 MB        1        974 MB/s       7100 MB/s        7.3x
   8192 MB        4        381 MB/s       5536 MB/s       14.5x

=== Thread Scaling: Compression — Random Data (MB/s) ===

      Size  threads          pigz        pigzpp     speedup
-----------------------------------------------------------
     16 MB        1         39 MB/s        847 MB/s       21.7x
     16 MB        4        133 MB/s       1560 MB/s       11.7x
    128 MB        1         40 MB/s       1073 MB/s       26.6x
    128 MB        4        138 MB/s       2824 MB/s       20.5x
   1024 MB        1         37 MB/s       1078 MB/s       29.2x
   1024 MB        4        135 MB/s       3255 MB/s       24.2x
....