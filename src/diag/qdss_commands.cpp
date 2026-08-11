// Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
// SPDX-License-Identifier: BSD 3-Clause Clear License

#include "qdss_commands.h"

namespace QC {

// --- Command builders ---

// HW_ACCELERATION (cmd 548) REQUEST payload is 10 bytes (verified on device):
//   [1] cmd_version, [1] operation, [2] reserved, [1] hw_type, [1] hw_accel_version,
//   [4] diagid_mask (LE).
// e.g. ATB enable v2: 02 01 00 00 02 01 80 00 00 00. A wrong reserved width makes the
// device read hw_type from the wrong offset -> BAD_PARM.
static ByteBuffer build_qdss_hw_accel_cmd(uint8_t cmd_version, uint8_t operation,
                                          uint8_t hw_type, uint8_t hw_accel_version,
                                          uint32_t diagid_mask) {
    ByteBuffer payload;
    payload.reserve(10);
    payload.push_back(cmd_version);
    payload.push_back(operation);
    payload.push_back(0);            // reserved [1/2]
    payload.push_back(0);            // reserved [2/2]
    payload.push_back(hw_type);
    payload.push_back(hw_accel_version);
    payload.push_back(static_cast<uint8_t>(diagid_mask & 0xFF));
    payload.push_back(static_cast<uint8_t>((diagid_mask >> 8) & 0xFF));
    payload.push_back(static_cast<uint8_t>((diagid_mask >> 16) & 0xFF));
    payload.push_back(static_cast<uint8_t>((diagid_mask >> 24) & 0xFF));
    return build_subsystem_command(SubsysId::DIAG_SERV, QdssCmd::HW_ACCELERATION, payload);
}

ByteBuffer build_qdss_hw_accel_query(uint8_t cmd_version) {
    // QUERY: hw_type=ALL, hw_accel_version=0, diagid_mask unused (0).
    return build_qdss_hw_accel_cmd(cmd_version, QdssHwAccelOp::QUERY,
                                   QdssHwAccelType::ALL, 0, 0);
}

ByteBuffer build_qdss_hw_accel_enable(uint8_t hw_type, uint32_t diagid_mask, uint8_t cmd_version) {
    return build_qdss_hw_accel_cmd(cmd_version, QdssHwAccelOp::ENABLE, hw_type, 1, diagid_mask);
}

ByteBuffer build_qdss_hw_accel_disable(uint8_t hw_type, uint32_t diagid_mask) {
    return build_qdss_hw_accel_cmd(2, QdssHwAccelOp::DISABLE, hw_type, 1, diagid_mask);
}

ByteBuffer build_qdss_query_transport() {
    // subsys=0x12 (DIAG_SERV), cmd=547 (GET_TRANSPORT), no payload
    return build_subsystem_command(SubsysId::DIAG_SERV, QdssCmd::GET_TRANSPORT);
}

ByteBuffer build_qdss_stm_enable(uint8_t processor_mask) {
    // subsys=0x12 (DIAG_SERV), cmd=532 (DIAG_OVER_STM)
    // [1] version, [1] processor_mask, [1] stm_command
    ByteBuffer payload = { 2, processor_mask, QdssStmCommand::ENABLE_DIAG_OVER_STM };
    return build_subsystem_command(SubsysId::DIAG_SERV, QdssCmd::DIAG_OVER_STM, payload);
}

ByteBuffer build_qdss_stm_disable() {
    // [1] version, [1] processor_mask, [1] stm_command
    ByteBuffer payload = { 2, QdssProcessorMask::ALL, QdssStmCommand::DISABLE_DIAG_OVER_STM };
    return build_subsystem_command(SubsysId::DIAG_SERV, QdssCmd::DIAG_OVER_STM, payload);
}

ByteBuffer build_qdss_set_trace_sink(uint8_t sink) {
    // subsys=0x5A (QDSS), cmd=257 (ENUM_TRACE_SINK)
    // Payload: [1] sink_type
    ByteBuffer payload = { sink };
    return build_subsystem_command(SubsysId::QDSS, QdssCmd::ENUM_TRACE_SINK, payload);
}

ByteBuffer build_qdss_filter_sw_trace(uint8_t processor_mask, bool enable) {
    // subsys=0x5A, cmd depends on processor
    uint16_t cmd = qdss_sw_trace_cmd_for_processor(processor_mask);
    if (cmd == 0) { return {}; }

    // Payload: [1] enable/disable
    ByteBuffer payload = { static_cast<uint8_t>(enable ? 1 : 0) };
    return build_subsystem_command(SubsysId::QDSS, cmd, payload);
}

ByteBuffer build_qdss_filter_entity(uint8_t processor_mask, bool enable,
                                     uint8_t entity_id) {
    // subsys=0x5A, cmd depends on processor
    uint16_t cmd = qdss_entity_cmd_for_processor(processor_mask);
    if (cmd == 0) { return {}; }

    // Payload: [1] enable/disable, [1] entity_id
    ByteBuffer payload = { static_cast<uint8_t>(enable ? 1 : 0), entity_id };
    return build_subsystem_command(SubsysId::QDSS, cmd, payload);
}

// --- Response parsers ---

QdssHwAccelInfo parse_qdss_hw_accel_response(const ByteBuffer& payload) {
    QdssHwAccelInfo info;

    // HW_ACCELERATION QUERY response op payload (verified against a dynamic-AtID device).
    // The response echoes the request header, then adds transport + entries:
    //   [0] cmd_version, [1] operation (echo), [2] reserved (echo),
    //   [3] hw_type (echo), [4] hw_accel_version (echo),
    //   [5] diag_transport, [6] num_types,
    //   then num_types entries, each 11 bytes:
    //     [type:1][ver:1][diagid_mask_supported:4 LE][diagid_mask_enabled:4 LE][atid:1]
    if (payload.size() < 7) return info;

    info.response_version = payload[0];
    info.diag_transport = payload[5];
    uint8_t num_types = payload[6];

    constexpr size_t entry_size = 11;
    size_t offset = 7;
    for (uint8_t i = 0; i < num_types && offset + entry_size <= payload.size(); ++i) {
        uint8_t hw_type = payload[offset];
        uint8_t hw_accel_ver = payload[offset + 1];
        uint32_t mask = static_cast<uint32_t>(payload[offset + 2])
                      | (static_cast<uint32_t>(payload[offset + 3]) << 8)
                      | (static_cast<uint32_t>(payload[offset + 4]) << 16)
                      | (static_cast<uint32_t>(payload[offset + 5]) << 24);
        uint8_t atid = payload[offset + 10];
        offset += entry_size;

        // Only process entries with hw_accel_ver == 1
        if (1 != hw_accel_ver)
            continue;

        switch (hw_type) {
            case QdssHwAccelType::STM:
                info.stm_supported = true;
                info.stm_diagid_mask = mask;
                info.stm_atid = atid;
                break;
            case QdssHwAccelType::ATB:
                info.atb_supported = true;
                info.atb_diagid_mask = mask;
                info.atb_atid = atid;
                break;
            case QdssHwAccelType::LPASS_STM:
                info.lpass_stm_supported = true;
                info.lpass_stm_diagid_mask = mask;
                info.lpass_stm_atid = atid;
                break;
            default:
                break;
        }
    }

    return info;
}

QdssHwAccelEnableResult parse_qdss_hw_accel_enable_response(const ByteBuffer& payload) {
    QdssHwAccelEnableResult result;

    // ENABLE/DISABLE response (verified on device), 11 bytes:
    //   [0] cmd_version, [1] operation, [2] status, [3-4] reserved,
    //   [5] hw_type, [6] hw_accel_version, [7-10] diagid_mask (LE).
    // e.g. ATB enable OK: 02 01 00 00 00 02 01 80 00 00 00.
    if (payload.size() < 11) return result;

    result.cmd_version = payload[0];
    result.status = payload[2];
    result.diagid_mask = static_cast<uint32_t>(payload[7])
                       | (static_cast<uint32_t>(payload[8]) << 8)
                       | (static_cast<uint32_t>(payload[9]) << 16)
                       | (static_cast<uint32_t>(payload[10]) << 24);
    result.valid = true;
    return result;
}

// --- Processor-to-command mapping ---

uint16_t qdss_sw_trace_cmd_for_processor(uint8_t processor_mask) {
    switch (processor_mask) {
        case QdssProcessorMask::APSS:  return QdssCmd::FILTER_SW_TRACE_APPS;
        case QdssProcessorMask::MODEM: return QdssCmd::FILTER_SW_TRACE_MODEM;
        case QdssProcessorMask::WCNSS: return QdssCmd::FILTER_SW_TRACE_WCNSS;
        case QdssProcessorMask::ADSP:  return QdssCmd::FILTER_SW_TRACE_ADSP;
        case QdssProcessorMask::SLPI:  return QdssCmd::FILTER_SW_TRACE_SLPI;
        case QdssProcessorMask::CDSP:  return QdssCmd::FILTER_SW_TRACE_CDSP;
        default: return 0;
    }
}

uint16_t qdss_entity_cmd_for_processor(uint8_t processor_mask) {
    switch (processor_mask) {
        case QdssProcessorMask::APSS:  return QdssCmd::FILTER_ENTITY_APPS;
        case QdssProcessorMask::MODEM: return QdssCmd::FILTER_ENTITY_MODEM;
        case QdssProcessorMask::WCNSS: return QdssCmd::FILTER_ENTITY_WCNSS;
        case QdssProcessorMask::ADSP:  return QdssCmd::FILTER_ENTITY_ADSP;
        case QdssProcessorMask::SLPI:  return QdssCmd::FILTER_ENTITY_SLPI;
        case QdssProcessorMask::CDSP:  return QdssCmd::FILTER_ENTITY_CDSP;
        default: return 0;
    }
}

} // namespace QC
