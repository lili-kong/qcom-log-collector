// Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
// SPDX-License-Identifier: BSD 3-Clause Clear License
//
// Unit tests for handshake command builders and response parsers.

#include "diag/handshake_commands.h"
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
// Security key request
// =====================================================================

TEST(security_key_exact_bytes) {
    // Must produce exact 8-byte sequence from QUTS Diag.cpp:376
    ByteBuffer pkt = build_security_key_request();
    ASSERT_EQ(pkt.size(), 8u);
    ASSERT_EQ(pkt[0], 0x4B);  // SUBSYS cmd
    ASSERT_EQ(pkt[1], 0x12);  // DIAG_SERV subsystem
    ASSERT_EQ(pkt[2], 0x33);  // subcmd lo (0x0833)
    ASSERT_EQ(pkt[3], 0x08);  // subcmd hi
    ASSERT_EQ(pkt[4], 0x01);  // payload
    ASSERT_EQ(pkt[5], 0x00);
    ASSERT_EQ(pkt[6], 0x00);
    ASSERT_EQ(pkt[7], 0x00);
}

TEST(security_key_subsystem_validation) {
    ByteBuffer pkt = build_security_key_request();
    ASSERT_TRUE(is_valid_subsystem_response(pkt, SubsysId::DIAG_SERV, HandshakeCmd::SECURITY_KEY_LIST));
}

// =====================================================================
// Feature query
// =====================================================================

TEST(feature_query_header) {
    ByteBuffer pkt = build_feature_query();
    ASSERT_TRUE(pkt.size() >= 4);
    ASSERT_EQ(pkt[0], 0x4B);  // SUBSYS
    ASSERT_EQ(pkt[1], 0x12);  // DIAG_SERV
    // subcmd = FEATURE_QUERY (0x0225) little-endian
    ASSERT_EQ(pkt[2], 0x25);
    ASSERT_EQ(pkt[3], 0x02);
}

TEST(feature_query_payload) {
    ByteBuffer pkt = build_feature_query();
    ASSERT_EQ(pkt.size(), 9u);  // 4 header + 5 payload
    ASSERT_EQ(pkt[4], 0x01);  // version
    ASSERT_EQ(pkt[5], 0xFF);  // feature mask (query all)
    ASSERT_EQ(pkt[6], 0xFF);
    ASSERT_EQ(pkt[7], 0xFF);
    ASSERT_EQ(pkt[8], 0xFF);
}

// =====================================================================
// DiagID feature enable (hypervisor enable)
// =====================================================================

TEST(diagid_feature_enable_both) {
    ByteBuffer pkt = build_diagid_feature_enable(HypervisorPacketFormat::BOTH);
    ASSERT_EQ(pkt.size(), 6u);  // 4 header + 2 payload
    ASSERT_EQ(pkt[0], 0x4B);
    ASSERT_EQ(pkt[1], 0x12);
    // subcmd = 0x0232 little-endian
    ASSERT_EQ(pkt[2], 0x32);
    ASSERT_EQ(pkt[3], 0x02);
    ASSERT_EQ(pkt[4], 0x01);  // version
    ASSERT_EQ(pkt[5], HypervisorPacketFormat::BOTH);  // mask=3
}

TEST(diagid_feature_enable_cmd_only) {
    ByteBuffer pkt = build_diagid_feature_enable(HypervisorPacketFormat::CMD);
    ASSERT_EQ(pkt[5], HypervisorPacketFormat::CMD);  // mask=1
}

TEST(diagid_feature_enable_default_mask) {
    // Default parameter should be BOTH
    ByteBuffer pkt = build_diagid_feature_enable();
    ASSERT_EQ(pkt[5], HypervisorPacketFormat::BOTH);
}

// =====================================================================
// DiagID priority config
// =====================================================================

TEST(diagid_priority_config_set) {
    ByteBuffer pkt = build_diagid_priority_config(DiagIdPriorityOp::SET, 1);
    ASSERT_EQ(pkt.size(), 7u);  // 4 header + 3 payload
    ASSERT_EQ(pkt[0], 0x4B);
    ASSERT_EQ(pkt[1], 0x12);
    // subcmd = 0x0234 little-endian
    ASSERT_EQ(pkt[2], 0x34);
    ASSERT_EQ(pkt[3], 0x02);
    ASSERT_EQ(pkt[4], 0x01);  // version
    ASSERT_EQ(pkt[5], DiagIdPriorityOp::SET);  // opcode=1
    ASSERT_EQ(pkt[6], 0x01);  // diag_id=1
}

