// Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
// SPDX-License-Identifier: BSD 3-Clause Clear License
//
// Unit tests for ADPL command builders and packet decoder.

#include <cassert>
#include <cstdio>
#include <cstring>

#include "diag/adpl_commands.h"
#include "diag/subsystem_commands.h"

using namespace QC;

static int tests_passed = 0;
static int tests_failed = 0;

#define TEST(name) \
    static void test_##name(); \
    struct test_reg_##name { test_reg_##name() { test_##name(); } } test_inst_##name; \
    static void test_##name()

#define CHECK(cond) do { \
    if (!(cond)) { \
        std::fprintf(stderr, "  FAIL: %s (line %d)\n", #cond, __LINE__); \
        tests_failed++; \
        return; \
    } \
} while(0)

#define PASS(name) do { \
    std::printf("  PASS: %s\n", name); \
    tests_passed++; \
} while(0)

// --- Command builder tests ---

TEST(build_enable) {
    auto pkt = build_adpl_enable(AdplVersion::VERSION7);
    // subsys header(4) + payload(6): dplVersion, packetLimit, maxPacketSize(2 LE),
    // dplCommand, reserved -- mirrors AdplDiagManager::configureAdpl FieldValueList.
    CHECK(pkt.size() == 10);
    CHECK(pkt[0] == 0x4B);                    // cmd_id = SUBSYS
    CHECK(pkt[1] == SubsysId::IPA);           // IPA subsystem id
    CHECK(pkt[2] == 0x00);                    // cmd 0x4000 low
    CHECK(pkt[3] == 0x40);                    // cmd 0x4000 high
    CHECK(pkt[4] == AdplVersion::VERSION7);   // dplVersion
    CHECK(pkt[5] == 0x00);                    // packetLimit (0 = enable)
    CHECK(pkt[6] == 0x00);                    // maxPacketSize low  (15360 = 0x3C00)
    CHECK(pkt[7] == 0x3C);                    // maxPacketSize high
    CHECK(pkt[8] == AdplDplCommand::ENABLE);  // dplCommand = 1
    CHECK(pkt[9] == 0x00);                    // reserved
    PASS("build_enable");
}

TEST(build_enable_default_version) {
    auto pkt = build_adpl_enable();  // default = HIGHEST_SUPPORTED = 7
    CHECK(pkt[4] == AdplVersion::HIGHEST_SUPPORTED);
    PASS("build_enable_default_version");
}

TEST(build_disable) {
    auto pkt = build_adpl_disable();
    // subsys header(4) + payload(6) -- mirrors AdplDiagManager::disableAdpl FieldValueList.
    CHECK(pkt.size() == 10);
    CHECK(pkt[0] == 0x4B);
    CHECK(pkt[1] == SubsysId::IPA);
    CHECK(pkt[2] == 0x00);                     // 0x4000 low
    CHECK(pkt[3] == 0x40);                     // 0x4000 high
    CHECK(pkt[4] == AdplVersion::HIGHEST_SUPPORTED);
    CHECK(pkt[5] == 0x04);                     // packetLimit = 4 (disable)
    CHECK(pkt[8] == AdplDplCommand::DISABLE);  // dplCommand = 0
    PASS("build_disable");
}

// --- Enable response parser tests ---

TEST(parse_enable_response_success) {
    ByteBuffer payload = { 0x01, 0x05 };  // status=1 (success), version=5
    auto resp = parse_adpl_enable_response(payload);
    CHECK(resp.status == 1);
    CHECK(resp.version == 5);
    CHECK(resp.ok());
    PASS("parse_enable_response_success");
}

TEST(parse_enable_response_failure) {
    ByteBuffer payload = { 0x00, 0x03 };  // status=0 (fail)
    auto resp = parse_adpl_enable_response(payload);
    CHECK(resp.status == 0);
    CHECK(!resp.ok());
    PASS("parse_enable_response_failure");
}

TEST(parse_enable_response_too_short) {
    ByteBuffer payload = { 0x01 };
    auto resp = parse_adpl_enable_response(payload);
    CHECK(resp.status == 1);
    CHECK(resp.version == 0);
    CHECK(!resp.ok());  // version 0 is invalid
    PASS("parse_enable_response_too_short");
}

TEST(parse_enable_response_empty) {
    ByteBuffer payload;
    auto resp = parse_adpl_enable_response(payload);
    CHECK(resp.status == 0);
    CHECK(resp.version == 0);
    CHECK(!resp.ok());
    PASS("parse_enable_response_empty");
}

// --- Header size tests ---

