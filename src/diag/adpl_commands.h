// Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
// SPDX-License-Identifier: BSD 3-Clause Clear License
//
// ADPL-specific diag subsystem command builders and packet decoder.
// All configuration commands are sent over the existing diag serial connection.
// ADPL data flows back on the same diag connection.

#ifndef DIAGLOGGER_ADPL_COMMANDS_H
#define DIAGLOGGER_ADPL_COMMANDS_H

#include "diaglogger/types.h"
#include "subsystem_commands.h"

namespace diaglogger {

// ADPL IPA subsystem command
namespace AdplCmd {
    constexpr uint16_t IPA_REQUEST              = 4096;   // 0x1000
    constexpr uint16_t ACCELERATED_IPA_REQUEST  = 16384;  // 0x4000
}

// ADPL protocol versions
namespace AdplVersion {
    constexpr uint8_t VERSION1 = 1;
    constexpr uint8_t VERSION2 = 2;
    constexpr uint8_t VERSION3 = 3;
    constexpr uint8_t VERSION4 = 4;
    constexpr uint8_t VERSION5 = 5;
    constexpr uint8_t VERSION6 = 6;
    constexpr uint8_t VERSION7 = 7;
    constexpr uint8_t HIGHEST_SUPPORTED = VERSION7;
}

// ADPL DPL command field values
namespace AdplDplCommand {
    constexpr uint8_t DISABLE = 0;
    constexpr uint8_t ENABLE  = 1;
}

// ADPL statusOpcode values for frame validation
namespace AdplStatusOpcode {
    constexpr uint8_t VALID_V3_V5  = 0x10;  // Valid for V3, V4, V5
    constexpr uint8_t VALID_V6_V7a = 0x0C;  // Valid for V6, V7
    constexpr uint8_t VALID_V6_V7b = 0x0D;  // Valid for V6, V7
}

/// Parsed ADPL enable response.
struct AdplEnableResponse {
    uint8_t version = 0;    // Negotiated DPL version (1-7)
    uint8_t status = 0;     // 1 = success, other = error
    bool ok() const { return status == 1 && version >= 1 && version <= 7; }
};

/// Decoded ADPL packet header + payload.
struct AdplPacket {
    uint8_t  version = 0;
    uint8_t  mux_id = 0;
    uint16_t sequence_number = 0;
    uint64_t timestamp_us = 0;   // Timestamp in microseconds (0 if not available)
    ByteBuffer diag_payload;     // Extracted raw diag payload
    bool valid = false;
};

// --- Command builders ---

/// Build ADPL enable command (subsys=IPA, cmd=0x4000).
/// version: requested DPL version (default HIGHEST_SUPPORTED=7)
ByteBuffer build_adpl_enable(uint8_t version = AdplVersion::HIGHEST_SUPPORTED);

/// Build ADPL disable command (subsys=IPA, cmd=0x4000).
ByteBuffer build_adpl_disable();

/// Parse ADPL enable response payload.
/// payload: the subsystem response payload (after 4-byte header).
AdplEnableResponse parse_adpl_enable_response(const ByteBuffer& payload);

// --- ADPL packet decoder ---

/// Detect ADPL version from first bytes of an ADPL data packet.
/// Returns 0 if unable to detect.
uint8_t detect_adpl_version(const uint8_t* data, size_t len);

/// Get the header size for a given ADPL version.
size_t adpl_header_size(uint8_t version);

/// Calculate payload padding for a given ADPL version and payload length.
size_t adpl_padding(uint8_t version, size_t payload_len);

/// Decode a single ADPL packet from raw data.
/// version: the ADPL version to use for decoding (from enable response).
/// data: pointer to start of ADPL frame.
/// len: available bytes.
/// Returns decoded AdplPacket. Check .valid field for success.
/// On success, total consumed bytes = adpl_header_size(version) + payload_len + padding.
AdplPacket decode_adpl_packet(uint8_t version, const uint8_t* data, size_t len);

/// Get total frame size of an ADPL packet (header + payload + padding).
/// Returns 0 if data is too short to determine frame size.
size_t adpl_frame_size(uint8_t version, const uint8_t* data, size_t len);

} // namespace diaglogger

#endif // DIAGLOGGER_ADPL_COMMANDS_H
