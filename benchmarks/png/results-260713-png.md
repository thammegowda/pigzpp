# PNG Benchmark Results - 2026-07-13

Hardware: Intel Xeon W-2235 (5 cores / 10 threads, 3.80 GHz), Ubuntu 22.04.5 (WSL2).

Encoder: `pigzpp.png.compress(preset=...)` (Python API) vs Pillow 12.1.0 and OpenCV 4.13.0.

Dataset: the 24-image [Kodak](https://r0k.us/graphics/kodak/) true-color set (768×512, lossless PNG).

Benchmark command pattern:

```bash
python benchmarks/png/bench_png.py \
  --image-dir /tmp/kodak \
  --limit 24 \
  --loops 5 \
  --mode <rgb|gray|mask> \
  --verify \
  --out build/png-bench-<mode>
```

Images loaded: 24. Loops: 5. Encoded samples per encoder per mode: 120.

Baseline: `pillow.default*` using `PIL.Image.save(..., format="PNG")`.
`*` marks the default mode for each library. All pigzpp outputs round-trip via
`pigzpp.png.decompress_array()`.

## RGB

| Encoder | img/s | speed vs Pillow | raw MB/s | avg bytes | size vs Pillow |
|---|---:|---:|---:|---:|---:|
| pigzpp.fast* | 62.1 | 8.64x | 69.8 | 737606 | 1.11x |
| cv2.default* | 51.4 | 7.16x | 57.9 | 725285 | 1.09x |
| pigzpp.balanced | 39.3 | 5.46x | 44.2 | 703312 | 1.06x |
| cv2.rle | 33.6 | 4.67x | 37.8 | 683842 | 1.03x |
| cv2.fast | 31.0 | 4.32x | 34.9 | 681946 | 1.03x |
| pillow.fast | 26.3 | 3.66x | 29.6 | 683862 | 1.03x |
| pillow.default* | 7.2 | 1.00x | 8.1 | 664232 | 1.00x |
| pigzpp.small | 6.6 | 0.92x | 7.4 | 659637 | 0.99x |
| cv2.small | 4.6 | 0.63x | 5.1 | 657216 | 0.99x |
| pillow.small | 4.0 | 0.56x | 4.5 | 658732 | 0.99x |

## Grayscale

| Encoder | img/s | speed vs Pillow | raw MB/s | avg bytes | size vs Pillow |
|---|---:|---:|---:|---:|---:|
| cv2.default* | 158.9 | 7.37x | 59.6 | 237067 | 1.05x |
| pigzpp.fast* | 147.6 | 6.85x | 55.4 | 242473 | 1.07x |
| pigzpp.balanced | 114.2 | 5.30x | 42.8 | 230395 | 1.02x |
| cv2.rle | 94.4 | 4.38x | 35.4 | 225226 | 1.00x |
| cv2.fast | 81.1 | 3.76x | 30.4 | 251303 | 1.11x |
| pillow.fast | 62.6 | 2.90x | 23.5 | 251219 | 1.11x |
| pillow.default* | 21.6 | 1.00x | 8.1 | 225655 | 1.00x |
| pigzpp.small | 20.5 | 0.95x | 7.7 | 225513 | 1.00x |
| cv2.small | 14.6 | 0.68x | 5.5 | 238901 | 1.06x |
| pillow.small | 12.1 | 0.56x | 4.5 | 225380 | 1.00x |

## Mask

| Encoder | img/s | speed vs Pillow | raw MB/s | avg bytes | size vs Pillow |
|---|---:|---:|---:|---:|---:|
| pigzpp.fast* | 561.7 | 6.31x | 210.6 | 18753 | 0.97x |
| cv2.default* | 473.7 | 5.32x | 177.6 | 19502 | 1.01x |
| cv2.rle | 281.2 | 3.16x | 105.5 | 17534 | 0.91x |
| cv2.fast | 255.3 | 2.87x | 95.7 | 27949 | 1.45x |
| pigzpp.balanced | 249.9 | 2.81x | 93.7 | 17690 | 0.92x |
| pillow.fast | 196.9 | 2.21x | 73.9 | 27954 | 1.45x |
| pillow.default* | 89.1 | 1.00x | 33.4 | 19280 | 1.00x |
| pigzpp.small | 30.9 | 0.35x | 11.6 | 16292 | 0.85x |
| cv2.small | 10.9 | 0.12x | 4.1 | 17038 | 0.88x |
| pillow.small | 10.7 | 0.12x | 4.0 | 17003 | 0.88x |
