# Plan 02: Beating pigz Performance

## Status Quo

pigzpp matches pigz on compression and decompression (50.8 MB test, 48-core machine):

| Benchmark | pigz | pigzpp | Δ |
|---|---|---|---|
| Compress level 1 | 42 ms | 32 ms | **pigzpp 24% faster** |
| Compress level 6 | 45 ms | 44 ms | Equal |
| Compress level 9 | 56 ms | 59 ms | ~5% slower |
| Decompress (level 6) | 54 ms | 54 ms | Equal |
| Compressed size (L6) | 3,090,285 B | 3,090,285 B | Identical |

Interoperability: pigzpp ↔ pigz ↔ gzip all pass.

---

## 6 Optimizations to Beat pigz

### 1. CRC-After-Write-Enqueue (biggest win for compression)

**Problem:** We compute CRC *before* inserting into the write queue. The write thread sits idle during CRC computation.

**pigz's approach:** Insert job into write queue FIRST, *then* compute CRC while write proceeds in parallel.

**Current code** in `compress.cpp` parallel compress worker:
```
compute CRC → set_value(check) → insert into write_queue → notify
```

**Should be:**
```
insert into write_queue → notify → compute CRC → signal check_ready
```

The write thread writes `job->out` immediately, then waits for the CRC flag only when it needs to combine check values. This overlaps CRC computation with I/O — the CRC for block N runs while block N's compressed data is being written to disk.

**Files:** `pigzpp/src/lib/compress.cpp` (parallel compress worker + write worker)

---

### 2. Replace `std::promise/future` with `std::atomic` Flag

**Problem:** Each `std::promise<unsigned long>` allocates shared state on the heap. For a 50 MB file at 128K block size, that's ~400 heap allocations + deallocations per file.

**Fix — change `Job` struct:**
```cpp
struct Job {
    int64_t seq = 0;
    bool more = false;
    BufferPtr in, dict, out;
    std::vector<unsigned char> lens;
    unsigned long check = 0;
    std::atomic<int> check_done{0}; // replaces std::promise<unsigned long>
};
```

Compress thread: `job->check = chk; job->check_done.store(1, std::memory_order_release);`

Write thread: spin-yield on `job->check_done.load(std::memory_order_acquire)` — fast because by the time the write finishes, CRC is already done.

**Files:** `pigzpp/src/lib/compress.cpp` (Job struct + compress worker + write worker)

---

### 3. Double-Buffered Read-Ahead for Decompression Input

**Problem:** `InputReader::load()` does a synchronous `read()` syscall. While reading from disk, the inflate engine is stalled.

**pigz does this** via `load_read()` thread + double-buffering between `in_buf` and `in_buf2`, activated when `procs > 1`.

**Fix — extend `InputReader` (in `format.h`):**
```cpp
// Add members:
unsigned char in_buf2_[EXT]{};
std::jthread read_thread_;
std::atomic<int> read_state_{0}; // 0=idle/done, 1=go read
size_t read_len_ = 0;
int which_ = -1; // -1=not started, 0=in_buf2, 1=in_buf
int procs_;
```

On first `load()` call with `procs > 1`:
1. Launch read thread
2. Alternate between `in_buf_` and `in_buf2_`
3. While inflate processes data from one buffer, the read thread fills the other
4. Use atomic spin-yield for minimal synchronization overhead

**Files:** `pigzpp/src/lib/format.h` + `pigzpp/src/lib/format.cpp` (InputReader class)

---

### 4. `posix_fadvise(FADV_SEQUENTIAL)` on Input Files

**Problem:** The kernel doesn't know we're doing sequential reads and uses default (conservative) read-ahead.

**Fix — add at the start of compress/decompress:**
```cpp
#ifdef POSIX_FADV_SEQUENTIAL
posix_fadvise(in_fd, 0, 0, POSIX_FADV_SEQUENTIAL);
#endif
```

This tells the kernel to aggressively prefetch pages, effectively free performance.

**Files:** `pigzpp/src/lib/compress.cpp` (Compressor::compress), `pigzpp/src/lib/decompress.cpp` (Decompressor::decompress)

---

### 5. Buffered Output in `single_compress` (Reduce Write Syscalls)

**Problem:** The `deflate_write` lambda calls `writen()` for every deflate output chunk (~a few KB each). For a 50 MB file, that's thousands of `write()` syscalls, each with context-switch overhead.

**Fix — accumulate into a 128K buffer, flush when full:**
```cpp
std::vector<unsigned char> write_buf(131072);
size_t write_pos = 0;

auto flush_write = [&] {
    if (write_pos > 0) {
        clen += writen(out_fd, write_buf.data(), write_pos);
        write_pos = 0;
    }
};

auto deflate_write = [&](int flush) {
    do {
        strm.avail_out = static_cast<unsigned>(write_buf.size() - write_pos);
        strm.next_out = write_buf.data() + write_pos;
        ::deflate(&strm, flush);
        write_pos = write_buf.size() - strm.avail_out;
        if (write_pos >= write_buf.size())
            flush_write();
    } while (strm.avail_out == 0);
    assert(strm.avail_in == 0);
};
// ... at end: flush_write();
```

**Files:** `pigzpp/src/lib/compress.cpp` (single_compress)

---

### 6. Read-Ahead in Parallel Compression Main Loop

**Problem:** The main thread reading input blocks synchronously — compress threads may idle while the next block is being read from disk.

**Fix — overlap read with dispatch:**
```cpp
// After enqueuing a job, kick off read for next block in background
auto read_future = std::async(std::launch::async, [&] {
    auto buf = in_pool.get();
    buf->len = readn(in_fd, buf->data(), buf->size());
    return buf;
});
// Next iteration: next_buf = read_future.get();
```

Alternatively, use a dedicated read thread that keeps one block ahead, matching pigz's design.

**Files:** `pigzpp/src/lib/compress.cpp` (parallel_compress main loop)

---

## Expected Impact

| # | Optimization | Affects | Expected Speedup |
|---|---|---|---|
| 1 | CRC-after-write-enqueue | Parallel compression | 5–15% |
| 2 | Atomic flag vs promise/future | Parallel compression | 2–5% |
| 3 | Double-buffered read-ahead | Decompression | 5–15% |
| 4 | posix_fadvise | Both | 2–5% |
| 5 | Buffered single_compress output | Single-thread compress | 3–8% |
| 6 | Read-ahead in parallel compress | Parallel compression | 2–5% |

**Combined target: 10–30% faster than pigz** while maintaining byte-identical output and full interoperability.

---

## Verification

After each optimization:
1. All 35 GoogleTest tests must pass
2. Cross-compatibility: pigzpp ↔ pigz ↔ gzip roundtrip
3. Compressed output size must remain identical to pigz
4. Benchmark against pigz on same 50.8 MB dataset (3 runs, best-of-3)

## Implementation Order

1. **Optimizations 1 + 2** together (both touch Job struct and compress worker)
2. **Optimization 4** (one-liner, test immediately)
3. **Optimization 5** (self-contained in single_compress)
4. **Optimization 3** (more complex, touches format.h/cpp + decompress.cpp)
5. **Optimization 6** (touches parallel_compress main loop)

Benchmark after each step to measure incremental gains.