TEST(header_sizes) {
    // Sizes fixed by TOOLS_STATIC_ASSERTs in workspace Frame.h (V1 from AdplV1Header).
    CHECK(adpl_header_size(AdplVersion::VERSION1) == 4);
    CHECK(adpl_header_size(AdplVersion::VERSION2) == 32);
    CHECK(adpl_header_size(AdplVersion::VERSION3) == 16);
    CHECK(adpl_header_size(AdplVersion::VERSION4) == 16);
    CHECK(adpl_header_size(AdplVersion::VERSION5) == 16);
    CHECK(adpl_header_size(AdplVersion::VERSION6) == 16);
    CHECK(adpl_header_size(AdplVersion::VERSION7) == 16);
    CHECK(adpl_header_size(0) == 0);
    CHECK(adpl_header_size(99) == 0);
    PASS("header_sizes");
}

// --- Padding tests ---

TEST(padding_v1_none) {
    // V1 carries its pad count in the header (m_padCd); no alignment padding.
    CHECK(adpl_padding(AdplVersion::VERSION1, 100) == 0);
    CHECK(adpl_padding(AdplVersion::VERSION1, 7) == 0);
    PASS("padding_v1_none");
}

TEST(padding_v2_4byte) {
    // V2: pad to 32-bit (4-byte) boundary
    CHECK(adpl_padding(AdplVersion::VERSION2, 8) == 0);
    CHECK(adpl_padding(AdplVersion::VERSION2, 4) == 0);
    CHECK(adpl_padding(AdplVersion::VERSION2, 1) == 3);
    CHECK(adpl_padding(AdplVersion::VERSION2, 5) == 3);
    CHECK(adpl_padding(AdplVersion::VERSION2, 6) == 2);
    PASS("padding_v2_4byte");
}

TEST(padding_v3_v5_8byte) {
    // V3/V4/V5: pad to 8-byte boundary
    CHECK(adpl_padding(AdplVersion::VERSION3, 8) == 0);
    CHECK(adpl_padding(AdplVersion::VERSION3, 7) == 1);
    CHECK(adpl_padding(AdplVersion::VERSION4, 5) == 3);
    CHECK(adpl_padding(AdplVersion::VERSION5, 13) == 3);
    PASS("padding_v3_v5_8byte");
}

TEST(padding_v6_8byte) {
    // V6: 8-byte aligned
    CHECK(adpl_padding(AdplVersion::VERSION6, 8) == 0);
    CHECK(adpl_padding(AdplVersion::VERSION6, 16) == 0);
    CHECK(adpl_padding(AdplVersion::VERSION6, 1) == 7);
    CHECK(adpl_padding(AdplVersion::VERSION6, 5) == 3);
    CHECK(adpl_padding(AdplVersion::VERSION6, 7) == 1);
    PASS("padding_v6_8byte");
}

TEST(padding_v7_16byte) {
    // V7: 16-byte aligned
    CHECK(adpl_padding(AdplVersion::VERSION7, 16) == 0);
    CHECK(adpl_padding(AdplVersion::VERSION7, 32) == 0);
    CHECK(adpl_padding(AdplVersion::VERSION7, 1) == 15);
    CHECK(adpl_padding(AdplVersion::VERSION7, 10) == 6);
    CHECK(adpl_padding(AdplVersion::VERSION7, 15) == 1);
    PASS("padding_v7_16byte");
}

// --- Decode V1 packet ---

TEST(decode_v1_packet) {
    // AdplV1Header: [0] padCd:6|reserved:2 (0), [1] muxId=0x02,
    // [2..3] packetLength = 3 (BIG-endian: 0x00,0x03), [payload: AA BB CC]
    uint8_t data[] = { 0x00, 0x02, 0x00, 0x03, 0xAA, 0xBB, 0xCC };
    auto pkt = decode_adpl_packet(AdplVersion::VERSION1, data, sizeof(data));
    CHECK(pkt.valid);
    CHECK(pkt.version == AdplVersion::VERSION1);
    CHECK(pkt.mux_id == 0x02);
    CHECK(pkt.diag_payload.size() == 3);
    CHECK(pkt.diag_payload[0] == 0xAA);
    CHECK(pkt.diag_payload[1] == 0xBB);
    CHECK(pkt.diag_payload[2] == 0xCC);
    PASS("decode_v1_packet");
}

// --- Decode V3 packet ---

