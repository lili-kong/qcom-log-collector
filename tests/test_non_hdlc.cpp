// SPDX-License-Identifier: Apache-2.0
// Copyright 2024 Qualcomm Innovation Center, Inc.
//
// Unit tests for Non-HDLC encoder/decoder and Non-HDLC switch commands.

#include "hdlc/non_hdlc.h"
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
// Non-HDLC encoder
// =====================================================================

TEST(encode_empty_payload) {
    ByteBuffer payload;
    ByteBuffer frame = non_hdlc_encode(payload);
    // [0x7E][0x01][0x00][0x00][0x7E]
    ASSERT_EQ(frame.size(), 5u);
    ASSERT_EQ(frame[0], 0x7E);
    ASSERT_EQ(frame[1], 0x01);  // version
    ASSERT_EQ(frame[2], 0x00);  // len_lo
    ASSERT_EQ(frame[3], 0x00);  // len_hi
    ASSERT_EQ(frame[4], 0x7E);
}

TEST(encode_simple_payload) {
    ByteBuffer payload = { 0x4B, 0x12, 0x18, 0x02 };
    ByteBuffer frame = non_hdlc_encode(payload);
    // [0x7E][0x01][0x04][0x00][0x4B 0x12 0x18 0x02][0x7E]
    ASSERT_EQ(frame.size(), 9u);
    ASSERT_EQ(frame[0], 0x7E);
    ASSERT_EQ(frame[1], 0x01);
    ASSERT_EQ(frame[2], 0x04);  // len_lo = 4
    ASSERT_EQ(frame[3], 0x00);  // len_hi = 0
    ASSERT_EQ(frame[4], 0x4B);
    ASSERT_EQ(frame[5], 0x12);
    ASSERT_EQ(frame[6], 0x18);
    ASSERT_EQ(frame[7], 0x02);
    ASSERT_EQ(frame[8], 0x7E);
}

TEST(encode_payload_with_flag_byte) {
    // 0x7E in payload should NOT be escaped in non-HDLC
    ByteBuffer payload = { 0x7E, 0x00, 0x7E };
    ByteBuffer frame = non_hdlc_encode(payload);
    ASSERT_EQ(frame.size(), 8u);  // 4 header + 3 payload + 1 trailing
    ASSERT_EQ(frame[0], 0x7E);   // start flag
    ASSERT_EQ(frame[1], 0x01);   // version
    ASSERT_EQ(frame[2], 0x03);   // len = 3
    ASSERT_EQ(frame[3], 0x00);
    ASSERT_EQ(frame[4], 0x7E);   // payload byte (NOT a delimiter)
    ASSERT_EQ(frame[5], 0x00);
    ASSERT_EQ(frame[6], 0x7E);   // payload byte (NOT a delimiter)
    ASSERT_EQ(frame[7], 0x7E);   // trailing flag
}

TEST(encode_large_payload_length) {
    // 300-byte payload to test 16-bit length encoding
    ByteBuffer payload(300, 0xAA);
    ByteBuffer frame = non_hdlc_encode(payload);
    ASSERT_EQ(frame.size(), 305u);  // 4 + 300 + 1
    ASSERT_EQ(frame[2], 0x2C);     // 300 & 0xFF = 0x2C
    ASSERT_EQ(frame[3], 0x01);     // 300 >> 8 = 0x01
}

TEST(encode_raw_pointer_overload) {
    uint8_t data[] = { 0x0C };
    ByteBuffer frame = non_hdlc_encode(data, 1);
    ASSERT_EQ(frame.size(), 6u);
    ASSERT_EQ(frame[0], 0x7E);
    ASSERT_EQ(frame[1], 0x01);
    ASSERT_EQ(frame[2], 0x01);  // len = 1
    ASSERT_EQ(frame[3], 0x00);
    ASSERT_EQ(frame[4], 0x0C);  // payload
    ASSERT_EQ(frame[5], 0x7E);
}

// =====================================================================
// Non-HDLC decoder — basic
// =====================================================================

