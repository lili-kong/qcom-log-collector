// Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
// SPDX-License-Identifier: BSD 3-Clause Clear License
//
// Generic diag subsystem command builder.
// Both QDSS and ADPL configuration use subsystem commands (cmd_id=0x4B)
// sent over the same diag serial connection.

#ifndef QC_SUBSYSTEM_COMMANDS_H
#define QC_SUBSYSTEM_COMMANDS_H

#include "types.h"

namespace QC {

// Diag subsystem command ID
namespace DiagCmd {
    constexpr uint8_t SUBSYS    = 0x4B;
    constexpr uint8_t SUBSYS_V2 = 0x80;  // Subsystem dispatch V2 (delayed-response capable)
}

// Subsystem IDs used by QDSS and ADPL
namespace SubsysId {
    constexpr uint8_t DIAG_SERV = 0x12;  // Diag service subsystem (QDSS HW accel, STM)
    constexpr uint8_t QDSS      = 0x5A;  // QDSS subsystem (trace sink, filters)
    constexpr uint8_t IPA       = 0x64;  // IPA subsystem (ADPL) -- same ID, different cmds
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

/// Build a Subsystem Dispatch V2 (0x80) command packet.
///
/// Request format (Qualcomm SubsystemCommandRecord, same layout as V1 but cmd=0x80):
///   [1 byte]  cmd_id = 0x80
///   [1 byte]  subsystem_id
///   [2 bytes] subsystem_cmd (little-endian)
///   [N bytes] payload
///
/// V2 responses carry an 11-byte SubsystemV2Record after the 0x80 byte
/// (id:1, command:2, status:4, responseId:2, responseIndex:15+more:1).
ByteBuffer build_subsystem_v2_command(uint8_t subsys_id, uint16_t subcmd,
                                      const ByteBuffer& payload = {});

/// Check if a response is a valid subsystem command response.
/// Validates: response[0]==0x4B, response[1]==subsys_id, response[2..3]==subcmd.
bool is_valid_subsystem_response(const ByteBuffer& response,
                                 uint8_t subsys_id, uint16_t subcmd);

/// Extract the payload portion of a subsystem response (skip 4-byte header).
/// Returns empty buffer if response is too short.
ByteBuffer extract_subsystem_payload(const ByteBuffer& response);

} // namespace QC

#endif // QC_SUBSYSTEM_COMMANDS_H
