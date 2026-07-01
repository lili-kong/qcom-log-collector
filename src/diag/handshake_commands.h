// Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
// SPDX-License-Identifier: BSD 3-Clause Clear License
//
// Handshake subsystem command builders and response parsers.
// Implements the connection handshake sequence: security keys,
// feature query, DiagID/hypervisor setup.

#ifndef DIAGLOGGER_HANDSHAKE_COMMANDS_H
#define DIAGLOGGER_HANDSHAKE_COMMANDS_H

#include "diaglogger/types.h"
#include "subsystem_commands.h"

#include <array>
#include <string>
#include <vector>

namespace diaglogger {

// --- Handshake subsystem command codes (subsys=0x12, DIAG_SERV) ---

namespace HandshakeCmd {
    constexpr uint16_t DIAGID_TABLE_QUERY        = 0x0222;  // 546
    constexpr uint16_t DIAGID_FEATURE_ENABLE     = 0x0232;  // 562
    constexpr uint16_t DIAGID_REGISTRATION_TABLE = 0x0233;  // 563
    constexpr uint16_t DIAGID_PRIORITY_CONFIG    = 0x0234;  // 564
    constexpr uint16_t SECURITY_KEY_LIST         = 0x0833;  // 2099

    // TABL-dependent command codes. FEATURE_QUERY comes from TABL library headers
    // (::Diag::DiagFeatureQueryRequest) which are not in this repo. Using a
    // placeholder that can be updated when TABL headers are available.
    constexpr uint16_t FEATURE_QUERY             = 0x0225;  // placeholder

    // Non-HDLC switch
    constexpr uint16_t NON_HDLC_SWITCH           = 0x0218;  // 536

