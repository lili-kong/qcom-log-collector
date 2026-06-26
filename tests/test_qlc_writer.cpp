// SPDX-License-Identifier: Apache-2.0
// Copyright 2024 Qualcomm Innovation Center, Inc.
//
// Unit tests for the QLC metadata binary file writer.

#include "qlc/qlc_writer.h"
#include <cassert>
#include <iostream>
#include <fstream>
#include <cstring>
#include <cstdio>

using namespace diaglogger;

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

static ByteBuffer read_file_bytes(const std::string& path) {
    std::ifstream f(path, std::ios::binary);
    f.seekg(0, std::ios::end);
    size_t size = static_cast<size_t>(f.tellg());
    f.seekg(0, std::ios::beg);
    ByteBuffer data(size);
    f.read(reinterpret_cast<char*>(data.data()), static_cast<std::streamsize>(size));
    return data;
}

static uint32_t read_u32_le(const uint8_t* p) {
    return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8)
         | (static_cast<uint32_t>(p[2]) << 16) | (static_cast<uint32_t>(p[3]) << 24);
}

static uint16_t read_u16_le(const uint8_t* p) {
    return static_cast<uint16_t>(p[0]) | (static_cast<uint16_t>(p[1]) << 8);
}

// =====================================================================
// Default metadata (empty/zeroed)
// =====================================================================

TEST(write_defaults) {
    std::string path = "test_qlc_defaults.qlc";
    QlcMetadata meta;

    ASSERT_TRUE(QlcWriter::write(path, meta));

    ByteBuffer data = read_file_bytes(path);
    // Minimum: 4+6+1+4+4+1+2+4 = 26 bytes
    ASSERT_EQ(data.size(), 26u);

    size_t off = 0;
    ASSERT_EQ(read_u32_le(data.data() + off), 26u); off += 4;
    ASSERT_TRUE(memcmp(data.data() + off, "v0.0.1", 6) == 0); off += 6;
    ASSERT_EQ(data[off], 1); off += 1;  // HDLC default
    ASSERT_EQ(read_u32_le(data.data() + off), 0u); off += 4;  // GUIDListEntryCount
    ASSERT_EQ(read_u32_le(data.data() + off), 0u); off += 4;  // DiagIDEntryCount
    ASSERT_EQ(data[off], 0); off += 1;  // KeyCount
    ASSERT_EQ(read_u16_le(data.data() + off), 0u); off += 2;  // ATIDEntryCount
    ASSERT_EQ(read_u32_le(data.data() + off), 0u);  // DeviceInfoLength

    std::remove(path.c_str());
}

// =====================================================================
// Populated metadata
// =====================================================================

TEST(write_populated) {
    std::string path = "test_qlc_populated.qlc";
    QlcMetadata meta;

    meta.hdlc_data_type = 0;

    meta.diagid_table.valid = true;
    meta.diagid_table.entries = { { 1, "apps" }, { 2, "modem" } };

    meta.security_keys.valid = true;
    meta.security_keys.status = 0;
    SecurityKeyEntry key;
    key.version = 1; key.classifier = 2; key.public_type = 3; key.reserved = 0;
    key.public_len = 2; key.wrapped_len = 3;
    key.public_key = { 0xAA, 0xBB };
    key.wrapped_key = { 0xCC, 0xDD, 0xEE };
    meta.security_keys.keys.push_back(key);

    meta.qdss_hw_accel.stm_supported = true;
    meta.qdss_hw_accel.stm_atid = 16;

    meta.device_info.verno.raw = { 0x00, 0x41, 0x42 };
    meta.device_info.ext_build_id.raw = { 0x9F, 0x01 };
    meta.device_info.esn.raw = { 0x01, 0xEF, 0xBE, 0xAD, 0xDE };
    meta.device_info.status.raw = { 0x0C, 0xFF };

    ASSERT_TRUE(QlcWriter::write(path, meta));

    ByteBuffer data = read_file_bytes(path);
    size_t off = 0;

    // HeaderLength
    uint32_t header_len = read_u32_le(data.data() + off);
    ASSERT_EQ(header_len, static_cast<uint32_t>(data.size()));
    off += 4;

    // Version
    ASSERT_TRUE(memcmp(data.data() + off, "v0.0.1", 6) == 0); off += 6;

    // HDLCDataType = 0
    ASSERT_EQ(data[off], 0); off += 1;

    // GUIDListEntryCount = 0
    ASSERT_EQ(read_u32_le(data.data() + off), 0u); off += 4;

    // DiagIDEntryCount = 2
    ASSERT_EQ(read_u32_le(data.data() + off), 2u); off += 4;

    // DiagID entry 0
    ASSERT_EQ(data[off], 1); off += 1;
    ASSERT_TRUE(memcmp(data.data() + off, "apps", 4) == 0); off += 30;
    off += 16;  // guid zeros

    // DiagID entry 1
    ASSERT_EQ(data[off], 2); off += 1;
    ASSERT_TRUE(memcmp(data.data() + off, "modem", 5) == 0); off += 30;
    off += 16;  // guid zeros

    // KeyCount = 1
    ASSERT_EQ(data[off], 1); off += 1;

    // Key entry
    ASSERT_EQ(data[off], 1); off += 1;  // version
    ASSERT_EQ(data[off], 2); off += 1;  // classifier
    ASSERT_EQ(data[off], 3); off += 1;  // public_type
    ASSERT_EQ(data[off], 0); off += 1;  // reserved
    ASSERT_EQ(read_u16_le(data.data() + off), 2u); off += 2;  // public_len
    ASSERT_EQ(read_u16_le(data.data() + off), 3u); off += 2;  // wrapped_len
    ASSERT_EQ(data[off], 0xAA); off += 1;
    ASSERT_EQ(data[off], 0xBB); off += 1;
    ASSERT_EQ(data[off], 0xCC); off += 1;
    ASSERT_EQ(data[off], 0xDD); off += 1;
    ASSERT_EQ(data[off], 0xEE); off += 1;

    // ATIDEntryCount = 1
    ASSERT_EQ(read_u16_le(data.data() + off), 1u); off += 2;
    ASSERT_EQ(data[off], QdssHwAccelType::STM); off += 1;
    ASSERT_EQ(data[off], 16); off += 1;

    // DeviceInfoLength = 3 + 2 + 5 + 2 = 12
    ASSERT_EQ(read_u32_le(data.data() + off), 12u); off += 4;

    // DeviceInfo raw bytes
    ASSERT_EQ(data[off], 0x00);      // verno cmd
    ASSERT_EQ(data[off + 3], 0x9F);  // ext_build_id cmd
    ASSERT_EQ(data[off + 5], 0x01);  // esn cmd
    ASSERT_EQ(data[off + 10], 0x0C); // status cmd

    std::remove(path.c_str());
}

// =====================================================================
// Invalid path
// =====================================================================

TEST(write_invalid_path) {
    QlcMetadata meta;
    ASSERT_FALSE(QlcWriter::write("/nonexistent/dir/test.qlc", meta));
}

int main() {
    std::cout << "QLC Writer Tests:\n";
    std::cout << "\nResults: " << tests_passed << " passed, "
              << tests_failed << " failed\n";
    return tests_failed > 0 ? 1 : 0;
}
