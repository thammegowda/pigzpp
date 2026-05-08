#!/usr/bin/env python3
"""Benchmark PNG encoders against Pillow, the common Python PNG baseline."""

from __future__ import annotations

import argparse
import csv
import io
import statistics
import time
from dataclasses import dataclass
from pathlib import Path
from typing import Callable

import numpy as np
import pigzpp

try:
    import cv2
except Exception:  # pragma: no cover - optional dependency
    cv2 = None

try:
    from PIL import Image
except Exception:  # pragma: no cover - optional dependency
    Image = None


@dataclass
class CaseResult:
    name: str
    images: int
    seconds: float
    img_s: float
    mb_s: float
    avg_bytes: int
    min_bytes: int
    max_bytes: int
    speed_vs_pillow: float | None = None
    size_vs_pillow: float | None = None


Encoder = Callable[[np.ndarray], bytes]


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--image-dir", type=Path, help="Directory containing PNG/JPEG images to benchmark")
    parser.add_argument("--limit", type=int, default=200, help="Maximum images to load")
    parser.add_argument("--loops", type=int, default=3, help="Benchmark loops over loaded images")
    parser.add_argument("--size", type=int, default=512, help="Synthetic image size when --image-dir is omitted")
    parser.add_argument("--out", type=Path, default=Path("build/png-bench"), help="Output directory for report files")
    parser.add_argument("--mode", choices=("rgb", "gray", "mask"), default="rgb", help="Benchmark RGB, grayscale, or binary mask images")
    parser.add_argument("--verify", action="store_true", help="Verify a few encoded outputs decode to original pixels")
    return parser.parse_args()


