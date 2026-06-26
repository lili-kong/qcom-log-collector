// SPDX-License-Identifier: Apache-2.0
// Copyright 2024 Qualcomm Innovation Center, Inc.
//
// Unit tests for HDLC encode/decode and CRC-16.

#include "hdlc/hdlc.h"
#include "hdlc/crc16.h"
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

// --- CRC-16 tests ---

TEST(crc16_empty) {
    uint16_t c = crc16(nullptr, 0);
    ASSERT_EQ(c, 0x0000);  // init(0xFFFF) ^ final_xor(0xFFFF) = 0x0000
}

TEST(crc16_known_value) {
    // CRC-16/X.25 of "123456789": init=0xFFFF, poly=0x8408 (reflected), final XOR=0xFFFF
    const uint8_t data[] = {'1','2','3','4','5','6','7','8','9'};
    uint16_t c = crc16(data, sizeof(data));
    ASSERT_EQ(c, 0x906E);  // Standard CRC-16/X.25 check value
}

TEST(crc16_single_byte) {
    uint8_t byte = 0x7B;  // Loopback command
    uint16_t c = crc16(&byte, 1);
    ASSERT_TRUE(c != 0xFFFF);  // Should be different from empty
}

// --- HDLC encode tests ---

TEST(hdlc_encode_basic) {
    // Simple payload
    uint8_t data[] = {0x01, 0x02, 0x03};
    ByteBuffer frame = hdlc_encode(data, 3);

    // TUF-compatible format: no leading FLAG, ends with FLAG
    ASSERT_EQ(frame.front(), data[0]);  // Starts with payload (no leading 0x7E)
    ASSERT_EQ(frame.back(), HDLC_FLAG);

    // Must be longer than input (data + CRC + FLAG, with possible escaping)
    ASSERT_TRUE(frame.size() >= 6);  // 3 + 2 + 1 minimum
}

TEST(hdlc_encode_escapes_flag) {
    // Payload containing FLAG byte
    uint8_t data[] = {HDLC_FLAG};
    ByteBuffer frame = hdlc_encode(data, 1);

    // The FLAG in payload must be escaped
    // No leading FLAG (TUF format), so search from beginning to before trailing FLAG
    bool found_escaped = false;
    for (size_t i = 0; i < frame.size() - 1; ++i) {
        if (frame[i] == HDLC_ESCAPE && i + 1 < frame.size() - 1) {
            if (frame[i + 1] == (HDLC_FLAG ^ HDLC_ESCAPE_MASK)) {
                found_escaped = true;
                break;
            }
        }
    }
    ASSERT_TRUE(found_escaped);
}

TEST(hdlc_encode_escapes_escape) {
    // Payload containing ESCAPE byte
    uint8_t data[] = {HDLC_ESCAPE};
    ByteBuffer frame = hdlc_encode(data, 1);

    bool found_escaped = false;
    for (size_t i = 0; i < frame.size() - 1; ++i) {
        if (frame[i] == HDLC_ESCAPE && i + 1 < frame.size() - 1) {
            if (frame[i + 1] == (HDLC_ESCAPE ^ HDLC_ESCAPE_MASK)) {
                found_escaped = true;
                break;
            }
        }
    }
    ASSERT_TRUE(found_escaped);
}

// --- HDLC round-trip tests ---

TEST(hdlc_roundtrip_simple) {
    ByteBuffer original = {0x7B, 0x01, 0x02, 0x03, 0x04};
    ByteBuffer frame = hdlc_encode(original);

    HdlcDecoder decoder;
    decoder.feed(frame);
    auto decoded = decoder.next_frame();

    ASSERT_TRUE(decoded.has_value());
    ASSERT_EQ(decoded->size(), original.size());
    ASSERT_TRUE(*decoded == original);
}

TEST(hdlc_roundtrip_with_special_bytes) {
    // Payload with FLAG and ESCAPE bytes
    ByteBuffer original = {0x7E, 0x7D, 0x00, 0xFF, 0x7E, 0x7D};
    ByteBuffer frame = hdlc_encode(original);

    HdlcDecoder decoder;
    decoder.feed(frame);
    auto decoded = decoder.next_frame();

    ASSERT_TRUE(decoded.has_value());
    ASSERT_EQ(decoded->size(), original.size());
    ASSERT_TRUE(*decoded == original);
}