TEST(decode_single_frame) {
    NonHdlcDecoder decoder;
    // Feed: [0x7E][0x01][0x03][0x00][0xAA 0xBB 0xCC][0x7E]
    ByteBuffer wire = { 0x7E, 0x01, 0x03, 0x00, 0xAA, 0xBB, 0xCC, 0x7E };
    decoder.feed(wire);

    auto frame = decoder.next_frame();
    ASSERT_TRUE(frame.has_value());
    ASSERT_EQ(frame->size(), 3u);
    ASSERT_EQ((*frame)[0], 0xAA);
    ASSERT_EQ((*frame)[1], 0xBB);
    ASSERT_EQ((*frame)[2], 0xCC);

    // No more frames
    ASSERT_FALSE(decoder.next_frame().has_value());
    ASSERT_EQ(decoder.error_count(), 0u);
}

TEST(decode_round_trip) {
    // Encode then decode
    ByteBuffer original = { 0x4B, 0x12, 0x18, 0x02, 0x01, 0x00 };
    ByteBuffer wire = non_hdlc_encode(original);

    NonHdlcDecoder decoder;
    decoder.feed(wire);

    auto frame = decoder.next_frame();
    ASSERT_TRUE(frame.has_value());
    ASSERT_EQ(frame->size(), original.size());
    ASSERT_TRUE(*frame == original);
}

TEST(decode_round_trip_with_flag_bytes) {
    // Payload containing 0x7E should survive round-trip
    ByteBuffer original = { 0x7E, 0x01, 0x7E, 0x7E, 0x00 };
    ByteBuffer wire = non_hdlc_encode(original);

    NonHdlcDecoder decoder;
    decoder.feed(wire);

    auto frame = decoder.next_frame();
    ASSERT_TRUE(frame.has_value());
    ASSERT_TRUE(*frame == original);
}

TEST(decode_empty_payload) {
    NonHdlcDecoder decoder;
    // Empty payload: [0x7E][0x01][0x00][0x00][0x7E]
    ByteBuffer wire = { 0x7E, 0x01, 0x00, 0x00, 0x7E };
    decoder.feed(wire);

    auto frame = decoder.next_frame();
    ASSERT_TRUE(frame.has_value());
    ASSERT_EQ(frame->size(), 0u);
}

// =====================================================================
// Non-HDLC decoder — multi-frame
// =====================================================================

TEST(decode_multiple_frames) {
    NonHdlcDecoder decoder;
    // Two frames back-to-back (trailing 0x7E of first = leading 0x7E of second)
    ByteBuffer wire = {
        0x7E, 0x01, 0x02, 0x00, 0xAA, 0xBB,  // frame 1 (no trailing yet)
        0x7E, 0x01, 0x01, 0x00, 0xCC,          // frame 2 start (0x7E = end of frame 1)
        0x7E                                     // end of frame 2
    };
    decoder.feed(wire);

    auto f1 = decoder.next_frame();
    ASSERT_TRUE(f1.has_value());
    ASSERT_EQ(f1->size(), 2u);
    ASSERT_EQ((*f1)[0], 0xAA);
    ASSERT_EQ((*f1)[1], 0xBB);

    auto f2 = decoder.next_frame();
    ASSERT_TRUE(f2.has_value());
    ASSERT_EQ(f2->size(), 1u);
    ASSERT_EQ((*f2)[0], 0xCC);

    ASSERT_FALSE(decoder.next_frame().has_value());
}

TEST(decode_incremental_feed) {
    // Feed frame byte-by-byte
    NonHdlcDecoder decoder;
    ByteBuffer wire = { 0x7E, 0x01, 0x02, 0x00, 0xDD, 0xEE, 0x7E };

    for (uint8_t b : wire) {
        decoder.feed(&b, 1);
    }

    auto frame = decoder.next_frame();
    ASSERT_TRUE(frame.has_value());
    ASSERT_EQ(frame->size(), 2u);
    ASSERT_EQ((*frame)[0], 0xDD);
    ASSERT_EQ((*frame)[1], 0xEE);
}

