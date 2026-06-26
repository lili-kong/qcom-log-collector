// SPDX-License-Identifier: Apache-2.0
// Copyright 2024 Qualcomm Innovation Center, Inc.
//
// Unit tests for ADPL command builders and packet decoder.

#include "diag/adpl_commands.h"
#include "diag/subsystem_commands.h"
#include <cassert>
#include <cstdio>
#include <cstring>

using namespace diaglogger;

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
    // subsys header(4) + dpl_command(1) + version(1) = 6 bytes
    CHECK(pkt.size() == 6);
    CHECK(pkt[0] == 0x4B);
    CHECK(pkt[1] == 0x5A);   // IPA subsys
    CHECK(pkt[2] == 0x00);   // 0x4000 low
    CHECK(pkt[3] == 0x40);   // 0x4000 high
    CHECK(pkt[4] == AdplDplCommand::ENABLE);   // 1
    CHECK(pkt[5] == AdplVersion::VERSION7);    // 7
    PASS("build_enable");
}

TEST(build_enable_default_version) {
    auto pkt = build_adpl_enable();  // default = HIGHEST_SUPPORTED = 7
    CHECK(pkt[5] == AdplVersion::HIGHEST_SUPPORTED);
    PASS("build_enable_default_version");
}

TEST(build_disable) {
    auto pkt = build_adpl_disable();
    CHECK(pkt.size() == 5);  // subsys header(4) + dpl_command(1)
    CHECK(pkt[0] == 0x4B);
    CHECK(pkt[1] == 0x5A);
    CHECK(pkt[2] == 0x00);   // 0x4000 low
    CHECK(pkt[3] == 0x40);   // 0x4000 high
    CHECK(pkt[4] == AdplDplCommand::DISABLE);  // 0
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
    CHECK(adpl_header_size(AdplVersion::VERSION1) == 4);
    CHECK(adpl_header_size(AdplVersion::VERSION2) == 4);
    CHECK(adpl_header_size(AdplVersion::VERSION3) == 8);
    CHECK(adpl_header_size(AdplVersion::VERSION4) == 12);
    CHECK(adpl_header_size(AdplVersion::VERSION5) == 16);
    CHECK(adpl_header_size(AdplVersion::VERSION6) == 20);
    CHECK(adpl_header_size(AdplVersion::VERSION7) == 24);
    CHECK(adpl_header_size(0) == 0);
    CHECK(adpl_header_size(99) == 0);
    PASS("header_sizes");
}

// --- Padding tests ---

TEST(padding_v1_v5_none) {
    // V1-V5 have no padding
    CHECK(adpl_padding(AdplVersion::VERSION1, 100) == 0);
    CHECK(adpl_padding(AdplVersion::VERSION3, 7) == 0);
    CHECK(adpl_padding(AdplVersion::VERSION5, 13) == 0);
    PASS("padding_v1_v5_none");
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
    // V1: [version=1][mux_id=0x02][length_lo=3][length_hi=0][payload: AA BB CC]
    uint8_t data[] = { 0x01, 0x02, 0x03, 0x00, 0xAA, 0xBB, 0xCC };
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
    // V3: [statusOpcode=0x10][mux_id=1][length=4 LE][seq=0x0001][pad=0x0000][payload: 01 02 03 04]
    uint8_t data[] = {
        0x10, 0x01,             // statusOpcode, mux_id
        0x04, 0x00,             // payload_len = 4 (LE)
        0x01, 0x00,             // sequence = 1
        0x00, 0x00,             // padding
        0x01, 0x02, 0x03, 0x04  // payload
    };
    auto pkt = decode_adpl_packet(AdplVersion::VERSION3, data, sizeof(data));
    CHECK(pkt.valid);
    CHECK(pkt.version == AdplVersion::VERSION3);
    CHECK(pkt.mux_id == 0x01);
    CHECK(pkt.sequence_number == 1);
    CHECK(pkt.diag_payload.size() == 4);
    CHECK(pkt.diag_payload[0] == 0x01);
    PASS("decode_v3_packet");
}

TEST(decode_v3_wrong_opcode) {
    // Wrong statusOpcode
    uint8_t data[] = { 0x0C, 0x01, 0x02, 0x00, 0x00, 0x00, 0x00, 0x00, 0xAA, 0xBB };
    auto pkt = decode_adpl_packet(AdplVersion::VERSION3, data, sizeof(data));
    CHECK(!pkt.valid);  // 0x0C is not valid for V3
    PASS("decode_v3_wrong_opcode");
}

