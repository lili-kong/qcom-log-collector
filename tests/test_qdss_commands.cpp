// Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
// SPDX-License-Identifier: BSD 3-Clause Clear License
//
// Unit tests for QDSS command builders and response parsers.

#include <cassert>
#include <cstdio>

#include "diag/qdss_commands.h"
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

// --- HW Acceleration tests ---

TEST(hw_accel_query) {
    auto pkt = build_qdss_hw_accel_query();
    // Header(4) + [version, operation, reserved(2), hw_type, hw_accel_version, diagid_mask(4)] = 14 bytes
    CHECK(pkt.size() == 14);
    CHECK(pkt[0] == 0x4B);
    CHECK(pkt[1] == 0x12);
    CHECK(pkt[2] == 0x24);
    CHECK(pkt[3] == 0x02);
    CHECK(pkt[4] == 2);                      // cmd_version (dynamic AtID default)
    CHECK(pkt[5] == QdssHwAccelOp::QUERY);   // 2
    CHECK(pkt[6] == 0);                      // reserved [1/2]
    CHECK(pkt[7] == 0);                      // reserved [2/2]
    CHECK(pkt[8] == QdssHwAccelType::ALL);   // hw_type = 0
    CHECK(pkt[9] == 0);                      // hw_accel_version
    CHECK(pkt[10] == 0);                     // diagid_mask = 0 (unused for QUERY)
    PASS("hw_accel_query");
}

TEST(hw_accel_enable) {
    auto pkt = build_qdss_hw_accel_enable(QdssHwAccelType::STM, 0x0000000F);
    // Header(4) + [version, operation, reserved(2), hw_type, hw_accel_version, mask(4)] = 14 bytes
    CHECK(pkt.size() == 14);
    CHECK(pkt[0] == 0x4B);
    CHECK(pkt[1] == 0x12);
    CHECK(pkt[2] == 0x24);  // 548 low
    CHECK(pkt[3] == 0x02);  // 548 high
    CHECK(pkt[4] == 2);                       // cmd_version (dynamic AtID default)
    CHECK(pkt[5] == QdssHwAccelOp::ENABLE);   // 1
    CHECK(pkt[6] == 0);                       // reserved [1/2]
    CHECK(pkt[7] == 0);                       // reserved [2/2]
    CHECK(pkt[8] == QdssHwAccelType::STM);    // hw_type = 1
    CHECK(pkt[9] == 1);                       // hw_accel_version
    CHECK(pkt[10] == 0x0F);                   // mask byte 0
    CHECK(pkt[11] == 0x00);
    CHECK(pkt[12] == 0x00);
    CHECK(pkt[13] == 0x00);
    PASS("hw_accel_enable");
}

TEST(hw_accel_disable) {
    auto pkt = build_qdss_hw_accel_disable(QdssHwAccelType::ATB, 0x0000000F);
    // Header(4) + [version, operation, reserved(2), hw_type, hw_accel_version, mask(4)] = 14 bytes
    CHECK(pkt.size() == 14);
    CHECK(pkt[4] == 2);                        // cmd_version
    CHECK(pkt[5] == QdssHwAccelOp::DISABLE);   // 0
    CHECK(pkt[6] == 0);                        // reserved [1/2]
    CHECK(pkt[7] == 0);                        // reserved [2/2]
    CHECK(pkt[8] == QdssHwAccelType::ATB);     // hw_type = 2
    CHECK(pkt[9] == 1);                        // hw_accel_version
    CHECK(pkt[10] == 0x0F);                    // mask byte 0
    CHECK(pkt[11] == 0x00);
    CHECK(pkt[12] == 0x00);
    CHECK(pkt[13] == 0x00);
    PASS("hw_accel_disable");
}

// --- STM tests ---

TEST(stm_enable) {
    auto pkt = build_qdss_stm_enable(QdssProcessorMask::ALL);
    // Header(4) + [version, processor_mask, stm_command] = 7 bytes
    CHECK(pkt.size() == 7);
    CHECK(pkt[0] == 0x4B);
    CHECK(pkt[1] == 0x12);
    CHECK(pkt[2] == 0x14);  // 532 = 0x0214 low
    CHECK(pkt[3] == 0x02);  // 0x0214 high
    CHECK(pkt[4] == 2);                                      // version
    CHECK(pkt[5] == QdssProcessorMask::ALL);                 // 0x3F
    CHECK(pkt[6] == QdssStmCommand::ENABLE_DIAG_OVER_STM);  // 1
    PASS("stm_enable");
}

