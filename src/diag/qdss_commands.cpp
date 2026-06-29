// Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
// SPDX-License-Identifier: BSD 3-Clause Clear License

#include "qdss_commands.h"

namespace diaglogger {

// --- Command builders ---

ByteBuffer build_qdss_hw_accel_query() {
    // subsys=0x12 (DIAG_SERV), cmd=548 (HW_ACCELERATION)
    // Payload: [1] operation=QUERY(2), [1] hw_type=ALL(0)
    ByteBuffer payload = { QdssHwAccelOp::QUERY, QdssHwAccelType::ALL };
    return build_subsystem_command(SubsysId::DIAG_SERV, QdssCmd::HW_ACCELERATION, payload);
}

ByteBuffer build_qdss_hw_accel_enable(uint8_t hw_type, uint32_t diagid_mask) {
    // Payload: [1] operation=ENABLE(1), [1] hw_type, [4] diagid_mask (LE)
    ByteBuffer payload;
    payload.reserve(6);
    payload.push_back(QdssHwAccelOp::ENABLE);
    payload.push_back(hw_type);
    payload.push_back(static_cast<uint8_t>(diagid_mask & 0xFF));
    payload.push_back(static_cast<uint8_t>((diagid_mask >> 8) & 0xFF));
    payload.push_back(static_cast<uint8_t>((diagid_mask >> 16) & 0xFF));
    payload.push_back(static_cast<uint8_t>((diagid_mask >> 24) & 0xFF));
    return build_subsystem_command(SubsysId::DIAG_SERV, QdssCmd::HW_ACCELERATION, payload);
}

ByteBuffer build_qdss_hw_accel_disable(uint8_t hw_type) {
    // Payload: [1] operation=DISABLE(0), [1] hw_type
    ByteBuffer payload = { QdssHwAccelOp::DISABLE, hw_type };
    return build_subsystem_command(SubsysId::DIAG_SERV, QdssCmd::HW_ACCELERATION, payload);
}

ByteBuffer build_qdss_query_transport() {
    // subsys=0x12 (DIAG_SERV), cmd=547 (GET_TRANSPORT), no payload
    return build_subsystem_command(SubsysId::DIAG_SERV, QdssCmd::GET_TRANSPORT);
}

ByteBuffer build_qdss_stm_enable(uint8_t processor_mask) {
    // subsys=0x12 (DIAG_SERV), cmd=532 (DIAG_OVER_STM)
    // Payload: [1] command=ENABLE(1), [1] processor_mask
    ByteBuffer payload = { QdssStmCommand::ENABLE_DIAG_OVER_STM, processor_mask };
    return build_subsystem_command(SubsysId::DIAG_SERV, QdssCmd::DIAG_OVER_STM, payload);
}

ByteBuffer build_qdss_stm_disable() {
    // Payload: [1] command=DISABLE(0), [1] processor_mask=ALL
    ByteBuffer payload = { QdssStmCommand::DISABLE_DIAG_OVER_STM, QdssProcessorMask::ALL };
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
    if (cmd == 0) return {};

    // Payload: [1] enable/disable
    ByteBuffer payload = { static_cast<uint8_t>(enable ? 1 : 0) };
    return build_subsystem_command(SubsysId::QDSS, cmd, payload);
}

ByteBuffer build_qdss_filter_entity(uint8_t processor_mask, bool enable,
                                     uint8_t entity_id) {
    // subsys=0x5A, cmd depends on processor
    uint16_t cmd = qdss_entity_cmd_for_processor(processor_mask);
    if (cmd == 0) return {};

    // Payload: [1] enable/disable, [1] entity_id
    ByteBuffer payload = { static_cast<uint8_t>(enable ? 1 : 0), entity_id };
    return build_subsystem_command(SubsysId::QDSS, cmd, payload);
}

// --- Response parsers ---

QdssHwAccelInfo parse_qdss_hw_accel_response(const ByteBuffer& payload) {
    QdssHwAccelInfo info;

    // Minimum response: [1] status, [1] num_types
    if (payload.size() < 2) return info;

    uint8_t status = payload[0];
    if (status != 0) return info;  // Non-zero = error

    uint8_t num_types = payload[1];

    // Each type entry: [1] hw_type, [1] supported, [4] diagid_mask
    // Optional with response_version >= 2: [1] atid
    size_t offset = 2;
    for (uint8_t i = 0; i < num_types && offset + 6 <= payload.size(); ++i) {
        uint8_t hw_type = payload[offset];
        uint8_t supported = payload[offset + 1];
        uint32_t mask = static_cast<uint32_t>(payload[offset + 2])
                      | (static_cast<uint32_t>(payload[offset + 3]) << 8)
                      | (static_cast<uint32_t>(payload[offset + 4]) << 16)
                      | (static_cast<uint32_t>(payload[offset + 5]) << 24);
        offset += 6;

        switch (hw_type) {
            case QdssHwAccelType::STM:
                info.stm_supported = (supported != 0);
                info.stm_diagid_mask = mask;
                break;
            case QdssHwAccelType::ATB:
                info.atb_supported = (supported != 0);
                info.atb_diagid_mask = mask;
                break;
            case QdssHwAccelType::LPASS_STM:
                info.lpass_stm_supported = (supported != 0);
                info.lpass_stm_diagid_mask = mask;
                break;
            default:
                break;
        }
    }

    // Check for response version and dynamic ATIDs
    if (offset < payload.size()) {
        info.response_version = payload[offset];
        offset += 1;

        if (info.response_version >= QdssResponseVersion::DYNAMIC_ATID) {
            // Dynamic ATIDs: [1] num_atid_entries, then per entry: [1] hw_type, [1] atid
            if (offset < payload.size()) {
                uint8_t num_atids = payload[offset];
                offset += 1;
                for (uint8_t i = 0; i < num_atids && offset + 2 <= payload.size(); ++i) {
                    uint8_t hw_type = payload[offset];
                    uint8_t atid = payload[offset + 1];
                    offset += 2;

                    switch (hw_type) {
                        case QdssHwAccelType::STM:
                            info.stm_atid = atid;
                            break;
                        case QdssHwAccelType::ATB:
                            info.atb_atid = atid;
                            break;
                        case QdssHwAccelType::LPASS_STM:
                            info.lpass_stm_atid = atid;
                            break;
                        default:
                            break;
                    }
                }
            }
        }
    }

    return info;
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

} // namespace diaglogger
