// Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
// SPDX-License-Identifier: BSD 3-Clause Clear License
//
// Unit tests for qsr4_writer: .qdb header validation and decompression.

#include "file_writer/qsr4_writer.h"
#include <cassert>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

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
#define ASSERT_FALSE(x) do { if ((x)) throw std::runtime_error( \
    std::string("Assertion failed (expected false) at line ") + std::to_string(__LINE__)); } while(0)

// Build a valid 64-byte .qdb header for the given guid (version 0).
static ByteBuffer make_qdb_header(const std::array<uint8_t,16>& guid) {
    ByteBuffer h(QDB_HEADER_SIZE, 0);
    h[0] = 0x7F; h[1] = 'Q'; h[2] = 'D'; h[3] = 'B';
    for (int i = 0; i < 16; ++i) h[4 + i] = guid[i];
    // bytes 20..23 version = 0 (already zeroed)
    return h;
}

TEST(validate_qdb_header_good) {
    std::array<uint8_t,16> guid{}; for (int i=0;i<16;++i) guid[i]=static_cast<uint8_t>(i);
    ByteBuffer qdb = make_qdb_header(guid);
    std::array<uint8_t,16> out{};
    ASSERT_TRUE(validate_qdb_header(qdb, out));
    ASSERT_TRUE(out == guid);
}

TEST(validate_qdb_header_bad_magic) {
    std::array<uint8_t,16> guid{};
    ByteBuffer qdb = make_qdb_header(guid);
    qdb[0] = 0x00;
    std::array<uint8_t,16> out{};
    ASSERT_FALSE(validate_qdb_header(qdb, out));
}

TEST(validate_qdb_header_bad_version) {
    std::array<uint8_t,16> guid{};
    ByteBuffer qdb = make_qdb_header(guid);
    qdb[20] = 0x01;  // version != 0
    std::array<uint8_t,16> out{};
    ASSERT_FALSE(validate_qdb_header(qdb, out));
}

TEST(validate_qdb_header_too_short) {
    ByteBuffer qdb(10, 0); qdb[0]=0x7F;
    std::array<uint8_t,16> out{};
    ASSERT_FALSE(validate_qdb_header(qdb, out));
}

TEST(guid_to_string_format) {
    std::array<uint8_t,16> guid = {0x01,0x23,0x45,0x67,0x89,0xAB,0xCD,0xEF,
                                   0x01,0x23,0x45,0x67,0x89,0xAB,0xCD,0xEF};
    ASSERT_TRUE(guid_to_string(guid) == "01234567-89ab-cdef-0123-456789abcdef");
}

TEST(decompress_qdb_round_trip) {
    // 64-byte header + real zlib stream of ("Hello QShrink4 payload! " * 40),
    // produced by Python zlib.compress(..., 9). Decompress must reproduce it.
    const uint8_t comp[] = {
        120,218,243,72,205,201,201,87,8,12,206,40,202,204,203,54,81,40,72,172,
        204,201,79,76,81,84,240,24,21,31,21,31,21,31,244,226,0,160,175,77,8
    };
    std::string unit = "Hello QShrink4 payload! ";
    std::string original;
    for (int i = 0; i < 40; ++i) original += unit;

    std::array<uint8_t,16> guid{};
    ByteBuffer qdb = make_qdb_header(guid);
    qdb.insert(qdb.end(), comp, comp + sizeof(comp));

    std::string out_path = "test_qsr4_round_trip.qsr4";
    ASSERT_TRUE(decompress_qdb_to_qsr4(qdb, out_path));

    std::ifstream f(out_path, std::ios::binary);
    std::string got((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    f.close();
    std::remove(out_path.c_str());
    ASSERT_TRUE(got == original);
}

int main() {
    std::cout << "Running qsr4_writer tests:\n";
    std::cout << tests_passed + tests_failed << " tests, "
              << tests_failed << " failed\n";
    return tests_failed == 0 ? 0 : 1;
}
