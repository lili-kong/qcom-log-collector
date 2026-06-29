// Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
// SPDX-License-Identifier: BSD 3-Clause Clear License
//
// Unit tests for generic subsystem command builder.

#include "diag/subsystem_commands.h"
#include <cassert>
#include <cstdio>

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

// --- Tests ---

TEST(build_subsystem_command_no_payload) {
    auto pkt = build_subsystem_command(SubsysId::DIAG_SERV, 0x0214);
    CHECK(pkt.size() == 4);
    CHECK(pkt[0] == DiagCmd::SUBSYS);      // 0x4B
    CHECK(pkt[1] == SubsysId::DIAG_SERV);  // 0x12
    CHECK(pkt[2] == 0x14);                  // cmd low byte
    CHECK(pkt[3] == 0x02);                  // cmd high byte
    PASS("build_subsystem_command_no_payload");
}

TEST(build_subsystem_command_with_payload) {
    ByteBuffer payload = {0x01, 0x02, 0x03};
    auto pkt = build_subsystem_command(SubsysId::QDSS, 0x0101, payload);
    CHECK(pkt.size() == 7);
    CHECK(pkt[0] == 0x4B);
    CHECK(pkt[1] == 0x5A);
    CHECK(pkt[2] == 0x01);  // cmd low
    CHECK(pkt[3] == 0x01);  // cmd high
    CHECK(pkt[4] == 0x01);
    CHECK(pkt[5] == 0x02);
    CHECK(pkt[6] == 0x03);
    PASS("build_subsystem_command_with_payload");
}

TEST(build_subsystem_command_raw_payload) {
    uint8_t raw[] = {0xAA, 0xBB};
    auto pkt = build_subsystem_command(SubsysId::IPA, 0x4000, raw, 2);
    CHECK(pkt.size() == 6);
    CHECK(pkt[0] == 0x4B);
    CHECK(pkt[1] == 0x5A);   // IPA == QDSS == 0x5A
    CHECK(pkt[2] == 0x00);   // 0x4000 low
    CHECK(pkt[3] == 0x40);   // 0x4000 high
    CHECK(pkt[4] == 0xAA);
    CHECK(pkt[5] == 0xBB);
    PASS("build_subsystem_command_raw_payload");
}

TEST(is_valid_subsystem_response_good) {
    ByteBuffer resp = {0x4B, 0x12, 0x14, 0x02, 0x00, 0x01};
    CHECK(is_valid_subsystem_response(resp, 0x12, 0x0214));
    PASS("is_valid_subsystem_response_good");
}

TEST(is_valid_subsystem_response_wrong_cmd) {
    ByteBuffer resp = {0x4B, 0x12, 0x14, 0x02, 0x00};
    CHECK(!is_valid_subsystem_response(resp, 0x12, 0x0215));  // wrong subcmd
    PASS("is_valid_subsystem_response_wrong_cmd");
}

TEST(is_valid_subsystem_response_wrong_subsys) {
    ByteBuffer resp = {0x4B, 0x5A, 0x01, 0x01, 0x00};
    CHECK(!is_valid_subsystem_response(resp, 0x12, 0x0101));  // wrong subsys
    PASS("is_valid_subsystem_response_wrong_subsys");
}

TEST(is_valid_subsystem_response_too_short) {
    ByteBuffer resp = {0x4B, 0x12};
    CHECK(!is_valid_subsystem_response(resp, 0x12, 0x0214));
    PASS("is_valid_subsystem_response_too_short");
}

TEST(extract_subsystem_payload) {
    ByteBuffer resp = {0x4B, 0x12, 0x14, 0x02, 0xAA, 0xBB, 0xCC};
    auto payload = extract_subsystem_payload(resp);
    CHECK(payload.size() == 3);
    CHECK(payload[0] == 0xAA);
    CHECK(payload[1] == 0xBB);
    CHECK(payload[2] == 0xCC);
    PASS("extract_subsystem_payload");
}

TEST(extract_subsystem_payload_empty) {
    ByteBuffer resp = {0x4B, 0x12, 0x14, 0x02};
    auto payload = extract_subsystem_payload(resp);
    CHECK(payload.empty());
    PASS("extract_subsystem_payload_empty");
}

TEST(extract_subsystem_payload_too_short) {
    ByteBuffer resp = {0x4B};
    auto payload = extract_subsystem_payload(resp);
    CHECK(payload.empty());
    PASS("extract_subsystem_payload_too_short");
}

int main() {
    std::printf("=== Subsystem Command Tests ===\n");
    // Tests are auto-run by static constructors
    std::printf("\nResults: %d passed, %d failed\n", tests_passed, tests_failed);
    return tests_failed > 0 ? 1 : 0;
}
