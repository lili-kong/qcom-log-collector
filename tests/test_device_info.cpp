// Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
// SPDX-License-Identifier: BSD 3-Clause Clear License
//
// Unit tests for device info command builders and response parsers.

#include "diag/handshake_commands.h"
#include "diag/commands.h"
#include <cassert>
#include <iostream>
#include <cstring>

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

// =====================================================================
// Command builders
// =====================================================================

TEST(build_verno_request) {
    ByteBuffer pkt = build_verno_request();
    ASSERT_EQ(pkt.size(), 1u);
    ASSERT_EQ(pkt[0], DiagCmd::VERNO);
}

TEST(build_esn_request) {
    ByteBuffer pkt = build_esn_request();
    ASSERT_EQ(pkt.size(), 1u);
    ASSERT_EQ(pkt[0], DiagCmd::ESN);
}

// =====================================================================
// VERNO response parser
// =====================================================================

TEST(parse_verno_valid) {
    ByteBuffer resp;
    resp.push_back(0x00);  // cmd byte
    const char* date = "Jan 15 2024";
    resp.insert(resp.end(), date, date + 11);
    const char* time = "12:30:45";
    resp.insert(resp.end(), time, time + 8);
    const char* rdate = "Feb 20 2024";
    resp.insert(resp.end(), rdate, rdate + 11);
    const char* rtime = "09:15:00";
    resp.insert(resp.end(), rtime, rtime + 8);
    const char* vdir = "BLDPATH\0";
    resp.insert(resp.end(), vdir, vdir + 8);
    resp.push_back(2);    // station_class_mark
    resp.push_back(6);    // mobile_cai
    resp.push_back(10);   // manufacturers_model
    resp.push_back(0x34); // firmware_revision LE = 0x1234
    resp.push_back(0x12);
    resp.push_back(3);    // slot_cycle_index
    resp.push_back(0xAB); // msm_revision LE = 0xCDAB
    resp.push_back(0xCD);

    auto result = parse_verno_response(resp);
    ASSERT_TRUE(result.valid);
    ASSERT_TRUE(result.compile_date == "Jan 15 2024");
    ASSERT_TRUE(result.compile_time == "12:30:45");
    ASSERT_TRUE(result.release_date == "Feb 20 2024");
    ASSERT_TRUE(result.release_time == "09:15:00");
    ASSERT_TRUE(result.version_directory == "BLDPATH");
    ASSERT_EQ(result.station_class_mark, 2);
    ASSERT_EQ(result.mobile_cai, 6);
    ASSERT_EQ(result.manufacturers_model, 10);
    ASSERT_EQ(result.firmware_revision, 0x1234);
    ASSERT_EQ(result.slot_cycle_index, 3);
    ASSERT_EQ(result.msm_revision, 0xCDAB);
}

TEST(parse_verno_too_short) {
    ByteBuffer resp = { 0x00, 0x01, 0x02 };
    auto result = parse_verno_response(resp);
    ASSERT_FALSE(result.valid);
}

TEST(parse_verno_wrong_cmd) {
    ByteBuffer resp(55, 0);
    resp[0] = 0xFF;
    auto result = parse_verno_response(resp);
    ASSERT_FALSE(result.valid);
}

// =====================================================================
// EXT_BUILD_ID response parser
// =====================================================================

TEST(parse_ext_build_id_valid) {
    ByteBuffer resp;
    resp.push_back(0x9F);  // cmd byte
    resp.push_back(0x02);  // version
    for (int i = 0; i < 16; ++i) resp.push_back(0xAA);  // reserved
    resp.push_back(0x78); resp.push_back(0x56);  // msm_revision LE
    resp.push_back(0x34); resp.push_back(0x12);
    resp.push_back(0x42); resp.push_back(0x00);  // model_number LE
    resp.push_back(0x00); resp.push_back(0x00);
    const char* sw = "MPSS.HO.1.0";
    resp.insert(resp.end(), sw, sw + strlen(sw) + 1);
    const char* model = "SDX75";
    resp.insert(resp.end(), model, model + strlen(model) + 1);

    auto result = parse_ext_build_id_response(resp);
    ASSERT_TRUE(result.valid);
    ASSERT_EQ(result.version, 2);
    ASSERT_EQ(result.reserved.size(), 16u);
    ASSERT_EQ(result.msm_revision, 0x12345678u);
    ASSERT_EQ(result.model_number, 0x42u);
    ASSERT_TRUE(result.software_revision == "MPSS.HO.1.0");
    ASSERT_TRUE(result.model_name == "SDX75");
}

TEST(parse_ext_build_id_too_short) {
    ByteBuffer resp(10, 0);
    resp[0] = 0x9F;
    auto result = parse_ext_build_id_response(resp);
    ASSERT_FALSE(result.valid);
}

// =====================================================================
// ESN response parser
// =====================================================================

TEST(parse_esn_valid) {
    ByteBuffer resp = { 0x01, 0xEF, 0xBE, 0xAD, 0xDE };
    auto result = parse_esn_response(resp);
    ASSERT_TRUE(result.valid);
    ASSERT_EQ(result.esn, 0xDEADBEEFu);
}

TEST(parse_esn_too_short) {
    ByteBuffer resp = { 0x01, 0x02, 0x03 };
    auto result = parse_esn_response(resp);
    ASSERT_FALSE(result.valid);
}

TEST(parse_esn_wrong_cmd) {
    ByteBuffer resp = { 0xFF, 0x00, 0x00, 0x00, 0x00 };
    auto result = parse_esn_response(resp);
    ASSERT_FALSE(result.valid);
}

// =====================================================================
// STATUS response parser
// =====================================================================

TEST(parse_status_valid) {
    ByteBuffer resp = { 0x0C, 0x01, 0x02, 0x03, 0x04 };
    auto result = parse_status_response(resp);
    ASSERT_TRUE(result.valid);
    ASSERT_EQ(result.raw.size(), 5u);
}

TEST(parse_status_wrong_cmd) {
    ByteBuffer resp = { 0xFF, 0x01 };
    auto result = parse_status_response(resp);
    ASSERT_FALSE(result.valid);
}

TEST(parse_status_empty) {
    ByteBuffer resp;
    auto result = parse_status_response(resp);
    ASSERT_FALSE(result.valid);
}

int main() {
    std::cout << "Device Info Tests:\n";
    std::cout << "\nResults: " << tests_passed << " passed, "
              << tests_failed << " failed\n";
    return tests_failed > 0 ? 1 : 0;
}
