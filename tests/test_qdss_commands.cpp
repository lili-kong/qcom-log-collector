// Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
// SPDX-License-Identifier: BSD 3-Clause Clear License
//
// Unit tests for QDSS command builders and response parsers.

#include "diag/qdss_commands.h"
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

// --- HW Acceleration tests ---

TEST(hw_accel_query) {
    auto pkt = build_qdss_hw_accel_query();
    // Header: 0x4B, 0x12, 0x24, 0x02 (cmd=548=0x0224)
    // Payload: QUERY(2), ALL(0)
    CHECK(pkt.size() == 6);
    CHECK(pkt[0] == 0x4B);
    CHECK(pkt[1] == 0x12);
    CHECK(pkt[2] == 0x24);
    CHECK(pkt[3] == 0x02);
    CHECK(pkt[4] == QdssHwAccelOp::QUERY);   // 2
    CHECK(pkt[5] == QdssHwAccelType::ALL);    // 0
    PASS("hw_accel_query");
}

TEST(hw_accel_enable) {
    auto pkt = build_qdss_hw_accel_enable(QdssHwAccelType::STM, 0x0000000F);
    // Header(4) + operation(1) + hw_type(1) + mask(4) = 10 bytes
    CHECK(pkt.size() == 10);
    CHECK(pkt[0] == 0x4B);
    CHECK(pkt[1] == 0x12);
    CHECK(pkt[2] == 0x24);  // 548 low
    CHECK(pkt[3] == 0x02);  // 548 high
    CHECK(pkt[4] == QdssHwAccelOp::ENABLE);   // 1
    CHECK(pkt[5] == QdssHwAccelType::STM);    // 1
    CHECK(pkt[6] == 0x0F);  // mask byte 0
    CHECK(pkt[7] == 0x00);
    CHECK(pkt[8] == 0x00);
    CHECK(pkt[9] == 0x00);
    PASS("hw_accel_enable");
}

TEST(hw_accel_disable) {
    auto pkt = build_qdss_hw_accel_disable(QdssHwAccelType::ATB);
    CHECK(pkt.size() == 6);
    CHECK(pkt[4] == QdssHwAccelOp::DISABLE);  // 0
    CHECK(pkt[5] == QdssHwAccelType::ATB);    // 2
    PASS("hw_accel_disable");
}

// --- STM tests ---

TEST(stm_enable) {
    auto pkt = build_qdss_stm_enable(QdssProcessorMask::ALL);
    // Header(4) + command(1) + processor_mask(1) = 6 bytes
    CHECK(pkt.size() == 6);
    CHECK(pkt[0] == 0x4B);
    CHECK(pkt[1] == 0x12);
    CHECK(pkt[2] == 0x14);  // 532 = 0x0214 low
    CHECK(pkt[3] == 0x02);  // 0x0214 high
    CHECK(pkt[4] == QdssStmCommand::ENABLE_DIAG_OVER_STM);  // 1
    CHECK(pkt[5] == QdssProcessorMask::ALL);                 // 0x3F
    PASS("stm_enable");
}

TEST(stm_disable) {
    auto pkt = build_qdss_stm_disable();
    CHECK(pkt.size() == 6);
    CHECK(pkt[4] == QdssStmCommand::DISABLE_DIAG_OVER_STM);  // 0
    CHECK(pkt[5] == QdssProcessorMask::ALL);
    PASS("stm_disable");
}

TEST(stm_enable_specific_processor) {
    auto pkt = build_qdss_stm_enable(QdssProcessorMask::MODEM | QdssProcessorMask::APSS);
    CHECK(pkt[5] == (QdssProcessorMask::MODEM | QdssProcessorMask::APSS));  // 0x09
    PASS("stm_enable_specific_processor");
}

// --- Transport query ---

TEST(query_transport) {
    auto pkt = build_qdss_query_transport();
    CHECK(pkt.size() == 4);  // Just the subsys header, no payload
    CHECK(pkt[0] == 0x4B);
    CHECK(pkt[1] == 0x12);
    CHECK(pkt[2] == 0x23);  // 547 = 0x0223 low
    CHECK(pkt[3] == 0x02);
    PASS("query_transport");
}

