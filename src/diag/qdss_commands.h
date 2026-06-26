// SPDX-License-Identifier: Apache-2.0
// Copyright 2024 Qualcomm Innovation Center, Inc.
//
// QDSS-specific diag subsystem command builders.
// All commands are sent over the existing diag serial connection.
// QDSS data flows back as diag packets on the same port (Diag-over-STM).

#ifndef DIAGLOGGER_QDSS_COMMANDS_H
#define DIAGLOGGER_QDSS_COMMANDS_H

#include "diaglogger/types.h"
#include "subsystem_commands.h"

namespace diaglogger {

// --- QDSS Diag Service subsystem commands (subsys=0x12) ---

namespace QdssCmd {
    // Diag Service subsystem (0x12) commands
    constexpr uint16_t DIAG_OVER_STM      = 532;   // 0x0214
    constexpr uint16_t GET_TRANSPORT       = 547;   // 0x0223
    constexpr uint16_t HW_ACCELERATION     = 548;   // 0x0224
    constexpr uint16_t ISLAND_BUFFERING    = 565;   // 0x0235

    // QDSS subsystem (0x5A) commands
    constexpr uint16_t STM_REQUEST         = 20;    // 0x0014
    constexpr uint16_t ENUM_TRACE_SINK     = 257;   // 0x0101
    constexpr uint16_t FILTER_STM          = 259;   // 0x0103

    // Per-processor SW trace filter commands (subsys=0x5A)
    constexpr uint16_t FILTER_SW_TRACE_APPS   = 262;   // 0x0106
    constexpr uint16_t FILTER_SW_TRACE_MODEM  = 518;   // 0x0206
    constexpr uint16_t FILTER_SW_TRACE_WCNSS  = 774;   // 0x0306
    constexpr uint16_t FILTER_SW_TRACE_ADSP   = 1286;  // 0x0506
    constexpr uint16_t FILTER_SW_TRACE_SLPI   = 2054;  // 0x0806
    constexpr uint16_t FILTER_SW_TRACE_CDSP   = 3334;  // 0x0D06

    // Per-processor entity filter commands (subsys=0x5A)
    constexpr uint16_t FILTER_ENTITY_APPS   = 264;   // 0x0108
    constexpr uint16_t FILTER_ENTITY_MODEM  = 520;   // 0x0208
    constexpr uint16_t FILTER_ENTITY_WCNSS  = 776;   // 0x0308
    constexpr uint16_t FILTER_ENTITY_ADSP   = 1288;  // 0x0508
    constexpr uint16_t FILTER_ENTITY_SLPI   = 2056;  // 0x0808
    constexpr uint16_t FILTER_ENTITY_CDSP   = 3336;  // 0x0D08

