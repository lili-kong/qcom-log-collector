// Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
// SPDX-License-Identifier: BSD 3-Clause Clear License
//
// Unit tests for diag command builders.

#include <cassert>
#include <cstring>
#include <iostream>

#include "diag/commands.h"

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

// --- STATUS ---

TEST(status_request_format) {
    ByteBuffer pkt = build_status_request();
    ASSERT_EQ(pkt.size(), 1u);
    ASSERT_EQ(pkt[0], DiagCmd::STATUS);
}

// --- EXT_BUILD_ID ---

TEST(ext_build_id_request_format) {
    ByteBuffer pkt = build_ext_build_id_request();
    ASSERT_EQ(pkt.size(), 1u);
    ASSERT_EQ(pkt[0], DiagCmd::EXT_BUILD_ID);
}

// --- Loopback ---

TEST(loopback_cmd_id) {
    ByteBuffer pkt = build_loopback_request();
    ASSERT_TRUE(!pkt.empty());
    ASSERT_EQ(pkt[0], DiagCmd::LOOPBACK);
    ASSERT_TRUE(pkt.size() >= 2);  // cmd + at least 1 payload byte
}

// --- EVENT_RPT ---

TEST(event_report_enable) {
    ByteBuffer pkt = build_event_report(true);
    ASSERT_EQ(pkt.size(), 2u);
    ASSERT_EQ(pkt[0], DiagCmd::EVENT_RPT);
    ASSERT_EQ(pkt[1], 0x01);
}

TEST(event_report_disable) {
    ByteBuffer pkt = build_event_report(false);
    ASSERT_EQ(pkt.size(), 2u);
    ASSERT_EQ(pkt[0], DiagCmd::EVENT_RPT);
    ASSERT_EQ(pkt[1], 0x00);
}

// --- LOG_CONFIG disable ---

TEST(log_config_disable) {
    ByteBuffer cmd = build_log_config_disable(3);
    ASSERT_EQ(cmd[0], DiagCmd::LOG_CONFIG);
    // operation = DISABLE_ALL (0x00)
    ASSERT_EQ(cmd[4], LogConfigOp::DISABLE_ALL);
    // equipment_id = 3
    ASSERT_EQ(cmd[8], 0x03);
}

// --- Response validation ---

TEST(valid_response_check) {
    ByteBuffer resp = {DiagCmd::LOG_CONFIG, 0x00, 0x00};
    ASSERT_TRUE(is_valid_response(resp, DiagCmd::LOG_CONFIG));
    ASSERT_TRUE(!is_valid_response(resp, DiagCmd::LOOPBACK));
}

TEST(empty_response_invalid) {
    ByteBuffer resp;
    ASSERT_TRUE(!is_valid_response(resp, DiagCmd::LOG_CONFIG));
}

int main() {
    std::cout << "Command Builder Tests:\n";
    std::cout << "\nResults: " << tests_passed << " passed, "
              << tests_failed << " failed\n";
    return tests_failed > 0 ? 1 : 0;
}