// --- Trace sink ---

TEST(set_trace_sink) {
    auto pkt = build_qdss_set_trace_sink(QdssSinkType::USB);
    CHECK(pkt.size() == 5);
    CHECK(pkt[0] == 0x4B);
    CHECK(pkt[1] == 0x5A);   // QDSS subsys
    CHECK(pkt[2] == 0x01);   // 257 = 0x0101 low
    CHECK(pkt[3] == 0x01);
    CHECK(pkt[4] == QdssSinkType::USB);  // 3
    PASS("set_trace_sink");
}

// --- SW trace filter ---

TEST(filter_sw_trace_apps) {
    auto pkt = build_qdss_filter_sw_trace(QdssProcessorMask::APSS, true);
    CHECK(!pkt.empty());
    CHECK(pkt[0] == 0x4B);
    CHECK(pkt[1] == 0x5A);
    // cmd should be FILTER_SW_TRACE_APPS = 262 = 0x0106
    CHECK(pkt[2] == 0x06);
    CHECK(pkt[3] == 0x01);
    CHECK(pkt[4] == 1);  // enable
    PASS("filter_sw_trace_apps");
}

TEST(filter_sw_trace_modem_disable) {
    auto pkt = build_qdss_filter_sw_trace(QdssProcessorMask::MODEM, false);
    CHECK(!pkt.empty());
    // cmd should be FILTER_SW_TRACE_MODEM = 518 = 0x0206
    CHECK(pkt[2] == 0x06);
    CHECK(pkt[3] == 0x02);
    CHECK(pkt[4] == 0);  // disable
    PASS("filter_sw_trace_modem_disable");
}

TEST(filter_sw_trace_invalid_processor) {
    auto pkt = build_qdss_filter_sw_trace(0xFF, true);
    CHECK(pkt.empty());  // Unknown processor → empty
    PASS("filter_sw_trace_invalid_processor");
}

// --- Entity filter ---

TEST(filter_entity_adsp) {
    auto pkt = build_qdss_filter_entity(QdssProcessorMask::ADSP, true, 42);
    CHECK(!pkt.empty());
    // cmd = FILTER_ENTITY_ADSP = 1288 = 0x0508
    CHECK(pkt[2] == 0x08);
    CHECK(pkt[3] == 0x05);
    CHECK(pkt[4] == 1);   // enable
    CHECK(pkt[5] == 42);  // entity_id
    PASS("filter_entity_adsp");
}

// --- Processor-to-command mapping ---

TEST(sw_trace_cmd_mapping) {
    CHECK(qdss_sw_trace_cmd_for_processor(QdssProcessorMask::APSS)  == QdssCmd::FILTER_SW_TRACE_APPS);
    CHECK(qdss_sw_trace_cmd_for_processor(QdssProcessorMask::MODEM) == QdssCmd::FILTER_SW_TRACE_MODEM);
    CHECK(qdss_sw_trace_cmd_for_processor(QdssProcessorMask::WCNSS) == QdssCmd::FILTER_SW_TRACE_WCNSS);
    CHECK(qdss_sw_trace_cmd_for_processor(QdssProcessorMask::ADSP)  == QdssCmd::FILTER_SW_TRACE_ADSP);
    CHECK(qdss_sw_trace_cmd_for_processor(QdssProcessorMask::SLPI)  == QdssCmd::FILTER_SW_TRACE_SLPI);
    CHECK(qdss_sw_trace_cmd_for_processor(QdssProcessorMask::CDSP)  == QdssCmd::FILTER_SW_TRACE_CDSP);
    CHECK(qdss_sw_trace_cmd_for_processor(0xFF) == 0);  // invalid
    PASS("sw_trace_cmd_mapping");
}