TEST(decode_v3_packet) {
    // AdplV3Header (16B): [0] statusOpcode=0x10, [1] sequenceNumber=7, [5] muxId=1,
    // [8..13] dplTimeOfDay=0, [14..15] packetLength=4 (LE), [payload: 01 02 03 04]
    uint8_t data[] = {
        0x10, 0x07,             // statusOpcode, sequenceNumber
        0x00, 0x00,             // endPointSrcIndex/reserved1
        0x00, 0x01,             // reserved2, muxId
        0x00, 0x00,             // reserved3
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00,  // dplTimeOfDay (48b) = 0
        0x04, 0x00,             // packetLength = 4 (LE)
        0x01, 0x02, 0x03, 0x04  // payload
    };
    auto pkt = decode_adpl_packet(AdplVersion::VERSION3, data, sizeof(data));
    CHECK(pkt.valid);
    CHECK(pkt.version == AdplVersion::VERSION3);
    CHECK(pkt.mux_id == 0x01);
    CHECK(pkt.sequence_number == 7);
    CHECK(pkt.diag_payload.size() == 4);
    CHECK(pkt.diag_payload[0] == 0x01);
    PASS("decode_v3_packet");
}

TEST(decode_v3_wrong_opcode) {
    // statusOpcode 0x0C is not valid for V3 (must be 0x10)
    uint8_t data[16] = {0};
    data[0] = 0x0C;
    data[14] = 0x01;  // packetLength = 1
    auto pkt = decode_adpl_packet(AdplVersion::VERSION3, data, sizeof(data));
    CHECK(!pkt.valid);
    PASS("decode_v3_wrong_opcode");
}

// --- Decode V5 packet ---

TEST(decode_v5_packet) {
    // AdplV5Header (16B): [0] statusOpcode=0x10, [1] sequenceNumber=5, [5] muxId,
    // [8..13] dplTimeOfDay=4096, [14..15] packetLength=2 (LE), [payload: FF EE]
    uint8_t data[] = {
        0x10, 0x05,             // statusOpcode, sequenceNumber
        0x00, 0x00,             // endPointSrcIndex/reserved1
        0x00, 0x00,             // reserved2, muxId
        0x00, 0x00,             // reserved3
        0x00, 0x10, 0x00, 0x00, 0x00, 0x00,  // dplTimeOfDay (48b) = 0x1000 = 4096
        0x02, 0x00,             // packetLength = 2 (LE)
        0xFF, 0xEE              // payload
    };
    auto pkt = decode_adpl_packet(AdplVersion::VERSION5, data, sizeof(data));
    CHECK(pkt.valid);
    CHECK(pkt.version == AdplVersion::VERSION5);
    CHECK(pkt.sequence_number == 5);
    CHECK(pkt.timestamp_us == 4096);
    CHECK(pkt.diag_payload.size() == 2);
    CHECK(pkt.diag_payload[0] == 0xFF);
    CHECK(pkt.diag_payload[1] == 0xEE);
    PASS("decode_v5_packet");
}

// --- Decode V6 packet ---

TEST(decode_v6_packet) {
    // AdplV6Header (16B) + payload(5) + 8-byte aligned padding(3) = 24
    uint8_t data[24] = {0};
    data[0]  = 0x0C;  // statusOpcode (low nibble); timeOfDay high nibble = 0
    data[1]  = 0x0A;  // sequenceNumber = 10
    data[5]  = 0x03;  // muxId = 3
    // bytes 8-13: dplTimeOfDay = 0
    data[14] = 0x05;  // packetLength = 5 (LE)
    data[15] = 0x00;
    // payload starts at 16
    data[16] = 0x11;
    data[17] = 0x22;
    data[18] = 0x33;
    data[19] = 0x44;
    data[20] = 0x55;
    // padding bytes 21-23 to align to 8

    auto pkt = decode_adpl_packet(AdplVersion::VERSION6, data, sizeof(data));
    CHECK(pkt.valid);
    CHECK(pkt.version == AdplVersion::VERSION6);
    CHECK(pkt.mux_id == 0x03);
    CHECK(pkt.sequence_number == 10);
    CHECK(pkt.diag_payload.size() == 5);
    CHECK(pkt.diag_payload[0] == 0x11);
    CHECK(pkt.diag_payload[4] == 0x55);
    PASS("decode_v6_packet");
}

TEST(decode_v6_wrong_opcode) {
    // Low nibble of byte 0 must be 0x0C or 0x0D; 0x10 -> nibble 0x00 -> invalid.
    uint8_t data[16] = {0};
    data[0]  = 0x10;
    data[14] = 0x01;
    auto pkt = decode_adpl_packet(AdplVersion::VERSION6, data, sizeof(data));
    CHECK(!pkt.valid);
    PASS("decode_v6_wrong_opcode");
}

// --- Decode V7 packet ---

