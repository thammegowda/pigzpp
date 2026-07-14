# pigzpp Future Wishlist

High-level ideas for later; details TBD.

- **ZIP archive API (multi-file container).** Useful because existing ZIP libraries don't
  interface with pigzpp out of the box. ZIP already uses DEFLATE (which pigzpp emits), so
  this is mostly container plumbing (headers + central directory) reusing the existing
  parallel engine — with random-access extraction.
- **Seekable gzip formats (BGZF, dictzip).** gzip-compatible, indexed random access;
  pigzpp's independent blocks make it a natural fit.
- **Streaming (incremental) compress/decompress API** for large/unknown-length inputs.
- **Additional codecs** (zstd, brotli) behind the same container plumbing, where licensing
  and WASM portability allow.