// =====================================================================
// Non-HDLC decoder — QUTS test data validation
// =====================================================================

TEST(decode_quts_test_data) {
    // From QUTS Diag_UT.cpp NON_HDLC_BUFFER:
    // Frame 1: 7E 01 84 00 [132 bytes payload] 7E
    // Frame 2: 7E 01 65 00 [101 bytes payload] 7E
    // Frame 3: 7E 01 3F 00 [ 63 bytes payload] 7E
    //
    // Build synthetic frames matching these lengths.
    NonHdlcDecoder decoder;

    // Frame 1: 132 bytes payload
    ByteBuffer f1_payload(132, 0x11);
    ByteBuffer f1_wire = non_hdlc_encode(f1_payload);

    // Frame 2: 101 bytes payload
    ByteBuffer f2_payload(101, 0x22);
    ByteBuffer f2_wire = non_hdlc_encode(f2_payload);

    // Frame 3: 63 bytes payload
    ByteBuffer f3_payload(63, 0x33);
    ByteBuffer f3_wire = non_hdlc_encode(f3_payload);

    // Concatenate (the trailing 0x7E of one frame and leading 0x7E of next
    // can overlap, but non_hdlc_encode adds both, so the decoder sees
    // consecutive flags which is fine)
    ByteBuffer all_wire;
    all_wire.insert(all_wire.end(), f1_wire.begin(), f1_wire.end());
    all_wire.insert(all_wire.end(), f2_wire.begin(), f2_wire.end());
    all_wire.insert(all_wire.end(), f3_wire.begin(), f3_wire.end());

    decoder.feed(all_wire);

    auto r1 = decoder.next_frame();
    ASSERT_TRUE(r1.has_value());
    ASSERT_EQ(r1->size(), 132u);

    auto r2 = decoder.next_frame();
    ASSERT_TRUE(r2.has_value());
    ASSERT_EQ(r2->size(), 101u);

    auto r3 = decoder.next_frame();
    ASSERT_TRUE(r3.has_value());
    ASSERT_EQ(r3->size(), 63u);

    ASSERT_FALSE(decoder.next_frame().has_value());
    ASSERT_EQ(decoder.error_count(), 0u);
}

TEST(decode_quts_raw_frame_header) {
    // Verify decoder handles the exact header bytes from QUTS test data:
    // 7E 01 84 00 = flag, version=1, len=0x0084=132
    NonHdlcDecoder decoder;
    ByteBuffer wire;
    wire.push_back(0x7E);
    wire.push_back(0x01);
    wire.push_back(0x84);
    wire.push_back(0x00);
    // 132 bytes of payload
    for (int i = 0; i < 132; ++i) {
        wire.push_back(static_cast<uint8_t>(i & 0xFF));
    }
    wire.push_back(0x7E);

    decoder.feed(wire);
    auto frame = decoder.next_frame();
    ASSERT_TRUE(frame.has_value());
    ASSERT_EQ(frame->size(), 132u);
    ASSERT_EQ((*frame)[0], 0x00);
    ASSERT_EQ((*frame)[131], 131 & 0xFF);
}

// =====================================================================
// Non-HDLC decoder — error handling
// =====================================================================

TEST(decode_invalid_version) {
    NonHdlcDecoder decoder;
    // Version 0x02 is not recognized
    ByteBuffer wire = { 0x7E, 0x02, 0x01, 0x00, 0xAA, 0x7E };
    decoder.feed(wire);

    ASSERT_FALSE(decoder.next_frame().has_value());
    ASSERT_EQ(decoder.error_count(), 1u);
}

TEST(decode_garbage_before_frame) {
    NonHdlcDecoder decoder;
    // Garbage bytes, then valid frame
    ByteBuffer wire = { 0xFF, 0x00, 0x42,
                         0x7E, 0x01, 0x01, 0x00, 0xAA, 0x7E };
    decoder.feed(wire);

    auto frame = decoder.next_frame();
    ASSERT_TRUE(frame.has_value());
    ASSERT_EQ(frame->size(), 1u);
    ASSERT_EQ((*frame)[0], 0xAA);
    ASSERT_EQ(decoder.error_count(), 0u);
}

