#include <gtest/gtest.h>
#include "pool.h"

#include <thread>
#include <vector>

using namespace pigzpp;

TEST(Pool, BasicGetPut) {
    BufferPool pool(1024, -1);
    auto buf = pool.get();
    ASSERT_NE(buf, nullptr);
    EXPECT_EQ(buf->size(), 1024u);
    EXPECT_EQ(buf->len, 0u);
    buf.reset(); // returns to pool via custom deleter
}

TEST(Pool, Reuse) {
    BufferPool pool(1024, -1);
    auto buf1 = pool.get();
    auto raw1 = buf1.get(); // raw Buffer pointer
    buf1.reset(); // returns to pool via custom deleter

    auto buf2 = pool.get();
    EXPECT_EQ(buf2.get(), raw1); // Same Buffer object reused.
}

TEST(Pool, LimitBlocks) {
    BufferPool pool(64, 2);
    auto buf1 = pool.get();
    auto buf2 = pool.get();

    // Third get should block until one is returned.
    bool got_third = false;
    std::thread t([&] {
        auto buf3 = pool.get();
        got_third = true;
        buf3.reset(); // return to pool via custom deleter
    });

    // Give thread time to block
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    EXPECT_FALSE(got_third);

    buf1.reset(); // return to pool via custom deleter
    t.join();
    EXPECT_TRUE(got_third);
    buf2.reset();
}

TEST(Pool, ConcurrentAccess) {
    BufferPool pool(256, -1);
    constexpr int N = 100;
    std::vector<std::thread> threads;

    for (int i = 0; i < N; i++) {
        threads.emplace_back([&pool] {
            auto buf = pool.get();
            buf->len = 42;
            std::memset(buf->data(), 'x', 42);
            buf.reset(); // return to pool via deleter
        });
    }
    for (auto& t : threads) t.join();
}

TEST(Buffer, Grow) {
    Buffer buf(16);
    EXPECT_EQ(buf.size(), 16u);
    buf.grow();
    EXPECT_GT(buf.size(), 16u);
}
