// Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
// SPDX-License-Identifier: BSD 3-Clause Clear License
//
// Unit tests for handshake command builders and response parsers.

#include <cassert>
#include <cstring>
#include <iostream>

#include "diag/handshake_commands.h"

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

// =====================================================================
// Security key request
// =====================================================================

TEST(security_key_exact_bytes) {
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

// =====================================================================
// QShrink4 download builders
// =====================================================================

TEST(qshrink_file_open_exact_bytes) {
    std::array<uint8_t, 16> guid{};
    for (int i = 0; i < 16; ++i) guid[i] = static_cast<uint8_t>(i + 1);
    ByteBuffer pkt = build_qshrink_file_open(HandshakeCmd::QSR4_FILE_LIST_APPS, 2, guid);
    // header: 0x80 (SUBSYS_V2), 0x12, subcmd LE (0x020F -> 0F 02)
    ASSERT_EQ(pkt.size(), 4u + 4u + 16u);
    ASSERT_EQ(pkt[0], 0x80);
    ASSERT_EQ(pkt[1], 0x12);
    ASSERT_EQ(pkt[2], 0x0F);
    ASSERT_EQ(pkt[3], 0x02);
    ASSERT_EQ(pkt[4], 0x02); ASSERT_EQ(pkt[5], 0x00);  // version=2 LE
    ASSERT_EQ(pkt[6], 0x01); ASSERT_EQ(pkt[7], 0x00);  // op=FILE_OPEN LE
    ASSERT_EQ(pkt[8], 0x01);   // guid[0]
    ASSERT_EQ(pkt[23], 0x10);  // guid[15]
}

TEST(qshrink_file_read_exact_bytes) {
    ByteBuffer pkt = build_qshrink_file_read(HandshakeCmd::QSR4_FILE_LIST_APPS, 2,
                                             0x1234, 4000, 8000);
    ASSERT_EQ(pkt.size(), 4u + 4u + 2u + 4u + 4u);
    ASSERT_EQ(pkt[4], 0x02); ASSERT_EQ(pkt[5], 0x00);  // version
    ASSERT_EQ(pkt[6], 0x02); ASSERT_EQ(pkt[7], 0x00);  // op=FILE_READ
    ASSERT_EQ(pkt[8], 0x34); ASSERT_EQ(pkt[9], 0x12);  // fd LE
    ASSERT_EQ(pkt[10], 0xA0); ASSERT_EQ(pkt[11], 0x0F); ASSERT_EQ(pkt[12], 0x00); ASSERT_EQ(pkt[13], 0x00);  // length=4000 LE
    ASSERT_EQ(pkt[14], 0x40); ASSERT_EQ(pkt[15], 0x1F); ASSERT_EQ(pkt[16], 0x00); ASSERT_EQ(pkt[17], 0x00);  // offset=8000 LE
}

TEST(qshrink_file_close_exact_bytes) {
    ByteBuffer pkt = build_qshrink_file_close(HandshakeCmd::QSR4_FILE_LIST_APPS, 2, 0x1234);
    ASSERT_EQ(pkt.size(), 4u + 4u + 2u);
    ASSERT_EQ(pkt[6], 0x03); ASSERT_EQ(pkt[7], 0x00);  // op=FILE_CLOSE
    ASSERT_EQ(pkt[8], 0x34); ASSERT_EQ(pkt[9], 0x12);  // fd LE
}

// Build a SUBSYS_V2 response frame: [0x80][11-byte SubsystemV2Record][4-byte ver/op]
// followed by op_payload. Header bytes are filler; parsers key off QSR4_V2_OP_OFFSET.
static ByteBuffer make_v2_response(const ByteBuffer& op_payload) {
    ByteBuffer resp(QSR4_V2_OP_OFFSET, 0x00);
    resp[0] = 0x80;  // SUBSYS_V2
    resp.insert(resp.end(), op_payload.begin(), op_payload.end());
    return resp;
}

TEST(qshrink_file_open_parse) {
    // op payload: [guid:16][fd=0x1234 LE][status=0]
    ByteBuffer op(16, 0xAA);   // guid
    op.push_back(0x34); op.push_back(0x12);  // fd LE
    op.push_back(0x00);                       // status
    QshrinkFileOpenResult r = parse_qshrink_file_open_response(make_v2_response(op));
    ASSERT_TRUE(r.valid);
    ASSERT_TRUE(r.ok());
    ASSERT_EQ(r.file_descriptor, 0x1234);
}

TEST(qshrink_file_open_parse_bad_status) {
    ByteBuffer op(16, 0x00);   // guid
    op.push_back(0x00); op.push_back(0x00);  // fd
    op.push_back(0x05);                       // status = 5
    QshrinkFileOpenResult r = parse_qshrink_file_open_response(make_v2_response(op));
    ASSERT_TRUE(r.valid);
    ASSERT_FALSE(r.ok());
    ASSERT_EQ(r.status, 5);
}

TEST(qshrink_file_read_parse) {
    // op payload: [fd:2][offset:4][numBytes=3 LE:4][status=0][data: AA BB CC]
    ByteBuffer op = { 0x00, 0x00,               // fd
                      0x00, 0x00, 0x00, 0x00,   // offset
                      0x03, 0x00, 0x00, 0x00,   // numBytes = 3
                      0x00,                     // status
                      0xAA, 0xBB, 0xCC };       // data
    QshrinkFileReadResult r = parse_qshrink_file_read_response(make_v2_response(op));
    ASSERT_TRUE(r.ok());
    ASSERT_EQ(r.data.size(), 3u);
    ASSERT_EQ(r.data[0], 0xAA);
    ASSERT_EQ(r.data[2], 0xCC);
}

int main() {
    std::cout << "Handshake Command Tests:\n";
    std::cout << "\nResults: " << tests_passed << " passed, "
              << tests_failed << " failed\n";
    return tests_failed > 0 ? 1 : 0;
}