// --- Decode V5 packet ---

TEST(decode_v5_packet) {
    // V5: 16-byte header + payload
    uint8_t data[] = {
        0x10, 0x00,                         // statusOpcode, mux_id
        0x02, 0x00,                         // payload_len = 2
        0x05, 0x00,                         // sequence = 5
        0x00, 0x00,                         // padding
        0x00, 0x10, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,  // timestamp = 4096
        0xFF, 0xEE                          // payload
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
    // V6: 20-byte header + payload + 8-byte aligned padding
    uint8_t data[28] = {0};
    data[0] = 0x0C;  // statusOpcode (V6/V7)
    data[1] = 0x03;  // mux_id
    data[2] = 0x05;  // payload_len = 5
    data[3] = 0x00;
    data[4] = 0x0A;  // sequence = 10
    data[5] = 0x00;
    // bytes 6-7: padding
    // bytes 8-15: timestamp = 0
    // bytes 16-19: reserved
    // payload starts at 20
    data[20] = 0x11;
    data[21] = 0x22;
    data[22] = 0x33;
    data[23] = 0x44;
    data[24] = 0x55;
    // padding bytes 25-27 to align to 8

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
    uint8_t data[24] = {0};
    data[0] = 0x10;  // V3-V5 opcode, not valid for V6
    data[2] = 0x01;
    auto pkt = decode_adpl_packet(AdplVersion::VERSION6, data, sizeof(data));
    CHECK(!pkt.valid);
    PASS("decode_v6_wrong_opcode");
}

// --- Decode V7 packet ---

TEST(decode_v7_packet) {
    // V7: 24-byte header + payload + 16-byte aligned padding
    uint8_t data[40] = {0};
    data[0] = 0x0D;  // statusOpcode (V6/V7 alternate)
    data[1] = 0x00;  // mux_id
    data[2] = 0x03;  // payload_len = 3
    data[3] = 0x00;
    data[4] = 0x64;  // sequence = 100
    data[5] = 0x00;
    // timestamp at 8-15
    data[8] = 0x01;  // timestamp = 1
    // reserved at 16-23
    // payload starts at 24
    data[24] = 0xDE;
    data[25] = 0xAD;
    data[26] = 0xBE;
    // padding to 16-byte boundary: need 13 more bytes padding

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
    uint8_t data[] = { 0x01, 0x00, 0x0A, 0x00 };  // payload_len=10
    size_t fs = adpl_frame_size(AdplVersion::VERSION1, data, sizeof(data));
    CHECK(fs == 4 + 10);  // header(4) + payload(10) + pad(0)
    PASS("frame_size_v1");
}

TEST(frame_size_v6_aligned) {
    uint8_t data[20] = {0};
    data[2] = 0x08;  // payload_len=8 (already 8-byte aligned)
    size_t fs = adpl_frame_size(AdplVersion::VERSION6, data, sizeof(data));
    CHECK(fs == 20 + 8 + 0);  // header(20) + payload(8) + pad(0)
    PASS("frame_size_v6_aligned");
}

TEST(frame_size_v6_unaligned) {
    uint8_t data[20] = {0};
    data[2] = 0x05;  // payload_len=5 → pad=3
    size_t fs = adpl_frame_size(AdplVersion::VERSION6, data, sizeof(data));
    CHECK(fs == 20 + 5 + 3);
    PASS("frame_size_v6_unaligned");
}

TEST(frame_size_v7_unaligned) {
    uint8_t data[24] = {0};
    data[2] = 0x03;  // payload_len=3 → pad=13
    size_t fs = adpl_frame_size(AdplVersion::VERSION7, data, sizeof(data));
    CHECK(fs == 24 + 3 + 13);
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
    // V1 header says payload=10, but only 2 bytes available after header
    uint8_t data[] = { 0x01, 0x00, 0x0A, 0x00, 0xFF, 0xFF };
    auto pkt = decode_adpl_packet(AdplVersion::VERSION1, data, sizeof(data));
    CHECK(!pkt.valid);
    PASS("decode_too_short_for_payload");
}

TEST(decode_zero_payload) {
    uint8_t data[] = { 0x01, 0x00, 0x00, 0x00 };  // payload_len=0
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
