Changes:
  1. ISA-L is integrated
  2. Text dataset is replaced -- previously a small piece of text was duplicated to reach target size. Now I download multilingual data (English, and Chinese) to eval on real text data
  3. pigz is used as baseline (1x) instad of gzip. 


make bench-bin
python3 benchmarks/gen_data.py --sizes 16 128 1024 8192 --data-dir build/bench_data
Downloading English (Wikitext-103) ...
  150/150 MB (100%)
  → 582510 text fragments
Downloading Chinese (Wikipedia) ...
  560/560 MB (100%)
  → 230792 text fragments
Cached multilingual seed: build/bench_data/multilingual_seed.txt (1094.8 MB, 813302 fragments)
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
python benchmarks/bench_binary.py --sizes 16 128 1024 8192 --iterations 3 \
        --threads 1 4 8 16 --pigzpp build/pigzpp --data-dir build/bench_data
Binaries:
      pigz: /usr/bin/pigz -- pigz 2.6
      gzip: /usr/bin/gzip -- gzip 1.10
     igzip: /usr/bin/igzip -- igzip command line interface 2.30.0
    pigzpp: build/pigzpp -- pigzpp 1.0.0 (based on pigz 2.8)
Sizes: [16, 128, 1024, 8192] MB | Iterations: 3 | Threads: [1, 4, 8, 16]

=== Compression — Text Data (MB/s, speedup vs pigz) ===

      Size            pigz            gzip           igzip          pigzpp
--------------------------------------------------------------------------
     16 MB     381 (1.0x)      15 (0.0x)     226 (0.6x)     801 (2.1x)
    128 MB     573 (1.0x)      15 (0.0x)     251 (0.4x)    1877 (3.3x)
   1024 MB     644 (1.0x)               —     238 (0.4x)    2441 (3.8x)
   8192 MB     637 (1.0x)               —     241 (0.4x)    3351 (5.3x)

=== Decompression — Text Data (MB/s, speedup vs pigz) ===

      Size            pigz            gzip           igzip          pigzpp
--------------------------------------------------------------------------
     16 MB     214 (1.0x)     147 (0.7x)     518 (2.4x)     528 (2.5x)
    128 MB     230 (1.0x)     150 (0.7x)     621 (2.7x)     603 (2.6x)
   1024 MB     204 (1.0x)               —     471 (2.3x)     452 (2.2x)
   8192 MB     208 (1.0x)               —     485 (2.3x)     453 (2.2x)

=== Compression — Random Data (MB/s, speedup vs pigz) ===

      Size            pigz            gzip           igzip          pigzpp
--------------------------------------------------------------------------
     16 MB     538 (1.0x)      32 (0.1x)     617 (1.1x)    1014 (1.9x)
    128 MB     901 (1.0x)      32 (0.0x)     770 (0.9x)    2143 (2.4x)
   1024 MB    1002 (1.0x)               —     795 (0.8x)    2494 (2.5x)
   8192 MB     998 (1.0x)               —     791 (0.8x)    3323 (3.3x)

=== Decompression — Random Data (MB/s, speedup vs pigz) ===

      Size            pigz            gzip           igzip          pigzpp
--------------------------------------------------------------------------
     16 MB     557 (1.0x)     203 (0.4x)    1900 (3.4x)    1852 (3.3x)
    128 MB     634 (1.0x)     208 (0.3x)    2438 (3.8x)    2241 (3.5x)
   1024 MB     611 (1.0x)               —    3079 (5.0x)    4581 (7.5x)
   8192 MB     636 (1.0x)               —    3344 (5.3x)    2890 (4.5x)

=== Thread Scaling: Compression — Text Data (MB/s) ===

      Size  threads          pigz        pigzpp     speedup
-----------------------------------------------------------
     16 MB        1         17 MB/s        224 MB/s       13.3x
     16 MB        4         65 MB/s        604 MB/s        9.2x
     16 MB        8        126 MB/s        905 MB/s        7.2x
     16 MB       16        227 MB/s       1003 MB/s        4.4x
    128 MB        1         17 MB/s        236 MB/s       14.0x
    128 MB        4         67 MB/s        880 MB/s       13.1x
    128 MB        8        134 MB/s       1412 MB/s       10.6x
    128 MB       16        264 MB/s       1524 MB/s        5.8x
   1024 MB        1         19 MB/s        230 MB/s       12.2x
   1024 MB        4         75 MB/s        927 MB/s       12.4x
   1024 MB        8        150 MB/s       1803 MB/s       12.1x
   1024 MB       16        298 MB/s       2449 MB/s        8.2x
   8192 MB        1         19 MB/s        230 MB/s       12.2x
   8192 MB        4         75 MB/s        945 MB/s       12.6x
   8192 MB        8        150 MB/s       1874 MB/s       12.5x
   8192 MB       16        300 MB/s       3161 MB/s       10.5x

