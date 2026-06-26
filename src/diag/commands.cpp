// SPDX-License-Identifier: Apache-2.0
// Copyright 2024 Qualcomm Innovation Center, Inc.

#include "commands.h"
#include "subsystem_commands.h"
#include <cstring>

namespace diaglogger {

// Helper to append a little-endian uint32 to a buffer
static void append_u32(ByteBuffer& buf, uint32_t val) {
    buf.push_back(static_cast<uint8_t>(val & 0xFF));
    buf.push_back(static_cast<uint8_t>((val >> 8) & 0xFF));
    buf.push_back(static_cast<uint8_t>((val >> 16) & 0xFF));
    buf.push_back(static_cast<uint8_t>((val >> 24) & 0xFF));
}

ByteBuffer build_verno_request() {
    return { DiagCmd::VERNO };
}

ByteBuffer build_esn_request() {
    return { DiagCmd::ESN };
}

ByteBuffer build_loopback_request() {
    // Loopback command: cmd_id + test payload
    ByteBuffer pkt;
    pkt.push_back(DiagCmd::LOOPBACK);
    // Add test payload bytes for verification
    pkt.push_back(0x01);
    pkt.push_back(0x02);
    pkt.push_back(0x03);
    pkt.push_back(0x04);
    return pkt;
}

ByteBuffer build_status_request() {
    // STATUS command: single byte, matches QUTS doSimpleConnect()
    return { DiagCmd::STATUS };
}

ByteBuffer build_ext_build_id_request() {
    // EXT_BUILD_ID command: single byte, matches QUTS MOBILE_ID_REQUEST[]
    return { DiagCmd::EXT_BUILD_ID };
}

ByteBuffer build_log_config_disable(uint8_t equip_id) {
    // LOG_CONFIG disable: operation = DISABLE_ALL (0x00)
    ByteBuffer pkt;
    pkt.push_back(DiagCmd::LOG_CONFIG);
    pkt.push_back(0x00);
    pkt.push_back(0x00);
    pkt.push_back(0x00);
    append_u32(pkt, LogConfigOp::DISABLE_ALL);
    append_u32(pkt, static_cast<uint32_t>(equip_id));
    return pkt;
}

std::vector<ByteBuffer> build_log_config_disable_all() {
    std::vector<ByteBuffer> commands;
    for (int i = 0; i < 16; ++i) {
        commands.push_back(build_log_config_disable(static_cast<uint8_t>(i)));
    }
    return commands;
}

ByteBuffer build_event_report(bool enable) {
    // EVENT_RPT packet: cmd_id + enable flag
    ByteBuffer pkt;
    pkt.push_back(DiagCmd::EVENT_RPT);
    pkt.push_back(enable ? 0x01 : 0x00);
    return pkt;
}

ByteBuffer build_set_event_mask_disable(uint16_t num_bits) {
    // SET_EVENT_MASK (0x82) with all-zero mask.
    // Matches TUF SetEventConfigRequest::createFromMask() with empty mask.
    // Format: [cmd=0x82][reserved(3)][numItems=uint16 LE][mask bytes (all zero)]
    uint16_t mask_bytes = static_cast<uint16_t>(num_bits / 8 + 1);
    ByteBuffer pkt;
    pkt.reserve(1 + 3 + 2 + mask_bytes);
    pkt.push_back(DiagCmd::SET_EVENT_MASK);
    pkt.push_back(0x00);  // reserved[0]
    pkt.push_back(0x00);  // reserved[1]
    pkt.push_back(0x00);  // reserved[2]
    pkt.push_back(static_cast<uint8_t>(num_bits & 0xFF));
    pkt.push_back(static_cast<uint8_t>((num_bits >> 8) & 0xFF));
    pkt.resize(1 + 3 + 2 + mask_bytes, 0x00);  // zero-filled mask
    return pkt;
}

ByteBuffer build_ext_msg_cfg_disable() {
    // EXT_MSG_CFG (0x7D) disable all debug messages.
    // Matches TUF DebugMessageConfigRequest::createDisableAllMasksCommand().
    // Format: [cmd=0x7D][subcmd=5][reserved=0x00 0x00][mask=0x00 0x00 0x00 0x00]
    ByteBuffer pkt;
    pkt.push_back(DiagCmd::EXT_MSG_CFG);
    pkt.push_back(MsgCfgSubCmd::SET_ALL_SUBSYS_RUNTIME_MASKS);
    pkt.push_back(0x00);  // reserved
    pkt.push_back(0x00);  // reserved
    append_u32(pkt, 0);   // mask = 0 (disable all)
    return pkt;
}

ByteBuffer build_qtrace_disable() {
    // QTrace disable: subsys=0x44 (SUBSYSTEM_LTE), cmd=0x9004, numItems=0.
    // Matches TUF QTraceConfigMaskSetFilterRequest::clearAllMask().
    ByteBuffer payload;
    append_u32(payload, 0);  // numItems = 0 (disable all)
    return build_subsystem_command(QTraceCmd::SUBSYS_LTE,
                                   QTraceCmd::QTRACE_MASKSET_FILTER, payload);
}

ByteBuffer build_qshmetrics_disable() {
    // QshMetrics disable: subsys=0x44 (SUBSYSTEM_LTE), cmd=0x9002.
    // Matches TUF QshMetricsConfigRequest::createCompleteSetFromMask() with empty mask.
    // Payload: [numArgs=4][action=FLUSH_CLEAR(2)][enable=DISABLE(0)][clientID=0][numItems=0]
    ByteBuffer payload;
    append_u32(payload, 4);  // numArgs = MIN_NUM_ARGUMENTS
    append_u32(payload, 2);  // action = ACTION_FLUSH_CLEAR
    append_u32(payload, 0);  // enable = OPT_DISABLE
    append_u32(payload, 0);  // clientID = 0
    append_u32(payload, 0);  // numItems = 0
    return build_subsystem_command(QTraceCmd::SUBSYS_LTE,
                                   QTraceCmd::QSHMETRICS_CONFIG, payload);
}

bool parse_poll_response(const ByteBuffer& response) {
    // A valid poll response starts with the STATUS command byte
    return !response.empty() && response[0] == DiagCmd::STATUS;
}

bool is_valid_response(const ByteBuffer& response, uint8_t expected_cmd) {
    if (response.empty()) return false;
    return response[0] == expected_cmd;
}

} // namespace diaglogger