    // QShrink4 FILE_LIST per-peripheral subcmds (subsys=0x12)
    constexpr uint16_t QSR4_FILE_LIST_APPS  = 527;   // 0x020F
    constexpr uint16_t QSR4_FILE_LIST_MPSS  = 2070;  // 0x0816
    constexpr uint16_t QSR4_FILE_LIST_ADSP  = 3600;  // 0x0E10
    constexpr uint16_t QSR4_FILE_LIST_WCNSS = 5151;  // 0x141F
    constexpr uint16_t QSR4_FILE_LIST_SLPI  = 6680;  // 0x1A18
    constexpr uint16_t QSR4_FILE_LIST_CDSP  = 8209;  // 0x2011
}

// DiagID priority config opcodes
namespace DiagIdPriorityOp {
    constexpr uint8_t GET   = 0;
    constexpr uint8_t SET   = 1;
    constexpr uint8_t CLEAR = 2;
}

// DiagID priority config status codes
namespace DiagIdPriorityStatus {
    constexpr uint8_t SUCCESS         = 0;
    constexpr uint8_t GENERAL_ERROR   = 1;
    constexpr uint8_t INVALID_VERSION = 2;
    constexpr uint8_t INVALID_OPCODE  = 3;
    constexpr uint8_t INVALID_DIAG_ID = 4;
}

// Hypervisor packet format bitmask values
namespace HypervisorPacketFormat {
    constexpr uint8_t CMD   = 1;
    constexpr uint8_t ASYNC = 2;
    constexpr uint8_t BOTH  = 3;
}

// --- Response structures ---

/// Result from feature query response.
struct FeatureQueryResult {
    bool valid = false;
    uint8_t version = 0;
    uint32_t feature_mask = 0;   // Bitmask of supported features
    bool supports_hypervisor() const { return (feature_mask & 0x01) != 0; }
    bool supports_diagid() const { return (feature_mask & 0x02) != 0; }
};

/// Single entry in security key response.
struct SecurityKeyEntry {
    uint8_t version = 0;
    uint8_t classifier = 0;
    uint8_t public_type = 0;
    uint8_t reserved = 0;
    uint16_t public_len = 0;
    uint16_t wrapped_len = 0;
    ByteBuffer public_key;
    ByteBuffer wrapped_key;
};

/// Result from security key response.
struct SecurityKeyResult {
    bool valid = false;
    uint8_t status = 0;          // 0 = success
    std::vector<SecurityKeyEntry> keys;
    bool ok() const { return valid && status == 0; }
};

/// Result from hypervisor feature enable response.
struct HypervisorEnableResult {
    bool valid = false;
    uint8_t version = 0;
    uint8_t packet_format_mask = 0;
    uint8_t status = 0;          // 0 = success
    bool ok() const { return valid && status == 0; }
};

/// Result from DiagID priority config response.
struct DiagIdPriorityResult {
    bool valid = false;
    uint8_t version = 0;
    uint8_t opcode = 0;
    uint8_t diag_id = 0;
    uint8_t status = 0;          // 0 = success
    bool ok() const { return valid && status == 0; }
};

/// Single entry in DiagID table.
struct DiagIdEntry {
    uint8_t diag_id = 0;
    std::string processor_name;
    std::array<uint8_t, 16> guid{};
};

/// Result from DiagID table query response.
struct DiagIdTableResult {
    bool valid = false;
    uint8_t version = 0;
    std::vector<DiagIdEntry> entries;
};

/// Result from Non-HDLC switch response.
/// Success means device will switch to non-HDLC framing after this response.
struct NonHdlcSwitchResult {
    bool valid = false;
    uint8_t framing_version = 0;  // Expected: 1
    uint8_t result_code = 0xFF;   // 0 = success
    bool ok() const { return valid && result_code == 0 && framing_version == 1; }
};

/// VERNO (0x00) response — device version information.
struct VernoResponse {
    bool valid = false;
    ByteBuffer raw;
    std::string compile_date;
    std::string compile_time;
    std::string release_date;
    std::string release_time;
    std::string version_directory;
    uint8_t station_class_mark = 0;
    uint8_t mobile_cai = 0;
    uint8_t manufacturers_model = 0;
    uint16_t firmware_revision = 0;
    uint8_t slot_cycle_index = 0;
    uint16_t msm_revision = 0;
};

/// EXT_BUILD_ID (0x9F) response — extended build identification.
struct ExtBuildIdResponse {
    bool valid = false;
    ByteBuffer raw;
    uint8_t version = 0;
    ByteBuffer reserved;
    uint32_t msm_revision = 0;
    uint32_t model_number = 0;
    std::string software_revision;
    std::string model_name;
};

/// ESN (0x01) response — Electronic Serial Number.
struct EsnResponse {
    bool valid = false;
    ByteBuffer raw;
    uint32_t esn = 0;
};

/// STATUS (0x0C) response — DMSS status information.
struct StatusResponse {
    bool valid = false;
    ByteBuffer raw;
};

/// Single file entry from QSR4 FILE_LIST response.
struct QshrinkFileEntry {
    std::array<uint8_t, 16> guid{};
    uint32_t file_length = 0;
};

/// Result from QSR4 FILE_LIST response.
struct QshrinkFileListResult {
    bool valid = false;
    uint8_t status = 0xFF;
    std::vector<QshrinkFileEntry> files;
    bool ok() const { return valid && status == 0; }
};

/// Peripheral info for QSR4 FILE_LIST iteration.
struct QshrinkPeripheral {
    uint16_t subcmd;
    const char* name;
};

// --- Command builders ---

/// Build security key request
/// Subsys=0x12, subcmd=0x0833, payload=[0x01, 0x00, 0x00, 0x00].
ByteBuffer build_security_key_request();

/// Build feature query request.
/// Subsys=0x12, subcmd=FEATURE_QUERY, payload=[version=1][0xFF 0xFF 0xFF 0xFF].
ByteBuffer build_feature_query();

/// Build DiagID feature enable (hypervisor enable).
/// Subsys=0x12, subcmd=0x0232, payload=[version=1][mask].
/// mask: HypervisorPacketFormat value (CMD, ASYNC, or BOTH).
ByteBuffer build_diagid_feature_enable(uint8_t mask = HypervisorPacketFormat::BOTH);

/// Build DiagID priority config request.
/// Subsys=0x12, subcmd=0x0234, payload=[version=1][opcode][diag_id].
/// op: DiagIdPriorityOp value.
/// diag_id: target DiagID (typically 1 for apps processor).
ByteBuffer build_diagid_priority_config(uint8_t op, uint8_t diag_id);

/// Build DiagID registration table request.
/// Subsys=0x12, subcmd=0x0233, no payload.
ByteBuffer build_diagid_registration_table();

/// Build DiagID table query request.
/// Subsys=0x12, subcmd=0x0222, payload=[version].
ByteBuffer build_diagid_table_query(uint8_t version = 1);

/// Build Non-HDLC switch request.
/// Subsys=0x12, subcmd=0x0218, payload=[version=1].
/// Sent in HDLC mode; response received in HDLC mode.
/// After successful response, both TX and RX switch to non-HDLC framing.
ByteBuffer build_non_hdlc_switch_request();

/// Build QSR4 FILE_LIST request for a peripheral.
/// peripheral_subcmd: one of HandshakeCmd::QSR4_FILE_LIST_* values.
/// version: protocol version to try (3, 2, or 1).
ByteBuffer build_qshrink_file_list(uint16_t peripheral_subcmd, uint16_t version = 3);

/// Get list of peripherals to query for QSR4 FILE_LIST.
const std::vector<QshrinkPeripheral>& get_qshrink_peripherals();

// --- Response parsers ---

/// Parse feature query response payload (after 4-byte subsystem header).
FeatureQueryResult parse_feature_query_response(const ByteBuffer& payload);

/// Parse security key response payload.
SecurityKeyResult parse_security_key_response(const ByteBuffer& payload);

/// Parse hypervisor feature enable response payload.
HypervisorEnableResult parse_hypervisor_enable_response(const ByteBuffer& payload);

/// Parse DiagID priority config response payload.
DiagIdPriorityResult parse_diagid_priority_response(const ByteBuffer& payload);

/// Parse DiagID table query response payload.
DiagIdTableResult parse_diagid_table_response(const ByteBuffer& payload);

/// Parse Non-HDLC switch response payload.
/// Response payload: [1] framingVersion, [1] result (0=success).
NonHdlcSwitchResult parse_non_hdlc_switch_response(const ByteBuffer& payload);

/// Parse QSR4 FILE_LIST response payload (after 4-byte subsystem header).
QshrinkFileListResult parse_qshrink_file_list(const ByteBuffer& payload);

/// Parse VERNO (0x00) response (full response including cmd byte).
VernoResponse parse_verno_response(const ByteBuffer& response);

/// Parse EXT_BUILD_ID (0x9F) response (full response including cmd byte).
ExtBuildIdResponse parse_ext_build_id_response(const ByteBuffer& response);

/// Parse ESN (0x01) response (full response including cmd byte).
EsnResponse parse_esn_response(const ByteBuffer& response);

/// Parse STATUS (0x0C) response (full response including cmd byte).
StatusResponse parse_status_response(const ByteBuffer& response);

} // namespace diaglogger

#endif // DIAGLOGGER_HANDSHAKE_COMMANDS_H