TEST(decode_consecutive_flags) {
    NonHdlcDecoder decoder;
    // Multiple consecutive 0x7E then a valid frame
    ByteBuffer wire = { 0x7E, 0x7E, 0x7E, 0x01, 0x01, 0x00, 0xBB, 0x7E };
    decoder.feed(wire);

    auto frame = decoder.next_frame();
    ASSERT_TRUE(frame.has_value());
    ASSERT_EQ(frame->size(), 1u);
    ASSERT_EQ((*frame)[0], 0xBB);
}

TEST(decode_reset) {
    NonHdlcDecoder decoder;
    // Start a partial frame
    ByteBuffer partial = { 0x7E, 0x01, 0x05, 0x00, 0xAA };
    decoder.feed(partial);
    ASSERT_FALSE(decoder.next_frame().has_value());

    // Reset
    decoder.reset();

    // Feed a complete frame
    ByteBuffer complete = { 0x7E, 0x01, 0x01, 0x00, 0xBB, 0x7E };
    decoder.feed(complete);

    auto frame = decoder.next_frame();
    ASSERT_TRUE(frame.has_value());
    ASSERT_EQ(frame->size(), 1u);
    ASSERT_EQ((*frame)[0], 0xBB);
}

// =====================================================================
// Non-HDLC switch command builder/parser
// =====================================================================

TEST(non_hdlc_switch_request_format) {
    ByteBuffer pkt = build_non_hdlc_switch_request();
    ASSERT_EQ(pkt.size(), 5u);  // 4 header + 1 payload (version)
    ASSERT_EQ(pkt[0], 0x4B);   // SUBSYS cmd
    ASSERT_EQ(pkt[1], 0x12);   // DIAG_SERV
    // subcmd = 0x0218 little-endian
    ASSERT_EQ(pkt[2], 0x18);
    ASSERT_EQ(pkt[3], 0x02);
    ASSERT_EQ(pkt[4], 0x01);   // version=1
}

TEST(non_hdlc_switch_response_success) {
    // Confirmed from QUTS emulator: after subsys header, payload is [0x01][0x00]
    ByteBuffer payload = { 0x01, 0x00 };  // version=1, result=0 (success)
    auto result = parse_non_hdlc_switch_response(payload);
    ASSERT_TRUE(result.valid);
    ASSERT_TRUE(result.ok());
    ASSERT_EQ(result.framing_version, 1);
    ASSERT_EQ(result.result_code, 0);
}

TEST(non_hdlc_switch_response_failure) {
    ByteBuffer payload = { 0x01, 0x01 };  // version=1, result=1 (error)
    auto result = parse_non_hdlc_switch_response(payload);
    ASSERT_TRUE(result.valid);
    ASSERT_FALSE(result.ok());
}

TEST(non_hdlc_switch_response_wrong_version) {
    ByteBuffer payload = { 0x02, 0x00 };  // version=2, result=0
    auto result = parse_non_hdlc_switch_response(payload);
    ASSERT_TRUE(result.valid);
    ASSERT_FALSE(result.ok());  // ok() requires version==1
}

TEST(non_hdlc_switch_response_empty) {
    ByteBuffer payload;
    auto result = parse_non_hdlc_switch_response(payload);
    ASSERT_FALSE(result.valid);
}

TEST(non_hdlc_switch_response_short) {
    ByteBuffer payload = { 0x01 };  // only 1 byte, need 2
    auto result = parse_non_hdlc_switch_response(payload);
    ASSERT_FALSE(result.valid);
}

int main() {
    std::cout << "Non-HDLC Tests:\n";
    std::cout << "\nResults: " << tests_passed << " passed, "
              << tests_failed << " failed\n";
    return tests_failed > 0 ? 1 : 0;
}
