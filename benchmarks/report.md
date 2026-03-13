make bench-bin
build/bench_data/16MB.txt already exists (16M)
build/bench_data/128MB.txt already exists (129M)
build/bench_data/1024MB.txt already exists (1.1G)

Installing Python benchmark packages...
pip install --quiet zlib-ng isal pytest 2>/dev/null || \
        echo "Warning: some packages failed to install (optional)"
✓ Benchmark setup complete
python benchmarks/bench_binary.py --sizes 16 128 1024 --iterations 1 \
        --threads 1 4 --pigzpp build/pigzpp --data-dir build/bench_data
Binaries:
      gzip: /usr/bin/gzip -- gzip 1.10
      pigz: /usr/bin/pigz -- pigz 2.6
    pigzpp: build/pigzpp -- pigzpp 1.0.0 (based on pigz 2.8)
     igzip: /usr/bin/igzip -- igzip command line interface 2.30.0
Sizes: [16, 128, 1024] MB | Iterations: 1 | Threads: [1, 4]

=== Compression (MB/s, speedup vs gzip) ===

      Size            gzip            pigz          pigzpp           igzip
--------------------------------------------------------------------------
     16 MB      77 (1.0x)     398 (5.2x)     991 (12.8x)    1357 (17.6x)
    128 MB     118 (1.0x)     551 (4.7x)    1815 (15.4x)    2189 (18.6x)
   1024 MB     124 (1.0x)     461 (3.7x)    2034 (16.5x)    3174 (25.7x)

=== Decompression (MB/s, speedup vs gzip) ===

      Size            gzip            pigz          pigzpp           igzip
--------------------------------------------------------------------------
     16 MB     209 (1.0x)     189 (0.9x)    2343 (11.2x)    2749 (13.1x)
    128 MB     220 (1.0x)     199 (0.9x)    4514 (20.5x)    5578 (25.4x)
   1024 MB     202 (1.0x)     178 (0.9x)    4737 (23.5x)    4815 (23.9x)

=== Thread Scaling: Compression (MB/s) ===

      Size  threads          pigz        pigzpp     speedup
-----------------------------------------------------------
     16 MB        1        136 MB/s       1061 MB/s        7.8x
     16 MB        4        343 MB/s       1009 MB/s        2.9x
    128 MB        1        138 MB/s       1336 MB/s        9.7x
    128 MB        4        488 MB/s       2135 MB/s        4.4x
   1024 MB        1        136 MB/s       1395 MB/s       10.2x
   1024 MB        4        501 MB/s       2473 MB/s        4.9x

=== Thread Scaling: Decompression (MB/s) ===

      Size  threads          pigz        pigzpp     speedup
-----------------------------------------------------------
     16 MB        1        476 MB/s       2556 MB/s        5.4x
     16 MB        4        213 MB/s       2592 MB/s       12.2x
    128 MB        1        575 MB/s       4055 MB/s        7.1x
    128 MB        4        187 MB/s       4094 MB/s       21.9x
   1024 MB        1        508 MB/s       5029 MB/s        9.9x
   1024 MB        4        178 MB/s       4484 MB/s       25.2x