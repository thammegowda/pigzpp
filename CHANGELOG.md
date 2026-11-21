# Changelog

All notable changes to pigzpp are documented here. The format follows
[Keep a Changelog](https://keepachangelog.com/en/1.1.0/), and versions follow
[Semantic Versioning](https://semver.org/). `VERSION.txt` is the version source of truth.

## [Unreleased]

### Added

- CLI `-v` progress reporting on `stderr` (never `stdout`) for compression and
  decompression (`-d`, `-t`), with 500 ms terminal-width-aware inline redraws and a
  single summary line when `stderr` is redirected (#3, thanks to @gerdus).
- C++ progress API: `pigzpp::Config::progress` and `pigzpp::Config::progress_interval`
  across `Compressor` and `Decompressor`, plus `pigzpp::Progress` (`io_utils.h`) for
  formatted `stderr`, `FILE*`, or custom `Sink` output.
- Windows MSVC build and test portability improvements across the `Makefile`, test
  helpers, and core benchmarks (#3, @gerdus).

## [1.2.0] - 2026-11-21

### Added

- `pigzpp::png::decode` now reads every standard PNG layout instead of only
  8-bit, non-interlaced gray, gray+alpha, RGB, and RGBA:
  - palette (indexed-color) images, expanded to RGB, or to RGBA when a `tRNS`
    chunk is present;
  - 1-, 2-, and 4-bit grayscale and palette images, scaled to 8-bit samples;
  - 16-bit images, reduced to their high byte;
  - `tRNS` color keys on gray and RGB images, converted to an alpha channel;
  - Adam7-interlaced images.

### Changed

- The decoded channel count now depends on the source format (gray 1, gray+alpha 2,
  RGB 3, RGBA 4). Palette and color-keyed images gain an alpha channel only when they
  carry transparency.

### Fixed

- The new decoding paths validate their inputs and fail with a clear error on an
  invalid bit depth for the color type, a palette index outside the palette, a missing
  or malformed `PLTE`, or a malformed `tRNS` chunk.
