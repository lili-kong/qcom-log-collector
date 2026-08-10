// Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
// SPDX-License-Identifier: BSD 3-Clause Clear License
//
// Request/response signature pairing for the diag RX dispatcher.

#ifndef QC_RESPONSE_PAIRING_H
#define QC_RESPONSE_PAIRING_H

#include "diaglogger/types.h"

namespace QC {

// Command bytes whose responses echo a 4-byte header (cmd + subsys + 2-byte subcmd).
namespace PairingCmd {
    constexpr uint8_t SUBSYS    = 0x4B;  // subsystem dispatch
    constexpr uint8_t SUBSYS_V2 = 0x80;  // subsystem V2 dispatch
}

// Error/NAK response command IDs. The device sends these when it rejects a
// command. The byte following the error code is the original command code.
namespace DiagError {
    constexpr uint8_t BAD_CMD      = 0x13;
    constexpr uint8_t BAD_PARM     = 0x14;
    constexpr uint8_t BAD_LEN      = 0x15;
    constexpr uint8_t BAD_DEV      = 0x16;
    constexpr uint8_t BAD_MODE     = 0x18;
    constexpr uint8_t BAD_SPC_MODE = 0x19;
}

// Derive the expected response signature from a request payload.
//   - Subsystem (0x4B) / Subsystem V2 (0x80): first 4 bytes
//     (cmd, subsys_id, subcmd_lo, subcmd_hi).
//   - Any other diag command: first 1 byte (the echoed command code).
//   - Empty payload: empty signature (never matches anything).
ByteBuffer compute_response_signature(const ByteBuffer& request_payload);

// Return true if `frame` begins with every byte of `signature`.
// An empty signature never matches; a frame shorter than the signature never matches.
bool response_matches_signature(const ByteBuffer& signature, const ByteBuffer& frame);

// Return true if `frame` is an error/NAK response (BAD_CMD, BAD_PARM, etc.)
// that corresponds to the pending request signature. Error responses have the
// format: [error_cmd_id][original_cmd_code][...]. The original_cmd_code at
// frame[1] is matched against signature[0].
bool error_response_matches_signature(const ByteBuffer& signature, const ByteBuffer& frame);

// Return true if `frame` is an error/NAK response (BAD_CMD, BAD_PARM, etc.).
// Use this to distinguish: empty = timeout, is_error_response = device rejected,
// otherwise = valid response.
bool is_error_response(const ByteBuffer& frame);

} // namespace QC

#endif // QC_RESPONSE_PAIRING_H
