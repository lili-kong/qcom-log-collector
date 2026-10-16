// Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
// SPDX-License-Identifier: BSD 3-Clause Clear License
//
// Diag command IDs and packet builders for Qualcomm diagnostic protocol.

#ifndef QC_COMMANDS_H
#define QC_COMMANDS_H

#include <cstdint>
#include <vector>

#include "types.h"
#include <string>

namespace QC {

// Diag command IDs
namespace DiagCmd {
    constexpr uint8_t VERNO           = 0x00;
    constexpr uint8_t ESN             = 0x01;
    constexpr uint8_t STATUS          = 0x0C;
    constexpr uint8_t EVENT_RPT       = 0x60;
    constexpr uint8_t LOG_CONFIG      = 0x73;
    constexpr uint8_t LOOPBACK        = 0x7B;
    constexpr uint8_t EXT_MSG_CFG     = 0x7D;
    constexpr uint8_t SET_EVENT_MASK  = 0x82;
    constexpr uint8_t EXT_BUILD_ID    = 0x9F;
}

// LOG_CONFIG sub-operations
namespace LogConfigOp {
    constexpr uint8_t DISABLE_ALL = 0x00;
    constexpr uint8_t SET_MASK    = 0x03;
}

// EXT_MSG_CFG sub-commands (DebugMessageConfig::Subcommand)
namespace MsgCfgSubCmd {
    constexpr uint8_t SET_ALL_SUBSYS_RUNTIME_MASKS = 5;
}

// QTrace/QshMetrics subsystem (SUBSYSTEM_LTE = 68 = 0x44)
namespace QTraceCmd {
    constexpr uint8_t  SUBSYS_LTE                  = 0x44;
    constexpr uint16_t QTRACE_MASKSET_FILTER        = 0x9004;  // 36868
    constexpr uint16_t QSHMETRICS_CONFIG             = 0x9002;  // 36866
}

/// Build a VERNO request packet (cmd_id = 0x00, 1 byte).
ByteBuffer build_verno_request();

/// Build an ESN request packet (cmd_id = 0x01, 1 byte).
ByteBuffer build_esn_request();

/// Build a loopback request packet (cmd_id = 0x7B with test payload).
ByteBuffer build_loopback_request();

/// Build a STATUS request packet (cmd_id = 0x0C, 1 byte).
ByteBuffer build_status_request();

/// Build an EXT_BUILD_ID request packet (cmd_id = 0x9F, 1 byte).
ByteBuffer build_ext_build_id_request();

/// Build a LOG_CONFIG disable-all command for a given equipment ID.
ByteBuffer build_log_config_disable(uint8_t equip_id);

/// Build LOG_CONFIG disable-all for all equipment IDs (0-15).
std::vector<ByteBuffer> build_log_config_disable_all();

/// Build a LOG_CONFIG SET_MASK command for one equipment ID.
/// last_item is the log-item count (mask covers items 0..last_item-1); mask is the
/// bit array (LSB-first, bit = log-item number). Used to enable specific log codes.
ByteBuffer build_log_config_set_mask(uint8_t equip_id, uint32_t last_item,
                                     const std::vector<uint8_t>& mask);

/// Build an EVENT_RPT enable/disable command.
ByteBuffer build_event_report(bool enable);

/// Build a SET_EVENT_MASK (0x82) command with all events disabled (zero mask).
/// num_bits: number of event ID bits to include (default 4096, typical max).
ByteBuffer build_set_event_mask_disable(uint16_t num_bits = 4096);

/// Build an EXT_MSG_CFG (0x7D) disable-all debug messages command.
/// Subcommand SET_ALL_SUBSYS_RUNTIME_MASKS with mask=0.
ByteBuffer build_ext_msg_cfg_disable();

/// Build a QTrace mask-set filter request with numItems=0 (disable all).
/// Subsys command via DIAG_SERV, subcmd=0x9004.
ByteBuffer build_qtrace_disable();

/// Build a QshMetrics disable command (action=FLUSH_CLEAR, enable=0, numItems=0).
/// Subsys command via DIAG_SERV, subcmd=0x9002.
ByteBuffer build_qshmetrics_disable();

/// Parse a poll response. Returns true if the response is a valid STATUS reply.
bool parse_poll_response(const ByteBuffer& response);

/// Check if a response packet is a valid ACK for a given command.
bool is_valid_response(const ByteBuffer& response, uint8_t expected_cmd);

} // namespace QC

#endif // QC_COMMANDS_H