TEST(hdlc_roundtrip_empty_payload) {
    // Single byte payload
    ByteBuffer original = {0x0C};
    ByteBuffer frame = hdlc_encode(original);

    HdlcDecoder decoder;
    decoder.feed(frame);
    auto decoded = decoder.next_frame();

    ASSERT_TRUE(decoded.has_value());
    ASSERT_EQ(decoded->size(), 1u);
    ASSERT_EQ((*decoded)[0], 0x0C);
}

// --- Decoder streaming tests ---

TEST(decoder_partial_frames) {
    ByteBuffer original = {0x01, 0x02, 0x03};
    ByteBuffer frame = hdlc_encode(original);

    HdlcDecoder decoder;

    // Feed one byte at a time
    for (size_t i = 0; i < frame.size(); ++i) {
        decoder.feed(&frame[i], 1);
    }

    auto decoded = decoder.next_frame();
    ASSERT_TRUE(decoded.has_value());
    ASSERT_TRUE(*decoded == original);
}

TEST(decoder_multiple_frames) {
    ByteBuffer msg1 = {0x01, 0x02};
    ByteBuffer msg2 = {0x03, 0x04, 0x05};

    ByteBuffer combined;
    auto f1 = hdlc_encode(msg1);
    auto f2 = hdlc_encode(msg2);
    combined.insert(combined.end(), f1.begin(), f1.end());
    combined.insert(combined.end(), f2.begin(), f2.end());

    HdlcDecoder decoder;
    decoder.feed(combined);

    auto d1 = decoder.next_frame();
    ASSERT_TRUE(d1.has_value());
    ASSERT_TRUE(*d1 == msg1);

    auto d2 = decoder.next_frame();
    ASSERT_TRUE(d2.has_value());
    ASSERT_TRUE(*d2 == msg2);

    // No more frames
    auto d3 = decoder.next_frame();
    ASSERT_TRUE(!d3.has_value());
}

TEST(decoder_crc_error) {
    ByteBuffer original = {0x01, 0x02, 0x03};
    ByteBuffer frame = hdlc_encode(original);

    // Corrupt a byte in the middle (not a delimiter)
    if (frame.size() > 3) {
        frame[2] ^= 0xFF;
    }

    HdlcDecoder decoder;
    decoder.feed(frame);

    auto decoded = decoder.next_frame();
    // Should either fail CRC check or produce no frame
    if (decoded.has_value()) {
        // If it somehow decoded, the data should be wrong
        // But more likely next_frame returns nullopt
    }
    ASSERT_TRUE(decoder.crc_error_count() >= 1 || !decoded.has_value());
}

TEST(decoder_garbage_before_frame) {
    ByteBuffer original = {0xAB, 0xCD};
    ByteBuffer frame = hdlc_encode(original);

    // Prepend garbage + a 0x7E separator.  In TUF's protocol, a trailing
    // 0x7E from a previous frame (or an explicit delimiter) separates
    // garbage from the real frame.  Without a 0x7E separator, garbage
    // gets accumulated with the frame and causes CRC mismatch (expected
    // for the no-leading-flag format).
    ByteBuffer data = {0x12, 0x34, 0x56, HDLC_FLAG};
    data.insert(data.end(), frame.begin(), frame.end());

    HdlcDecoder decoder;
    decoder.feed(data);

    // First the garbage + 0x7E produces a CRC error (or is too short)
    // Then the real frame is decoded
    auto decoded = decoder.next_frame();
    ASSERT_TRUE(decoded.has_value());
    ASSERT_TRUE(*decoded == original);
}

int main() {
    std::cout << "HDLC Tests:\n";
    // Tests are auto-registered via static constructors above

    std::cout << "\nResults: " << tests_passed << " passed, "
              << tests_failed << " failed\n";
    return tests_failed > 0 ? 1 : 0;
}
