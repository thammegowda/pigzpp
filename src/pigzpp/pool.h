// Thread-safe buffer pool for memory reuse during parallel compression.
// Replaces struct pool/space from pigz.c with RAII-based C++ equivalents.

#pragma once

#include <cassert>
#include <cstddef>
#include <cstring>
#include <memory>
#include <mutex>
#include <condition_variable>
#include <vector>

namespace pigzpp {

class BufferPool;

// A buffer obtained from a pool. When all shared_ptr references are released,
// the buffer returns itself to the pool.
struct Buffer {
    std::vector<unsigned char> buf;
    size_t len = 0; // Current data length (may be < buf.size()).
    BufferPool* pool = nullptr;

    explicit Buffer(size_t size) : buf(size) {}

    unsigned char* data() { return buf.data(); }
    const unsigned char* data() const { return buf.data(); }
    size_t size() const { return buf.size(); }

    void grow() {
        size_t more = buf.size() + (buf.size() >> 2);
        if (more < 16) more = 16;
        if (more <= buf.size()) more = static_cast<size_t>(-1); // overflow
        buf.resize(more);
    }

    void resize(size_t new_size) {
        buf.resize(new_size);
    }
};

using BufferPtr = std::shared_ptr<Buffer>;

// Pool of reusable buffers. Thread-safe. Limits the number of buffers
// to prevent unbounded memory growth.
class BufferPool {
public:
    // limit < 0 means unlimited.
    BufferPool(size_t buf_size, int limit)
        : buf_size_(buf_size), limit_(limit) {}

    BufferPool() = default;

    void init(size_t buf_size, int limit) {
        buf_size_ = buf_size;
        limit_ = limit;
    }

    // Get a buffer from the pool (blocks if at limit and none available).
    // The returned shared_ptr has a custom deleter that returns the buffer
    // to the pool when all references are released.
    BufferPtr get() {
        std::unique_lock lock(mu_);

        // If at limit and none available, wait.
        if (limit_ == 0 && free_.empty()) {
            cv_.wait(lock, [this] { return !free_.empty(); });
        }

        Buffer* raw = nullptr;
        if (!free_.empty()) {
            raw = free_.back().release();
            free_.pop_back();
            raw->len = 0;
        } else {
            assert(limit_ != 0); // Should not reach here if limit is 0
            if (limit_ > 0) limit_--;
            made_++;
            lock.unlock();
            raw = new Buffer(buf_size_);
            lock.lock();
        }
        raw->pool = this;
        // Custom deleter returns buffer to pool
        return BufferPtr(raw, [](Buffer* b) {
            if (b && b->pool) {
                b->pool->put_raw(b);
            } else {
                delete b;
            }
        });
    }

    // Return a buffer to the pool for reuse (takes ownership of raw pointer).
    void put_raw(Buffer* raw) {
        std::lock_guard lock(mu_);
        raw->len = 0;
        free_.push_back(std::unique_ptr<Buffer>(raw));
        cv_.notify_one();
    }

    // Return a buffer to the pool for reuse (shared_ptr version — releases the shared_ptr).
    void put(BufferPtr bp) {
        // Just reset the shared_ptr; the custom deleter will return it to pool
        bp.reset();
    }

    size_t buf_size() const { return buf_size_; }

private:
    std::mutex mu_;
    std::condition_variable cv_;
    std::vector<std::unique_ptr<Buffer>> free_;
    size_t buf_size_ = 0;
    int limit_ = -1;
    int made_ = 0;
};

} // namespace pigzpp