TEST(entity_cmd_mapping) {
    CHECK(qdss_entity_cmd_for_processor(QdssProcessorMask::APSS)  == QdssCmd::FILTER_ENTITY_APPS);
    CHECK(qdss_entity_cmd_for_processor(QdssProcessorMask::MODEM) == QdssCmd::FILTER_ENTITY_MODEM);
    CHECK(qdss_entity_cmd_for_processor(QdssProcessorMask::WCNSS) == QdssCmd::FILTER_ENTITY_WCNSS);
    CHECK(qdss_entity_cmd_for_processor(QdssProcessorMask::ADSP)  == QdssCmd::FILTER_ENTITY_ADSP);
    CHECK(qdss_entity_cmd_for_processor(QdssProcessorMask::SLPI)  == QdssCmd::FILTER_ENTITY_SLPI);
    CHECK(qdss_entity_cmd_for_processor(QdssProcessorMask::CDSP)  == QdssCmd::FILTER_ENTITY_CDSP);
    PASS("entity_cmd_mapping");
}

// --- HW accel response parser ---

TEST(parse_hw_accel_response_basic) {
    // Simulate a response: status=0, num_types=2
    // Type 0: STM, supported=1, mask=0x0000000F
    // Type 1: ATB, supported=0, mask=0x00000000
    ByteBuffer payload = {
        0x00,  // status = success
        0x02,  // num_types = 2
        // Entry 0: STM
        QdssHwAccelType::STM, 0x01, 0x0F, 0x00, 0x00, 0x00,
        // Entry 1: ATB
        QdssHwAccelType::ATB, 0x00, 0x00, 0x00, 0x00, 0x00,
    };
    auto info = parse_qdss_hw_accel_response(payload);
    CHECK(info.stm_supported == true);
    CHECK(info.stm_diagid_mask == 0x0000000F);
    CHECK(info.atb_supported == false);
    CHECK(info.atb_diagid_mask == 0x00000000);
    CHECK(info.lpass_stm_supported == false);
    PASS("parse_hw_accel_response_basic");
}

TEST(parse_hw_accel_response_with_dynamic_atid) {
    // Response with version 2 (dynamic ATIDs)
    ByteBuffer payload = {
        0x00,  // status
        0x01,  // num_types
        QdssHwAccelType::STM, 0x01, 0xFF, 0x00, 0x00, 0x00,  // STM entry
        QdssResponseVersion::DYNAMIC_ATID,  // response version = 2
        0x01,  // num_atid_entries
        QdssHwAccelType::STM, 0x20,  // STM atid = 32
    };
    auto info = parse_qdss_hw_accel_response(payload);
    CHECK(info.stm_supported == true);
    CHECK(info.stm_diagid_mask == 0x000000FF);
    CHECK(info.response_version == QdssResponseVersion::DYNAMIC_ATID);
    CHECK(info.stm_atid == 0x20);  // Dynamic ATID = 32
    PASS("parse_hw_accel_response_with_dynamic_atid");
}

TEST(parse_hw_accel_response_error_status) {
    ByteBuffer payload = { 0x01, 0x00 };  // status=1 (error)
    auto info = parse_qdss_hw_accel_response(payload);
    CHECK(info.stm_supported == false);
    CHECK(info.atb_supported == false);
    PASS("parse_hw_accel_response_error_status");
}

TEST(parse_hw_accel_response_too_short) {
    ByteBuffer payload = { 0x00 };  // Too short
    auto info = parse_qdss_hw_accel_response(payload);
    CHECK(info.stm_supported == false);
    PASS("parse_hw_accel_response_too_short");
}

TEST(parse_hw_accel_response_empty) {
    ByteBuffer payload;
    auto info = parse_qdss_hw_accel_response(payload);
    CHECK(info.stm_supported == false);
    PASS("parse_hw_accel_response_empty");
}

int main() {
    std::printf("=== QDSS Command Tests ===\n");
    std::printf("\nResults: %d passed, %d failed\n", tests_passed, tests_failed);
    return tests_failed > 0 ? 1 : 0;
}
