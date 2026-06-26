// SPDX-License-Identifier: Apache-2.0
// Copyright 2024 Qualcomm Innovation Center, Inc.
//
// Generic diag subsystem command builder.
// Both QDSS and ADPL configuration use subsystem commands (cmd_id=0x4B)
// sent over the same diag serial connection.

#ifndef DIAGLOGGER_SUBSYSTEM_COMMANDS_H
#define DIAGLOGGER_SUBSYSTEM_COMMANDS_H

#include "diaglogger/types.h"

namespace diaglogger {

// Diag subsystem command ID
namespace DiagCmd {
    constexpr uint8_t SUBSYS = 0x4B;
}

// Subsystem IDs used by QDSS and ADPL
namespace SubsysId {
    constexpr uint8_t DIAG_SERV = 0x12;  // Diag service subsystem (QDSS HW accel, STM)
    constexpr uint8_t QDSS      = 0x5A;  // QDSS subsystem (trace sink, filters)
    constexpr uint8_t IPA       = 0x5A;  // IPA subsystem (ADPL) -- same ID, different cmds
}

/// Build a generic diag subsystem command packet.
///
/// Packet format:
///   [1 byte]  cmd_id = 0x4B
///   [1 byte]  subsystem_id
///   [2 bytes] subsystem_cmd (little-endian)
///   [N bytes] payload
ByteBuffer build_subsystem_command(uint8_t subsys_id, uint16_t subcmd,
                                   const ByteBuffer& payload = {});

/// Build a subsystem command with raw payload bytes appended.
ByteBuffer build_subsystem_command(uint8_t subsys_id, uint16_t subcmd,
                                   const uint8_t* payload, size_t payload_len);

/// Check if a response is a valid subsystem command response.
/// Validates: response[0]==0x4B, response[1]==subsys_id, response[2..3]==subcmd.
bool is_valid_subsystem_response(const ByteBuffer& response,
                                 uint8_t subsys_id, uint16_t subcmd);

/// Extract the payload portion of a subsystem response (skip 4-byte header).
/// Returns empty buffer if response is too short.
ByteBuffer extract_subsystem_payload(const ByteBuffer& response);

} // namespace diaglogger

#endif // DIAGLOGGER_SUBSYSTEM_COMMANDS_H