TEST(stm_disable) {
    auto pkt = build_qdss_stm_disable();
    // Header(4) + [version, processor_mask, stm_command] = 7 bytes
    CHECK(pkt.size() == 7);
    CHECK(pkt[4] == 2);                                       // version
    CHECK(pkt[5] == QdssProcessorMask::ALL);                  // 0x3F
    CHECK(pkt[6] == QdssStmCommand::DISABLE_DIAG_OVER_STM);  // 0
    PASS("stm_disable");
}

TEST(stm_enable_specific_processor) {
    auto pkt = build_qdss_stm_enable(QdssProcessorMask::MODEM | QdssProcessorMask::APSS);
    CHECK(pkt[5] == (QdssProcessorMask::MODEM | QdssProcessorMask::APSS));  // 0x09 (processor_mask)
    CHECK(pkt[6] == QdssStmCommand::ENABLE_DIAG_OVER_STM);                  // 1
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
    // Real dynamic-AtID QUERY response layout:
    //   [cmd_ver][op echo][reserved][hw_type echo][hw_ver echo][transport][num_types]
    //   then num_types entries: [type][ver][supported:4 LE][enabled:4 LE][atid] (11 bytes)
    // Type 0: STM, ver=1, supported=0x0000000F, atid=0x10
    // Type 1: ATB, ver=1, supported=0x00000003, atid=0x32
    ByteBuffer payload = {
        0x02,  // cmd_version = 2
        0x02,  // operation (echo QUERY)
        0x00,  // reserved
        0x00,  // hw_type (echo)
        0x00,  // hw_accel_version (echo)
        0x02,  // diag_transport (USB in QDSS_TRACE enum)
        0x02,  // num_types = 2
        // Entry 0: STM  [type][ver][supported:4][enabled:4][atid]
        QdssHwAccelType::STM, 0x01, 0x0F, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x10,
        // Entry 1: ATB
        QdssHwAccelType::ATB, 0x01, 0x03, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x32,
    };
    auto info = parse_qdss_hw_accel_response(payload);
    CHECK(info.response_version == 2);
    CHECK(info.diag_transport == 2);
    CHECK(info.stm_supported == true);
    CHECK(info.stm_diagid_mask == 0x0000000F);
    CHECK(info.stm_atid == 0x10);
    CHECK(info.atb_supported == true);
    CHECK(info.atb_diagid_mask == 0x00000003);
    CHECK(info.atb_atid == 0x32);
    CHECK(info.lpass_stm_supported == false);
    PASS("parse_hw_accel_response_basic");
}

TEST(parse_hw_accel_response_unknown_ver) {
    // Entry with hw_accel_ver != 1 should be skipped. 11-byte entries.
    ByteBuffer payload = {
        0x02,  // cmd_version = 2
        0x02,  // operation (echo)
        0x00,  // reserved
        0x00,  // hw_type (echo)
        0x00,  // hw_accel_version (echo)
        0x02,  // diag_transport
        0x02,  // num_types = 2
        // Entry 0: STM with unknown hw_accel_ver=2 — should be skipped
        QdssHwAccelType::STM, 0x02, 0x0F, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x10,
        // Entry 1: ATB with hw_accel_ver=1 — should be parsed
        QdssHwAccelType::ATB, 0x01, 0x07, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x32,
    };
    auto info = parse_qdss_hw_accel_response(payload);
    CHECK(info.stm_supported == false);   // skipped due to ver != 1
    CHECK(info.atb_supported == true);
    CHECK(info.atb_diagid_mask == 0x00000007);
    PASS("parse_hw_accel_response_unknown_ver");
}

TEST(parse_hw_accel_response_error_status) {
    // Too-short payload (< 7-byte header) yields nothing supported.
    ByteBuffer payload = { 0x02, 0x02, 0x00, 0x00 };
    auto info = parse_qdss_hw_accel_response(payload);
    CHECK(info.stm_supported == false);
    CHECK(info.atb_supported == false);
    PASS("parse_hw_accel_response_error_status");
}

TEST(parse_hw_accel_response_too_short) {
    ByteBuffer payload = { 0x01, 0x00, 0x03 };  // Only 3 bytes, need at least 4
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