def synthetic_images(size: int, count: int, mode: str) -> list[np.ndarray]:
    images: list[np.ndarray] = []
    y, x = np.mgrid[0:size, 0:size]
    for index in range(count):
        if mode != "rgb":
            image = ((x * (3 + index % 5) + y * (5 + index % 7)) & 0xFF).astype(np.uint8)
            if mode == "mask":
                image = ((image > 127) * 255).astype(np.uint8)
            images.append(image)
            continue
        image = np.empty((size, size, 3), dtype=np.uint8)
        image[..., 0] = (x * (3 + index % 5) + y * 2) & 0xFF
        image[..., 1] = (y * (5 + index % 7)) & 0xFF
        image[..., 2] = ((x // 8 + y // 8 + index) % 2) * 220
        images.append(image)
    return images


def load_images(image_dir: Path, limit: int, mode: str) -> list[np.ndarray]:
    if cv2 is None:
        raise RuntimeError("OpenCV is required to load image directories")
    paths = []
    for pattern in ("*.png", "*.jpg", "*.jpeg", "*.webp", "*.bmp"):
        paths.extend(sorted(image_dir.rglob(pattern)))
    images: list[np.ndarray] = []
    for path in paths[:limit]:
        if mode != "rgb":
            gray = cv2.imread(str(path), cv2.IMREAD_GRAYSCALE)
            if gray is None:
                continue
            if mode == "mask":
                gray = ((gray > 127) * 255).astype(np.uint8)
            images.append(gray)
            continue
        bgr = cv2.imread(str(path), cv2.IMREAD_COLOR)
        if bgr is None:
            continue
        images.append(cv2.cvtColor(bgr, cv2.COLOR_BGR2RGB))
    if not images:
        raise RuntimeError(f"no images loaded from {image_dir}")
    return images


def pigzpp_png(preset: str) -> Encoder:
    def encode(image: np.ndarray) -> bytes:
        return pigzpp.png.compress(image, preset=preset)

    return encode


def pillow_png(**save_kwargs) -> Encoder:
    if Image is None:
        raise RuntimeError("Pillow is required for the PNG benchmark baseline")

    def encode(image: np.ndarray) -> bytes:
        output = io.BytesIO()
        Image.fromarray(image).save(output, format="PNG", **save_kwargs)
        return output.getvalue()

    return encode


def cv2_png(params: list[int] | None = None) -> Encoder:
    if cv2 is None:
        raise RuntimeError("OpenCV is not installed")

    def encode(image: np.ndarray) -> bytes:
        bgr = image if image.ndim == 2 else cv2.cvtColor(image, cv2.COLOR_RGB2BGR)
        if params is None:
            ok, buffer = cv2.imencode(".png", bgr)
        else:
            ok, buffer = cv2.imencode(".png", bgr, params)
        if not ok:
            raise RuntimeError("cv2.imencode failed")
        return buffer.tobytes()

    return encode


def verify_encoder(name: str, encoder: Encoder, images: list[np.ndarray]) -> None:
    if cv2 is None:
        return
    for image in images[:3]:
        encoded = encoder(image)
        if not name.startswith("cv2."):
            decoded = pigzpp.png.decompress_array(encoded)
            if not np.array_equal(decoded, image):
                raise RuntimeError(f"{name} output did not round-trip")
            continue

        decoded = cv2.imdecode(np.frombuffer(encoded, dtype=np.uint8), cv2.IMREAD_UNCHANGED)
        if decoded is None:
            raise RuntimeError(f"{name} output did not decode")
        if image.ndim == 2 and decoded.ndim == 2:
            decoded_rgb = decoded
        elif decoded.ndim == 3 and decoded.shape[2] >= 3:
            decoded_rgb = cv2.cvtColor(decoded[..., :3], cv2.COLOR_BGR2RGB)
        else:
            raise RuntimeError(f"{name} decoded to unsupported shape {decoded.shape}")
        if not np.array_equal(decoded_rgb, image):
            raise RuntimeError(f"{name} output did not round-trip")


def run_case(name: str, encoder: Encoder, images: list[np.ndarray], loops: int) -> CaseResult:
    sizes: list[int] = []
    raw_bytes = sum(image.nbytes for image in images) * loops
    start = time.perf_counter()
    for _ in range(loops):
        for image in images:
            sizes.append(len(encoder(image)))
    seconds = time.perf_counter() - start
    count = len(images) * loops
    return CaseResult(
        name=name,
        images=count,
        seconds=seconds,
        img_s=count / seconds,
        mb_s=(raw_bytes / seconds) / (1024 * 1024),
        avg_bytes=round(statistics.mean(sizes)),
        min_bytes=min(sizes),
        max_bytes=max(sizes),
    )


def build_cases() -> list[tuple[str, Encoder]]:
    cases = [
        ("pillow.default*", pillow_png()),
        ("pillow.fast", pillow_png(compress_level=1)),
        ("pillow.small", pillow_png(compress_level=9, optimize=True)),
    ]
    if cv2 is not None:
        cases.extend([
            ("cv2.default*", cv2_png()),
            ("cv2.fast", cv2_png([cv2.IMWRITE_PNG_COMPRESSION, 1])),
            ("cv2.small", cv2_png([cv2.IMWRITE_PNG_COMPRESSION, 9])),
            ("cv2.rle", cv2_png([
                cv2.IMWRITE_PNG_COMPRESSION, 1,
                cv2.IMWRITE_PNG_STRATEGY, cv2.IMWRITE_PNG_STRATEGY_RLE,
            ])),
        ])
    cases.extend([
        ("pigzpp.fast*", pigzpp_png("fast")),
        ("pigzpp.balanced", pigzpp_png("balanced")),
        ("pigzpp.small", pigzpp_png("small")),
    ])
    return cases


def write_reports(results: list[CaseResult], output_dir: Path, image_count: int, loops: int, mode: str) -> None:
    output_dir.mkdir(parents=True, exist_ok=True)
    csv_path = output_dir / "png-bench.csv"
    md_path = output_dir / "png-bench.md"

    with csv_path.open("w", newline="") as handle:
        writer = csv.writer(handle)
        writer.writerow([
            "name",
            "images",
            "seconds",
            "img_s",
            "mb_s",
            "avg_bytes",
            "min_bytes",
            "max_bytes",
            "speed_vs_pillow",
            "size_vs_pillow",
        ])
        for result in results:
            writer.writerow([
                result.name,
                result.images,
                f"{result.seconds:.6f}",
                f"{result.img_s:.2f}",
                f"{result.mb_s:.2f}",
                result.avg_bytes,
                result.min_bytes,
                result.max_bytes,
                "" if result.speed_vs_pillow is None else f"{result.speed_vs_pillow:.4f}",
                "" if result.size_vs_pillow is None else f"{result.size_vs_pillow:.4f}",
            ])

    with md_path.open("w") as handle:
        handle.write("# PNG Benchmark\n\n")
        handle.write(f"Mode: {mode}\n\n")
        handle.write(f"Images: {image_count}; loops: {loops}; encoded samples: {image_count * loops}\n\n")
        handle.write("Baseline: `pillow.default*` (`PIL.Image.save(..., format=\"PNG\")`)\n\n")
        handle.write("`*` marks the default mode for each library.\n\n")
        handle.write("| Encoder | img/s | speed vs Pillow | raw MB/s | avg bytes | size vs Pillow |\n")
        handle.write("|---|---:|---:|---:|---:|---:|\n")
        for result in results:
            speed_ratio = "-" if result.speed_vs_pillow is None else f"{result.speed_vs_pillow:.2f}x"
            size_ratio = "-" if result.size_vs_pillow is None else f"{result.size_vs_pillow:.2f}x"
            handle.write(f"| {result.name} | {result.img_s:.1f} | {speed_ratio} | {result.mb_s:.1f} | {result.avg_bytes} | {size_ratio} |\n")

    print(f"wrote {csv_path}")
    print(f"wrote {md_path}")


def main() -> None:
    args = parse_args()
    images = load_images(args.image_dir, args.limit, args.mode) if args.image_dir else synthetic_images(args.size, args.limit, args.mode)
    cases = build_cases()
    if args.verify:
        for name, encoder in cases:
            verify_encoder(name, encoder, images)

    results = [run_case(name, encoder, images, args.loops) for name, encoder in cases]
    baseline = next((result for result in results if result.name == "pillow.default*"), None)
    if baseline:
        for result in results:
            result.speed_vs_pillow = result.img_s / baseline.img_s
            result.size_vs_pillow = result.avg_bytes / baseline.avg_bytes
    results.sort(key=lambda item: item.img_s, reverse=True)
    write_reports(results, args.out, len(images), args.loops, args.mode)
    for result in results:
        speed_ratio = "" if result.speed_vs_pillow is None else f" speed={result.speed_vs_pillow:.2f}x"
        size_ratio = "" if result.size_vs_pillow is None else f" size={result.size_vs_pillow:.2f}x"
        print(f"{result.name:34s} {result.img_s:8.1f} img/s {result.mb_s:8.1f} MB/s avg={result.avg_bytes:8d}{speed_ratio}{size_ratio}")


if __name__ == "__main__":
    main()