# PNG Benchmark Results - 2026-05-08

Fresh editable reinstall before benchmark:

```text
Successfully uninstalled pigzpp-1.0.0
Successfully installed pigzpp-1.0.0
pigzpp module: /opt/venv/lib/python3.12/site-packages/pigzpp.cpython-312-x86_64-linux-gnu.so
numpy: 2.3.5
has decompress_array: True
```

Benchmark command pattern:

```bash
python benchmarks/bench_png.py \
  --image-dir images-dir \
  --limit 200 \
  --loops 3 \
  --mode <rgb|gray|mask> \
  --verify \
  --out build/png-bench-fresh-<mode>
```

Images loaded: 124. Loops: 3. Encoded samples per encoder per mode: 372.

Baseline: `pillow.default*` using `PIL.Image.save(..., format="PNG")`.

`*` marks the default mode for each library.

Generated reports:

- `build/png-bench-fresh-rgb/png-bench.md`
- `build/png-bench-fresh-gray/png-bench.md`
- `build/png-bench-fresh-mask/png-bench.md`

## RGB

| Encoder | img/s | speed vs Pillow | raw MB/s | avg bytes | size vs Pillow |
|---|---:|---:|---:|---:|---:|
| pigzpp.fast* | 1036.9 | 6.75x | 241.6 | 59033 | 1.27x |
| cv2.default* | 702.5 | 4.57x | 163.7 | 66616 | 1.43x |
| pigzpp.balanced | 624.2 | 4.06x | 145.5 | 55206 | 1.19x |
| cv2.rle | 427.2 | 2.78x | 99.5 | 54808 | 1.18x |
| cv2.fast | 388.6 | 2.53x | 90.6 | 46757 | 1.01x |
| pillow.fast | 344.7 | 2.24x | 80.3 | 46535 | 1.00x |
| pillow.default* | 153.6 | 1.00x | 35.8 | 46478 | 1.00x |
| pigzpp.small | 66.6 | 0.43x | 15.5 | 44861 | 0.97x |
| cv2.small | 30.1 | 0.20x | 7.0 | 42150 | 0.91x |
| pillow.small | 29.0 | 0.19x | 6.8 | 45093 | 0.97x |

## Grayscale

| Encoder | img/s | speed vs Pillow | raw MB/s | avg bytes | size vs Pillow |
|---|---:|---:|---:|---:|---:|
| pigzpp.fast* | 2459.1 | 6.95x | 191.0 | 21812 | 1.07x |
| cv2.default* | 1971.0 | 5.57x | 153.1 | 24553 | 1.20x |
| pigzpp.balanced | 1489.4 | 4.21x | 115.7 | 20753 | 1.02x |
| cv2.rle | 1195.0 | 3.38x | 92.8 | 20462 | 1.00x |
| cv2.fast | 966.5 | 2.73x | 75.1 | 23205 | 1.14x |
| pillow.fast | 795.6 | 2.25x | 61.8 | 23092 | 1.13x |
| pillow.default* | 353.9 | 1.00x | 27.5 | 20409 | 1.00x |
| pigzpp.small | 139.1 | 0.39x | 10.8 | 19943 | 0.98x |
| cv2.small | 68.2 | 0.19x | 5.3 | 20793 | 1.02x |
| pillow.small | 62.5 | 0.18x | 4.9 | 20006 | 0.98x |

## Mask

| Encoder | img/s | speed vs Pillow | raw MB/s | avg bytes | size vs Pillow |
|---|---:|---:|---:|---:|---:|
| pigzpp.fast* | 7111.4 | 7.14x | 552.4 | 2869 | 1.01x |
| cv2.default* | 4656.7 | 4.68x | 361.7 | 3475 | 1.23x |
| pigzpp.balanced | 3156.4 | 3.17x | 245.2 | 2646 | 0.93x |
| cv2.rle | 2227.0 | 2.24x | 173.0 | 2613 | 0.92x |
| pillow.fast | 2106.9 | 2.12x | 163.7 | 4028 | 1.42x |
| cv2.fast | 2067.7 | 2.08x | 160.6 | 4021 | 1.42x |
| pillow.default* | 996.0 | 1.00x | 77.4 | 2834 | 1.00x |
| pigzpp.small | 395.2 | 0.40x | 30.7 | 2397 | 0.85x |
| cv2.small | 132.0 | 0.13x | 10.3 | 2492 | 0.88x |
| pillow.small | 131.8 | 0.13x | 10.2 | 2489 | 0.88x |

Summary: `pigzpp.fast*` was the throughput leader in all modes, ranging from 6.75x to 7.14x faster than Pillow default. `pigzpp.balanced` kept a strong speed advantage while improving size for masks, and `pigzpp.small` produced the smallest mask output at the expected cost in throughput.