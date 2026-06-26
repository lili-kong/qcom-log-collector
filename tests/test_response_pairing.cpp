// SPDX-License-Identifier: Apache-2.0
// Copyright 2024 Qualcomm Innovation Center, Inc.
//
// Unit tests for request/response signature pairing.

#include "diag/response_pairing.h"
#include <iostream>
#include <string>
#include <stdexcept>

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

// --- compute_response_signature ---

TEST(diag_signature_is_one_byte) {
    ByteBuffer sig = compute_response_signature({0x0C, 0x00, 0x00});
    ASSERT_EQ(sig.size(), 1u);
    ASSERT_EQ(static_cast<int>(sig[0]), 0x0C);
}

TEST(subsystem_signature_is_four_bytes) {
    ByteBuffer sig = compute_response_signature({0x4B, 0x12, 0x33, 0x08, 0xAA, 0xBB});
    ASSERT_EQ(sig.size(), 4u);
    ASSERT_EQ(static_cast<int>(sig[0]), 0x4B);
    ASSERT_EQ(static_cast<int>(sig[1]), 0x12);
    ASSERT_EQ(static_cast<int>(sig[2]), 0x33);
    ASSERT_EQ(static_cast<int>(sig[3]), 0x08);
}

TEST(subsystem_v2_signature_is_four_bytes) {
    ByteBuffer sig = compute_response_signature({0x80, 0x12, 0x21, 0x02, 0x99});
    ASSERT_EQ(sig.size(), 4u);
    ASSERT_EQ(static_cast<int>(sig[0]), 0x80);
    ASSERT_EQ(static_cast<int>(sig[3]), 0x02);
}

TEST(empty_payload_gives_empty_signature) {
    ByteBuffer sig = compute_response_signature({});
    ASSERT_TRUE(sig.empty());
}

// --- response_matches_signature ---

TEST(diag_match_succeeds) {
    ByteBuffer sig = {0x0C};
    ByteBuffer frame = {0x0C, 0xAB, 0xCD};
    ASSERT_TRUE(response_matches_signature(sig, frame));
}

TEST(diag_mismatch_fails) {
    ByteBuffer sig = {0x0C};
    ByteBuffer frame = {0x60, 0x00};
    ASSERT_TRUE(!response_matches_signature(sig, frame));
}

TEST(subsystem_match_succeeds) {
    ByteBuffer sig = {0x4B, 0x12, 0x33, 0x08};
    ByteBuffer frame = {0x4B, 0x12, 0x33, 0x08, 0x01, 0x02, 0x03};
    ASSERT_TRUE(response_matches_signature(sig, frame));
}

TEST(subsystem_subcmd_mismatch_fails) {
    ByteBuffer sig = {0x4B, 0x12, 0x33, 0x08};
    ByteBuffer frame = {0x4B, 0x12, 0x99, 0x99, 0x01};
    ASSERT_TRUE(!response_matches_signature(sig, frame));
}

TEST(empty_signature_never_matches) {
    ByteBuffer sig = {};
    ByteBuffer frame = {0x0C};
    ASSERT_TRUE(!response_matches_signature(sig, frame));
}

TEST(short_frame_never_matches) {
    ByteBuffer sig = {0x4B, 0x12, 0x33, 0x08};
    ByteBuffer frame = {0x4B, 0x12};
    ASSERT_TRUE(!response_matches_signature(sig, frame));
}

int main() {
    std::cout << "Response Pairing Tests:\n";
    std::cout << "\nResults: " << tests_passed << " passed, "
              << tests_failed << " failed\n";
    return tests_failed > 0 ? 1 : 0;
}
