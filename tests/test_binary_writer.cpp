// Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
// SPDX-License-Identifier: BSD 3-Clause Clear License
//
// Unit tests for the BinaryWriter streaming file writer.

#include <cassert>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iostream>
#include <vector>

#include "file_writer/binary_writer.h"

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

#define ASSERT_EQ(a, b) do { \
    if ((a) != (b)) throw std::runtime_error( \
        std::string("Expected ") + std::to_string(b) + " but got " + std::to_string(a) \
        + " at line " + std::to_string(__LINE__)); \
} while(0)

#define ASSERT_TRUE(x) do { \
    if (!(x)) throw std::runtime_error( \
        std::string("Assertion failed at line ") + std::to_string(__LINE__)); \
} while(0)

#define ASSERT_FALSE(x) do { \
    if ((x)) throw std::runtime_error( \
        std::string("Assertion failed (expected false) at line ") + std::to_string(__LINE__)); \
} while(0)

static std::vector<uint8_t> read_file_bytes(const std::string& path) {
    std::ifstream f(path, std::ios::binary);
    f.seekg(0, std::ios::end);
    size_t size = static_cast<size_t>(f.tellg());
    f.seekg(0, std::ios::beg);
    std::vector<uint8_t> data(size);
    f.read(reinterpret_cast<char*>(data.data()), static_cast<std::streamsize>(size));
    return data;
}

// =====================================================================
// Basic open/close
// =====================================================================

TEST(open_close) {
    std::string path = "test_bw_open_close.bin";
    BinaryWriter writer;

    ASSERT_FALSE(writer.is_open());
    ASSERT_TRUE(writer.open(path));
    ASSERT_TRUE(writer.is_open());
    ASSERT_EQ(writer.bytes_written(), 0u);

    writer.close();
    ASSERT_FALSE(writer.is_open());

    std::remove(path.c_str());
}

// =====================================================================
// Write data and verify file contents
// =====================================================================

TEST(write_data) {
    std::string path = "test_bw_write_data.bin";
    BinaryWriter writer;

    ASSERT_TRUE(writer.open(path));

    uint8_t data1[] = { 0xDE, 0xAD, 0xBE, 0xEF };
    ASSERT_TRUE(writer.write(data1, sizeof(data1)));
    ASSERT_EQ(writer.bytes_written(), 4u);

    uint8_t data2[] = { 0xCA, 0xFE, 0xBA, 0xBE };
    ASSERT_TRUE(writer.write(data2, sizeof(data2)));
    ASSERT_EQ(writer.bytes_written(), 8u);

    writer.close();

    auto contents = read_file_bytes(path);
    ASSERT_EQ(contents.size(), 8u);
    ASSERT_TRUE(memcmp(contents.data(), "\xDE\xAD\xBE\xEF\xCA\xFE\xBA\xBE", 8) == 0);

    std::remove(path.c_str());
}

// =====================================================================
// Write returns false when not open
// =====================================================================

TEST(write_when_closed) {
    BinaryWriter writer;
    uint8_t data[] = { 0x01, 0x02 };
    ASSERT_FALSE(writer.write(data, sizeof(data)));
}

// =====================================================================
// Write null/zero-length returns false
// =====================================================================

TEST(write_null_or_empty) {
    std::string path = "test_bw_null.bin";
    BinaryWriter writer;
    ASSERT_TRUE(writer.open(path));

    ASSERT_FALSE(writer.write(nullptr, 5));
    ASSERT_FALSE(writer.write(nullptr, 0));
    uint8_t data[] = { 0x01 };
    ASSERT_FALSE(writer.write(data, 0));

    ASSERT_EQ(writer.bytes_written(), 0u);

    writer.close();
    std::remove(path.c_str());
}

// =====================================================================
// Open invalid path fails
// =====================================================================

TEST(open_invalid_path) {
    BinaryWriter writer;
    ASSERT_FALSE(writer.open("/nonexistent/dir/file.bin"));
    ASSERT_FALSE(writer.is_open());
}

// =====================================================================
// Path accessor
// =====================================================================

TEST(path_accessor) {
    std::string path = "test_bw_path.bin";
    BinaryWriter writer;
    ASSERT_TRUE(writer.path().empty());

    ASSERT_TRUE(writer.open(path));
    ASSERT_TRUE(writer.path() == path);

    writer.close();
    std::remove(path.c_str());
}

// =====================================================================
// Move semantics
// =====================================================================

TEST(move_constructor) {
    std::string path = "test_bw_move.bin";
    BinaryWriter writer1;
    ASSERT_TRUE(writer1.open(path));

    uint8_t data[] = { 0xAA, 0xBB, 0xCC };
    ASSERT_TRUE(writer1.write(data, sizeof(data)));

    BinaryWriter writer2(std::move(writer1));
    ASSERT_TRUE(writer2.is_open());
    ASSERT_EQ(writer2.bytes_written(), 3u);
    ASSERT_TRUE(writer2.path() == path);

    // Write more through the moved-to writer
    uint8_t data2[] = { 0xDD, 0xEE };
    ASSERT_TRUE(writer2.write(data2, sizeof(data2)));
    ASSERT_EQ(writer2.bytes_written(), 5u);

    writer2.close();

    auto contents = read_file_bytes(path);
    ASSERT_EQ(contents.size(), 5u);
    ASSERT_TRUE(memcmp(contents.data(), "\xAA\xBB\xCC\xDD\xEE", 5) == 0);

    std::remove(path.c_str());
}

// =====================================================================
// Flush does not corrupt data
// =====================================================================

TEST(flush_preserves_data) {
    std::string path = "test_bw_flush.bin";
    BinaryWriter writer;
    ASSERT_TRUE(writer.open(path));

    uint8_t data[] = { 0x01, 0x02, 0x03 };
    ASSERT_TRUE(writer.write(data, sizeof(data)));
    writer.flush();

    // Write more after flush
    uint8_t data2[] = { 0x04, 0x05 };
    ASSERT_TRUE(writer.write(data2, sizeof(data2)));
    ASSERT_EQ(writer.bytes_written(), 5u);

    writer.close();

    auto contents = read_file_bytes(path);
    ASSERT_EQ(contents.size(), 5u);
    ASSERT_TRUE(memcmp(contents.data(), "\x01\x02\x03\x04\x05", 5) == 0);

    std::remove(path.c_str());
}

// =====================================================================
// Double close is safe
// =====================================================================

TEST(double_close) {
    std::string path = "test_bw_double_close.bin";
    BinaryWriter writer;
    ASSERT_TRUE(writer.open(path));
    writer.close();
    writer.close();  // should not crash
    ASSERT_FALSE(writer.is_open());
    std::remove(path.c_str());
}

int main() {
    std::cout << "BinaryWriter Tests:\n";
    std::cout << "\nResults: " << tests_passed << " passed, "
              << tests_failed << " failed\n";
    return tests_failed > 0 ? 1 : 0;
}