TEST(diagid_priority_config_get) {
    ByteBuffer pkt = build_diagid_priority_config(DiagIdPriorityOp::GET, 5);
    ASSERT_EQ(pkt[5], DiagIdPriorityOp::GET);  // opcode=0
    ASSERT_EQ(pkt[6], 0x05);  // diag_id=5
}

TEST(diagid_priority_config_clear) {
    ByteBuffer pkt = build_diagid_priority_config(DiagIdPriorityOp::CLEAR, 2);
    ASSERT_EQ(pkt[5], DiagIdPriorityOp::CLEAR);  // opcode=2
    ASSERT_EQ(pkt[6], 0x02);  // diag_id=2
}

// =====================================================================
// DiagID registration table
// =====================================================================

TEST(diagid_registration_table_format) {
    ByteBuffer pkt = build_diagid_registration_table();
    ASSERT_EQ(pkt.size(), 4u);  // header only, no payload
    ASSERT_EQ(pkt[0], 0x4B);
    ASSERT_EQ(pkt[1], 0x12);
    // subcmd = 0x0233 little-endian
    ASSERT_EQ(pkt[2], 0x33);
    ASSERT_EQ(pkt[3], 0x02);
}

// =====================================================================
// DiagID table query
// =====================================================================

TEST(diagid_table_query_default_version) {
    ByteBuffer pkt = build_diagid_table_query();
    ASSERT_EQ(pkt.size(), 5u);  // 4 header + 1 payload
    // subcmd = 0x0222 little-endian
    ASSERT_EQ(pkt[2], 0x22);
    ASSERT_EQ(pkt[3], 0x02);
    ASSERT_EQ(pkt[4], 0x01);  // default version=1
}

TEST(diagid_table_query_explicit_version) {
    ByteBuffer pkt = build_diagid_table_query(2);
    ASSERT_EQ(pkt[4], 0x02);  // version=2
}

// =====================================================================
// Response parsers — feature query
// =====================================================================

TEST(parse_feature_query_success) {
    // [1] version=1, [4] feature_mask=0x03 (hypervisor + diagid)
    ByteBuffer payload = { 0x01, 0x03, 0x00, 0x00, 0x00 };
    auto result = parse_feature_query_response(payload);
    ASSERT_TRUE(result.valid);
    ASSERT_EQ(result.version, 1);
    ASSERT_EQ(result.feature_mask, 3u);
    ASSERT_TRUE(result.supports_hypervisor());
    ASSERT_TRUE(result.supports_diagid());
}

TEST(parse_feature_query_no_features) {
    ByteBuffer payload = { 0x01, 0x00, 0x00, 0x00, 0x00 };
    auto result = parse_feature_query_response(payload);
    ASSERT_TRUE(result.valid);
    ASSERT_FALSE(result.supports_hypervisor());
    ASSERT_FALSE(result.supports_diagid());
}

TEST(parse_feature_query_empty) {
    ByteBuffer payload;
    auto result = parse_feature_query_response(payload);
    ASSERT_FALSE(result.valid);
}

// =====================================================================
// Response parsers — security key
// =====================================================================

TEST(parse_security_key_success) {
    ByteBuffer payload = { 0x00 };  // status=0 success
    auto result = parse_security_key_response(payload);
    ASSERT_TRUE(result.valid);
    ASSERT_TRUE(result.ok());
}

TEST(parse_security_key_failure) {
    ByteBuffer payload = { 0x01 };  // status=1 error
    auto result = parse_security_key_response(payload);
    ASSERT_TRUE(result.valid);
    ASSERT_FALSE(result.ok());
}

TEST(parse_security_key_empty) {
    ByteBuffer payload;
    auto result = parse_security_key_response(payload);
    ASSERT_FALSE(result.valid);
}

// =====================================================================
// Response parsers — hypervisor enable
// =====================================================================