TEST(decode_v7_packet) {
    // AdplV6Header (16B, V6==V7) + payload(3) + 16-byte aligned padding(13) = 32
    uint8_t data[32] = {0};
    data[0]  = 0x0D;  // statusOpcode (low nibble); timeOfDay high nibble = 0
    data[1]  = 0x64;  // sequenceNumber = 100
    data[5]  = 0x00;  // muxId = 0
    data[8]  = 0x01;  // dplTimeOfDay low byte = 1 -> timestamp = 1
    data[14] = 0x03;  // packetLength = 3 (LE)
    data[15] = 0x00;
    // payload starts at 16
    data[16] = 0xDE;
    data[17] = 0xAD;
    data[18] = 0xBE;
    // padding to 16-byte boundary (13 bytes)

    auto pkt = decode_adpl_packet(AdplVersion::VERSION7, data, sizeof(data));
    CHECK(pkt.valid);
    CHECK(pkt.version == AdplVersion::VERSION7);
    CHECK(pkt.mux_id == 0x00);
    CHECK(pkt.sequence_number == 100);
    CHECK(pkt.timestamp_us == 1);
    CHECK(pkt.diag_payload.size() == 3);
    CHECK(pkt.diag_payload[0] == 0xDE);
    CHECK(pkt.diag_payload[1] == 0xAD);
    CHECK(pkt.diag_payload[2] == 0xBE);
    PASS("decode_v7_packet");
}

// --- Frame size tests ---

TEST(frame_size_v1) {
    // AdplV1Header: packetLength big-endian at [2..3] = 10; padCd = 0.
    uint8_t data[] = { 0x00, 0x00, 0x00, 0x0A };
    size_t fs = adpl_frame_size(AdplVersion::VERSION1, data, sizeof(data));
    CHECK(fs == 4 + 10);  // header(4) + payload(10) + pad(0)
    PASS("frame_size_v1");
}

TEST(frame_size_v6_aligned) {
    uint8_t data[16] = {0};
    data[14] = 0x08;  // packetLength=8 (already 8-byte aligned)
    size_t fs = adpl_frame_size(AdplVersion::VERSION6, data, sizeof(data));
    CHECK(fs == 16 + 8 + 0);  // header(16) + payload(8) + pad(0)
    PASS("frame_size_v6_aligned");
}

TEST(frame_size_v6_unaligned) {
    uint8_t data[16] = {0};
    data[14] = 0x05;  // packetLength=5 → pad=3
    size_t fs = adpl_frame_size(AdplVersion::VERSION6, data, sizeof(data));
    CHECK(fs == 16 + 5 + 3);
    PASS("frame_size_v6_unaligned");
}

TEST(frame_size_v7_unaligned) {
    uint8_t data[16] = {0};
    data[14] = 0x03;  // packetLength=3 → pad=13
    size_t fs = adpl_frame_size(AdplVersion::VERSION7, data, sizeof(data));
    CHECK(fs == 16 + 3 + 13);
    PASS("frame_size_v7_unaligned");
}

TEST(frame_size_too_short) {
    uint8_t data[] = { 0x01 };
    CHECK(adpl_frame_size(AdplVersion::VERSION1, data, 1) == 0);
    CHECK(adpl_frame_size(AdplVersion::VERSION7, data, 1) == 0);
    PASS("frame_size_too_short");
}

TEST(frame_size_invalid_version) {
    uint8_t data[4] = {0};
    CHECK(adpl_frame_size(0, data, 4) == 0);
    CHECK(adpl_frame_size(99, data, 4) == 0);
    PASS("frame_size_invalid_version");
}

// --- Decode edge cases ---

TEST(decode_too_short_for_header) {
    uint8_t data[2] = {0};
    auto pkt = decode_adpl_packet(AdplVersion::VERSION5, data, sizeof(data));
    CHECK(!pkt.valid);
    PASS("decode_too_short_for_header");
}

TEST(decode_too_short_for_payload) {
    // V1 header says payload=10 (big-endian), but only 2 bytes available after header
    uint8_t data[] = { 0x00, 0x00, 0x00, 0x0A, 0xFF, 0xFF };
    auto pkt = decode_adpl_packet(AdplVersion::VERSION1, data, sizeof(data));
    CHECK(!pkt.valid);
    PASS("decode_too_short_for_payload");
}

TEST(decode_zero_payload) {
    uint8_t data[] = { 0x00, 0x00, 0x00, 0x00 };  // packetLength=0, padCd=0
    auto pkt = decode_adpl_packet(AdplVersion::VERSION1, data, sizeof(data));
    CHECK(pkt.valid);
    CHECK(pkt.diag_payload.empty());
    PASS("decode_zero_payload");
}

TEST(decode_invalid_version) {
    uint8_t data[4] = {0};
    auto pkt = decode_adpl_packet(0, data, sizeof(data));
    CHECK(!pkt.valid);
    auto pkt2 = decode_adpl_packet(99, data, sizeof(data));
    CHECK(!pkt2.valid);
    PASS("decode_invalid_version");
}

int main() {
    std::printf("=== ADPL Command Tests ===\n");
    std::printf("\nResults: %d passed, %d failed\n", tests_passed, tests_failed);
    return tests_failed > 0 ? 1 : 0;
}
