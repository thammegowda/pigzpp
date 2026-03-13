#include <gtest/gtest.h>
#include <zlib.h>
#include "crc.h"
#include "config.h"

using namespace pigzpp;

TEST(CRC, Crc32zBasic) {
    const unsigned char data[] = "Hello, World!";
    unsigned long crc = crc32z(0UL, data, 13);
    EXPECT_NE(crc, 0UL);
    unsigned long expected = ::crc32(0UL, data, 13);
    EXPECT_EQ(crc, expected);
}

TEST(CRC, CheckGzip) {
    const unsigned char data[] = "Hello, World!";
    unsigned long c = check(Format::Gzip, 0UL, data, 13);
    unsigned long expected = ::crc32(0UL, data, 13);
    EXPECT_EQ(c, expected);
}

TEST(CRC, CheckZlib) {
    const unsigned char data[] = "Hello, World!";
    unsigned long c = check(Format::Zlib, 1UL, data, 13);
    unsigned long expected = ::adler32(1UL, data, 13);
    EXPECT_EQ(c, expected);
}

TEST(CRC, CombineCrc32) {
    const unsigned char data1[] = "Hello, ";
    const unsigned char data2[] = "World!";
    size_t len1 = 7, len2 = 6;

    unsigned long crc1 = ::crc32(0UL, data1, len1);
    unsigned long crc2 = ::crc32(0UL, data2, len2);
    unsigned long combined = check_combine(Format::Gzip, crc1, crc2, len2);

    unsigned long full_crc = ::crc32(0UL, data1, len1);
    full_crc = ::crc32(full_crc, data2, len2);
    EXPECT_EQ(combined, full_crc);
}

TEST(CRC, CombineCrc32WithPrecomputedOp) {
    size_t block = 128;
    std::vector<unsigned char> d1(block, 'A');
    std::vector<unsigned char> d2(block, 'B');

    unsigned long op = crc_combine_gen(block);
    unsigned long c1 = ::crc32(0UL, d1.data(), d1.size());
    unsigned long c2 = ::crc32(0UL, d2.data(), d2.size());
    unsigned long combined = check_combine(Format::Gzip, c1, c2, block, op);

    unsigned long full = ::crc32(0UL, d1.data(), d1.size());
    full = ::crc32(full, d2.data(), d2.size());
    EXPECT_EQ(combined, full);
}

TEST(CRC, CombineAdler32) {
    const unsigned char data1[] = "Hello, ";
    const unsigned char data2[] = "World!";
    size_t len1 = 7, len2 = 6;

    unsigned long a1 = ::adler32(1UL, data1, len1);
    unsigned long a2 = ::adler32(1UL, data2, len2);
    unsigned long combined = check_combine(Format::Zlib, a1, a2, len2);

    unsigned long full = ::adler32(1UL, data1, len1);
    full = ::adler32(full, data2, len2);
    EXPECT_EQ(combined, full);
}