TEST(parse_hypervisor_enable_success) {
    // [1] version=1, [1] mask=3, [1] status=0
    ByteBuffer payload = { 0x01, 0x03, 0x00 };
    auto result = parse_hypervisor_enable_response(payload);
    ASSERT_TRUE(result.valid);
    ASSERT_TRUE(result.ok());
    ASSERT_EQ(result.version, 1);
    ASSERT_EQ(result.packet_format_mask, 3);
    ASSERT_EQ(result.status, 0);
}

TEST(parse_hypervisor_enable_error) {
    ByteBuffer payload = { 0x01, 0x03, 0x01 };  // status=1 error
    auto result = parse_hypervisor_enable_response(payload);
    ASSERT_TRUE(result.valid);
    ASSERT_FALSE(result.ok());
}

TEST(parse_hypervisor_enable_short) {
    ByteBuffer payload = { 0x01, 0x03 };  // too short (need 3 bytes)
    auto result = parse_hypervisor_enable_response(payload);
    ASSERT_FALSE(result.valid);
}

// =====================================================================
// Response parsers — DiagID priority config
// =====================================================================

TEST(parse_diagid_priority_success) {
    // [1] version=1, [1] opcode=SET(1), [1] diag_id=1, [1] status=SUCCESS(0)
    ByteBuffer payload = { 0x01, 0x01, 0x01, 0x00 };
    auto result = parse_diagid_priority_response(payload);
    ASSERT_TRUE(result.valid);
    ASSERT_TRUE(result.ok());
    ASSERT_EQ(result.version, 1);
    ASSERT_EQ(result.opcode, 1);
    ASSERT_EQ(result.diag_id, 1);
}

TEST(parse_diagid_priority_invalid_diag_id) {
    ByteBuffer payload = { 0x01, 0x01, 0x05, 0x04 };  // status=INVALID_DIAG_ID
    auto result = parse_diagid_priority_response(payload);
    ASSERT_TRUE(result.valid);
    ASSERT_FALSE(result.ok());
    ASSERT_EQ(result.status, DiagIdPriorityStatus::INVALID_DIAG_ID);
}

TEST(parse_diagid_priority_short) {
    ByteBuffer payload = { 0x01, 0x01, 0x01 };  // only 3 bytes, need 4
    auto result = parse_diagid_priority_response(payload);
    ASSERT_FALSE(result.valid);
}

// =====================================================================
// Response parsers — DiagID table
// =====================================================================

TEST(parse_diagid_table_single_entry) {
    // [1] version=1, [1] num_entries=1, [1] diag_id=1, "apps\0"
    ByteBuffer payload = { 0x01, 0x01, 0x01, 'a', 'p', 'p', 's', 0x00 };
    auto result = parse_diagid_table_response(payload);
    ASSERT_TRUE(result.valid);
    ASSERT_EQ(result.version, 1);
    ASSERT_EQ(result.entries.size(), 1u);
    ASSERT_EQ(result.entries[0].diag_id, 1);
    ASSERT_TRUE(result.entries[0].processor_name == "apps");
}

TEST(parse_diagid_table_multiple_entries) {
    // version=1, num=2, {id=1,"apps\0"}, {id=2,"modem\0"}
    ByteBuffer payload = { 0x01, 0x02,
        0x01, 'a', 'p', 'p', 's', 0x00,
        0x02, 'm', 'o', 'd', 'e', 'm', 0x00 };
    auto result = parse_diagid_table_response(payload);
    ASSERT_TRUE(result.valid);
    ASSERT_EQ(result.entries.size(), 2u);
    ASSERT_EQ(result.entries[0].diag_id, 1);
    ASSERT_TRUE(result.entries[0].processor_name == "apps");
    ASSERT_EQ(result.entries[1].diag_id, 2);
    ASSERT_TRUE(result.entries[1].processor_name == "modem");
}

TEST(parse_diagid_table_empty) {
    ByteBuffer payload;
    auto result = parse_diagid_table_response(payload);
    ASSERT_FALSE(result.valid);
}

TEST(parse_diagid_table_zero_entries) {
    ByteBuffer payload = { 0x01, 0x00 };  // version=1, num=0
    auto result = parse_diagid_table_response(payload);
    ASSERT_TRUE(result.valid);
    ASSERT_EQ(result.entries.size(), 0u);
}

int main() {
    std::cout << "Handshake Command Tests:\n";
    std::cout << "\nResults: " << tests_passed << " passed, "
              << tests_failed << " failed\n";
    return tests_failed > 0 ? 1 : 0;
}