=== Thread Scaling: Decompression — Text Data (MB/s) ===

      Size  threads          pigz        pigzpp     speedup
-----------------------------------------------------------
     16 MB        1        194 MB/s        553 MB/s        2.8x
     16 MB        4        215 MB/s        547 MB/s        2.5x
     16 MB        8        211 MB/s        555 MB/s        2.6x
     16 MB       16        218 MB/s        537 MB/s        2.5x
    128 MB        1        200 MB/s        605 MB/s        3.0x
    128 MB        4        225 MB/s        639 MB/s        2.8x
    128 MB        8        229 MB/s        643 MB/s        2.8x
    128 MB       16        230 MB/s        638 MB/s        2.8x
   1024 MB        1        184 MB/s        455 MB/s        2.5x
   1024 MB        4        206 MB/s        463 MB/s        2.2x
   1024 MB        8        207 MB/s        451 MB/s        2.2x
   1024 MB       16        206 MB/s        452 MB/s        2.2x
   8192 MB        1        185 MB/s        460 MB/s        2.5x
   8192 MB        4        201 MB/s        456 MB/s        2.3x
   8192 MB        8        201 MB/s        455 MB/s        2.3x
   8192 MB       16        207 MB/s        453 MB/s        2.2x

=== Thread Scaling: Compression — Random Data (MB/s) ===

      Size  threads          pigz        pigzpp     speedup
-----------------------------------------------------------
     16 MB        1         32 MB/s        591 MB/s       18.3x
     16 MB        4        123 MB/s        989 MB/s        8.1x
     16 MB        8        228 MB/s       1236 MB/s        5.4x
     16 MB       16        397 MB/s       1342 MB/s        3.4x
    128 MB        1         33 MB/s        666 MB/s       20.5x
    128 MB        4        128 MB/s       2140 MB/s       16.7x
    128 MB        8        254 MB/s       1987 MB/s        7.8x
    128 MB       16        495 MB/s       1939 MB/s        3.9x
   1024 MB        1         33 MB/s        674 MB/s       20.7x
   1024 MB        4        129 MB/s       2617 MB/s       20.3x
   1024 MB        8        257 MB/s       3149 MB/s       12.2x
   1024 MB       16        513 MB/s       2679 MB/s        5.2x
   8192 MB        1         33 MB/s        714 MB/s       21.9x
   8192 MB        4        129 MB/s       3420 MB/s       26.5x
   8192 MB        8        258 MB/s       3272 MB/s       12.7x
   8192 MB       16        515 MB/s       3410 MB/s        6.6x

=== Thread Scaling: Decompression — Random Data (MB/s) ===

      Size  threads          pigz        pigzpp     speedup
-----------------------------------------------------------
     16 MB        1        727 MB/s       1876 MB/s        2.6x
     16 MB        4        566 MB/s       1910 MB/s        3.4x
     16 MB        8        556 MB/s       2018 MB/s        3.6x
     16 MB       16        557 MB/s       1935 MB/s        3.5x
    128 MB        1        854 MB/s       2515 MB/s        2.9x
    128 MB        4        627 MB/s       2928 MB/s        4.7x
    128 MB        8        566 MB/s       2050 MB/s        3.6x
    128 MB       16        624 MB/s       2811 MB/s        4.5x
   1024 MB        1        896 MB/s       3294 MB/s        3.7x
   1024 MB        4        642 MB/s       3169 MB/s        4.9x
   1024 MB        8        607 MB/s       3145 MB/s        5.2x
   1024 MB       16        637 MB/s       3114 MB/s        4.9x
   8192 MB        1        941 MB/s       3826 MB/s        4.1x
   8192 MB        4        549 MB/s       2994 MB/s        5.5x
   8192 MB        8        645 MB/s       2528 MB/s        3.9x
   8192 MB       16        634 MB/s       2822 MB/s        4.5x