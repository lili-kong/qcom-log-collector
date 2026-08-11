// Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
// SPDX-License-Identifier: BSD 3-Clause Clear License
//
// Unit tests for the self-contained zlib/DEFLATE inflater.

#include "inflate.h"
#include <iostream>
#include <string>

using namespace QC;

static int tests_passed = 0;
static int tests_failed = 0;

#define TEST(name) \
    static void test_##name(); \
    static struct Register_##name { \
        Register_##name() { \
            std::cout << "  " #name "... "; \
            try { test_##name(); std::cout << "PASS\n"; ++tests_passed; } \
            catch (const std::exception& e) { std::cout << "FAIL: " << e.what() << "\n"; ++tests_failed; } \
            catch (...) { std::cout << "FAIL: unknown exception\n"; ++tests_failed; } \
        } \
    } register_##name; \
    static void test_##name()

#define ASSERT_TRUE(x) do { if (!(x)) throw std::runtime_error( \
    std::string("Assertion failed at line ") + std::to_string(__LINE__)); } while(0)

TEST(zlib_inflate_repetitive) {
    // zlib.compress(b"Hello QShrink4 payload! " * 40, 9)
    const uint8_t comp[] = {
        120,218,243,72,205,201,201,87,8,12,206,40,202,204,203,54,81,40,72,172,
        204,201,79,76,81,84,240,24,21,31,21,31,21,31,244,226,0,160,175,77,8
    };
    std::string unit = "Hello QShrink4 payload! ";
    std::string expected;
    for (int i = 0; i < 40; ++i) expected += unit;

    ByteBuffer out;
    ASSERT_TRUE(zlib_inflate(comp, sizeof(comp), out));
    ASSERT_TRUE(out.size() == expected.size());
    ASSERT_TRUE(std::string(out.begin(), out.end()) == expected);
}

TEST(zlib_inflate_rejects_garbage) {
    const uint8_t bad[] = { 0x00, 0x01, 0x02, 0x03 };
    ByteBuffer out;
    ASSERT_TRUE(!zlib_inflate(bad, sizeof(bad), out));  // bad CM nibble
}

int main() {
    std::cout << "Running inflate tests:\n";
    std::cout << tests_passed + tests_failed << " tests, " << tests_failed << " failed\n";
    return tests_failed == 0 ? 0 : 1;
}