    // ADSP Filter STM (subsys=0x5A)
    constexpr uint16_t ADSP_FILTER_STM     = 1283;  // 0x0503
}

// HW Acceleration types
namespace QdssHwAccelType {
    constexpr uint8_t ALL       = 0;
    constexpr uint8_t STM       = 1;
    constexpr uint8_t ATB       = 2;
    constexpr uint8_t LPASS_STM = 3;
}

// HW Acceleration operations
namespace QdssHwAccelOp {
    constexpr uint8_t DISABLE = 0;
    constexpr uint8_t ENABLE  = 1;
    constexpr uint8_t QUERY   = 2;
}

// STM command types
namespace QdssStmCommand {
    constexpr uint8_t DISABLE_DIAG_OVER_STM   = 0;
    constexpr uint8_t ENABLE_DIAG_OVER_STM    = 1;
    constexpr uint8_t QUERY_PROCESSORS         = 2;
    constexpr uint8_t AUTO_QUERY_PROCESSORS    = 3;
}

// Processor mask bits
namespace QdssProcessorMask {
    constexpr uint8_t MODEM = 0x01;
    constexpr uint8_t ADSP  = 0x02;
    constexpr uint8_t WCNSS = 0x04;
    constexpr uint8_t APSS  = 0x08;
    constexpr uint8_t SLPI  = 0x10;
    constexpr uint8_t CDSP  = 0x20;
    constexpr uint8_t ALL   = 0x3F;
}

// Trace sink types
namespace QdssSinkType {
    constexpr uint8_t ETB          = 0;
    constexpr uint8_t DDR          = 1;
    constexpr uint8_t TPIU_SET_A   = 2;
    constexpr uint8_t USB          = 3;  // Default
    constexpr uint8_t USB_NON_BAM  = 4;
    constexpr uint8_t TPIU_SET_B   = 5;
    constexpr uint8_t SD           = 6;
    constexpr uint8_t TRACE_FILE   = 7;
    constexpr uint8_t PCIe         = 8;
}

// Default ATIDs (Address Trace IDs)
namespace QdssAtid {
    constexpr uint8_t STM       = 16;
    constexpr uint8_t ATB       = 50;
    constexpr uint8_t LPASS_STM = 25;
}

// HW acceleration response version
namespace QdssResponseVersion {
    constexpr uint8_t NO_DYNAMIC_ATID = 1;
    constexpr uint8_t DYNAMIC_ATID    = 2;
}

/// Parsed result from HW acceleration query response.
struct QdssHwAccelInfo {
    bool stm_supported = false;
    bool atb_supported = false;
    bool lpass_stm_supported = false;
    uint32_t stm_diagid_mask = 0;
    uint32_t atb_diagid_mask = 0;
    uint32_t lpass_stm_diagid_mask = 0;
    uint8_t stm_atid = QdssAtid::STM;
    uint8_t atb_atid = QdssAtid::ATB;
    uint8_t lpass_stm_atid = QdssAtid::LPASS_STM;
    uint8_t response_version = QdssResponseVersion::NO_DYNAMIC_ATID;
};

// --- Command builders ---

/// Build HW acceleration query command (subsys=0x12, cmd=548).
/// Queries which HW acceleration types (STM, ATB, LPASS_STM) the device supports.
ByteBuffer build_qdss_hw_accel_query();

/// Build HW acceleration enable command (subsys=0x12, cmd=548).
/// hw_type: QdssHwAccelType::STM, ATB, or LPASS_STM
/// diagid_mask: bitmask of DiagIDs to enable for this HW type
ByteBuffer build_qdss_hw_accel_enable(uint8_t hw_type, uint32_t diagid_mask);

/// Build HW acceleration disable command (subsys=0x12, cmd=548).
ByteBuffer build_qdss_hw_accel_disable(uint8_t hw_type);

/// Build transport query command (subsys=0x12, cmd=547).
ByteBuffer build_qdss_query_transport();

/// Build Diag-over-STM enable command (subsys=0x12, cmd=532).
/// processor_mask: bitmask of processors (QdssProcessorMask)
ByteBuffer build_qdss_stm_enable(uint8_t processor_mask = QdssProcessorMask::ALL);

/// Build Diag-over-STM disable command (subsys=0x12, cmd=532).
ByteBuffer build_qdss_stm_disable();

/// Build trace sink configuration command (subsys=0x5A, cmd=257).
/// sink: QdssSinkType value (default USB=3)
ByteBuffer build_qdss_set_trace_sink(uint8_t sink = QdssSinkType::USB);

/// Build per-processor SW trace filter command (subsys=0x5A).
/// processor_mask: single processor bit from QdssProcessorMask
/// enable: true to enable, false to disable
ByteBuffer build_qdss_filter_sw_trace(uint8_t processor_mask, bool enable);

/// Build per-processor entity filter command (subsys=0x5A).
/// processor_mask: single processor bit from QdssProcessorMask
/// enable: true to enable, false to disable
/// entity_id: entity to filter (optional)
ByteBuffer build_qdss_filter_entity(uint8_t processor_mask, bool enable,
                                     uint8_t entity_id = 0);

/// Parse HW acceleration query response payload.
/// payload: the subsystem response payload (after 4-byte header).
/// Returns parsed QdssHwAccelInfo, or default values on parse error.
QdssHwAccelInfo parse_qdss_hw_accel_response(const ByteBuffer& payload);

/// Get the SW trace filter command for a given processor.
uint16_t qdss_sw_trace_cmd_for_processor(uint8_t processor_mask);

/// Get the entity filter command for a given processor.
uint16_t qdss_entity_cmd_for_processor(uint8_t processor_mask);

} // namespace diaglogger

#endif // DIAGLOGGER_QDSS_COMMANDS_H
