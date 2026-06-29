// Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
// SPDX-License-Identifier: BSD 3-Clause Clear License

#include "handshake_commands.h"
#include "commands.h"
#include <cstring>
#include <KL/Macros.h>

namespace diaglogger {

// --- Command builders ---

ByteBuffer build_security_key_request() {
    // Exact bytes from QUTS Diag.cpp:376 — requestIPKeys()
    // [0x4B][0x12][0x33][0x08][0x01][0x00][0x00][0x00]
    ByteBuffer payload = { 0x01, 0x00, 0x00, 0x00 };
    return build_subsystem_command(SubsysId::DIAG_SERV, HandshakeCmd::SECURITY_KEY_LIST, payload);
}

ByteBuffer build_feature_query() {
    // Subsys=0x12, subcmd=FEATURE_QUERY
    // Payload: [1] version=1, [4] feature_mask=0xFFFFFFFF (query all)
    ByteBuffer payload = { 0x01, 0xFF, 0xFF, 0xFF, 0xFF };
    return build_subsystem_command(SubsysId::DIAG_SERV, HandshakeCmd::FEATURE_QUERY, payload);
}

ByteBuffer build_diagid_feature_enable(uint8_t mask) {
    // Subsys=0x12, subcmd=0x0232 (DIAGID_FEATURE_ENABLE)
    // Payload: [1] version=1, [1] packetFormatBitMask
    ByteBuffer payload = { 0x01, mask };
    return build_subsystem_command(SubsysId::DIAG_SERV, HandshakeCmd::DIAGID_FEATURE_ENABLE, payload);
}

ByteBuffer build_diagid_priority_config(uint8_t op, uint8_t diag_id) {
    // Subsys=0x12, subcmd=0x0234 (DIAGID_PRIORITY_CONFIG)
    // Payload: [1] version=1, [1] opCode, [1] diagId
    ByteBuffer payload = { 0x01, op, diag_id };
    return build_subsystem_command(SubsysId::DIAG_SERV, HandshakeCmd::DIAGID_PRIORITY_CONFIG, payload);
}

ByteBuffer build_diagid_registration_table() {
    // Subsys=0x12, subcmd=0x0233 (DIAGID_REGISTRATION_TABLE), no payload
    return build_subsystem_command(SubsysId::DIAG_SERV, HandshakeCmd::DIAGID_REGISTRATION_TABLE);
}

ByteBuffer build_diagid_table_query(uint8_t version) {
    // Subsys=0x12, subcmd=0x0222 (DIAGID_TABLE_QUERY)
    // Payload: [1] version
    ByteBuffer payload = { version };
    return build_subsystem_command(SubsysId::DIAG_SERV, HandshakeCmd::DIAGID_TABLE_QUERY, payload);
}

ByteBuffer build_non_hdlc_switch_request() {
    // Subsys=0x12, subcmd=0x0218 (NON_HDLC_SWITCH)
    // Payload: [1] version=1
    // Response is still received in HDLC mode.
    // After response, both TX and RX switch to non-HDLC framing.
    ByteBuffer payload = { 0x01 };
    return build_subsystem_command(SubsysId::DIAG_SERV, HandshakeCmd::NON_HDLC_SWITCH, payload);
}

ByteBuffer build_qshrink_file_list(uint16_t peripheral_subcmd, uint16_t version) {
    // Subsys=0x12, subcmd=peripheral_subcmd
    // Payload: [2] version (uint16 LE) + [2] operation=0 (uint16 LE)
    ByteBuffer payload = {
        static_cast<uint8_t>(version & 0xFF),
        static_cast<uint8_t>((version >> 8) & 0xFF),
        0x00, 0x00  // operation = FILE_LIST (0)
    };
    return build_subsystem_command(SubsysId::DIAG_SERV, peripheral_subcmd, payload);
}

const std::vector<QshrinkPeripheral>& get_qshrink_peripherals() {
    static const std::vector<QshrinkPeripheral> peripherals = {
        { HandshakeCmd::QSR4_FILE_LIST_APPS,  "apps"  },
        { HandshakeCmd::QSR4_FILE_LIST_MPSS,  "modem" },
        { HandshakeCmd::QSR4_FILE_LIST_ADSP,  "adsp"  },
        { HandshakeCmd::QSR4_FILE_LIST_WCNSS, "wcnss" },
        { HandshakeCmd::QSR4_FILE_LIST_SLPI,  "slpi"  },
        { HandshakeCmd::QSR4_FILE_LIST_CDSP,  "cdsp"  },
    };
    return peripherals;
}

// --- Response parsers ---

FeatureQueryResult parse_feature_query_response(const ByteBuffer& payload) {
    FeatureQueryResult result;

    // Minimum: [1] version, [4] feature_mask
    if (payload.size() < 5) return result;

    result.version = payload[0];
    result.feature_mask = static_cast<uint32_t>(payload[1])
                        | (static_cast<uint32_t>(payload[2]) << 8)
                        | (static_cast<uint32_t>(payload[3]) << 16)
                        | (static_cast<uint32_t>(payload[4]) << 24);
    result.valid = true;
    return result;
}

SecurityKeyResult parse_security_key_response(const ByteBuffer& payload) {
    SecurityKeyResult result;

    // Minimum: [1] status
    if (payload.empty()) return result;

    result.status = payload[0];
    result.valid = true;
    return result;
}

HypervisorEnableResult parse_hypervisor_enable_response(const ByteBuffer& payload) {
    HypervisorEnableResult result;

    // Expected: [1] version, [1] packetFormatBitMask, [1] status
    if (payload.size() < 3) return result;

    result.version = payload[0];
    result.packet_format_mask = payload[1];
    result.status = payload[2];
    result.valid = true;
    return result;
}

DiagIdPriorityResult parse_diagid_priority_response(const ByteBuffer& payload) {
    DiagIdPriorityResult result;

    // Expected: [1] version, [1] opcode, [1] diag_id, [1] status
    if (payload.size() < 4) return result;

    result.version = payload[0];
    result.opcode = payload[1];
    result.diag_id = payload[2];
    result.status = payload[3];
    result.valid = true;
    return result;
}

NonHdlcSwitchResult parse_non_hdlc_switch_response(const ByteBuffer& payload) {
    NonHdlcSwitchResult result;

    // Expected: [1] framingVersion, [1] result (0=success)
    // Confirmed from QUTS emulator: response bytes { 0x4B, 0x12, 0x18, 0x02, 0x01, 0x00 }
    // After extracting subsystem header (4 bytes), payload is: [0x01][0x00]
    if (payload.size() < 2) return result;

    result.framing_version = payload[0];
    result.result_code = payload[1];
    result.valid = true;
    return result;
}

DiagIdTableResult parse_diagid_table_response(const ByteBuffer& payload) {
    DiagIdTableResult result;

    // Minimum: [1] version, [1] num_entries
    if (payload.size() < 2) return result;

    result.version = payload[0];
    uint8_t num_entries = payload[1];

    size_t offset = 2;
    for (uint8_t i = 0; i < num_entries && offset < payload.size(); ++i) {
        DiagIdEntry entry;
        entry.diag_id = payload[offset];
        offset += 1;

        // Read null-terminated processor name string
        size_t name_start = offset;
        while (offset < payload.size() && payload[offset] != 0) {
            ++offset;
        }
        if (offset <= payload.size()) {
            entry.processor_name = std::string(
                reinterpret_cast<const char*>(payload.data() + name_start),
                offset - name_start);
        }
        // Skip null terminator
        if (offset < payload.size()) {
            ++offset;
        }

        result.entries.push_back(std::move(entry));
    }

    result.valid = true;
    return result;
}

QshrinkFileListResult parse_qshrink_file_list(const ByteBuffer& payload) {
    QshrinkFileListResult result;

    // Minimum: [1] status, [1] num_files
    if (payload.size() < 2) return result;

    result.status = payload[0];
    uint8_t num_files = payload[1];
    result.valid = true;

    size_t offset = 2;
    for (uint8_t i = 0; i < num_files; ++i) {
        // Each entry: [16] GUID + [4] file_length
        if (offset + 20 > payload.size()) break;

        QshrinkFileEntry entry;
        std::copy(payload.data() + offset, payload.data() + offset + 16, entry.guid.begin());
        offset += 16;

        entry.file_length = static_cast<uint32_t>(payload[offset])
                          | (static_cast<uint32_t>(payload[offset + 1]) << 8)
                          | (static_cast<uint32_t>(payload[offset + 2]) << 16)
                          | (static_cast<uint32_t>(payload[offset + 3]) << 24);
        offset += 4;

        result.files.push_back(std::move(entry));
    }

    return result;
}

// --- Device info response parsers ---

static std::string read_fixed_string(const uint8_t* data, size_t len) {
    size_t actual = 0;
    while (actual < len && data[actual] != '\0') ++actual;
    return std::string(reinterpret_cast<const char*>(data), actual);
}

static uint16_t read_u16_le(const uint8_t* data) {
    return static_cast<uint16_t>(data[0]) | (static_cast<uint16_t>(data[1]) << 8);
}

static uint32_t read_u32_le(const uint8_t* data) {
    return static_cast<uint32_t>(data[0]) | (static_cast<uint32_t>(data[1]) << 8)
         | (static_cast<uint32_t>(data[2]) << 16) | (static_cast<uint32_t>(data[3]) << 24);
}

VernoResponse parse_verno_response(const ByteBuffer& response) {
    VernoResponse result;
    if (response.size() < 55 || response[0] != DiagCmd::VERNO) {
        CFLOG_ERROR("Invalid VERNO response (size=" + std::to_string(response.size()) + ", expected >= 55)", true);
        return result;
    }
    result.raw = response;
    const uint8_t* p = response.data() + 1;
    result.compile_date = read_fixed_string(p, 11); p += 11;
    result.compile_time = read_fixed_string(p, 8); p += 8;
    result.release_date = read_fixed_string(p, 11); p += 11;
    result.release_time = read_fixed_string(p, 8); p += 8;
    result.version_directory = read_fixed_string(p, 8); p += 8;
    result.station_class_mark = *p++;
    result.mobile_cai = *p++;
    result.manufacturers_model = *p++;
    result.firmware_revision = read_u16_le(p); p += 2;
    result.slot_cycle_index = *p++;
    result.msm_revision = read_u16_le(p);
    result.valid = true;
    return result;
}

ExtBuildIdResponse parse_ext_build_id_response(const ByteBuffer& response) {
    ExtBuildIdResponse result;
    if (response.size() < 26 || response[0] != DiagCmd::EXT_BUILD_ID) {
        CFLOG_ERROR("Invalid EXT_BUILD_ID response (size=" + std::to_string(response.size()) + ", expected >= 26)", true);
        return result;
    }
    result.raw = response;
    size_t offset = 1;
    result.version = response[offset++];
    result.reserved.assign(response.begin() + offset, response.begin() + offset + 16);
    offset += 16;
    result.msm_revision = read_u32_le(response.data() + offset); offset += 4;
    result.model_number = read_u32_le(response.data() + offset); offset += 4;
    if (offset < response.size()) {
        const char* s = reinterpret_cast<const char*>(response.data() + offset);
        size_t len = strnlen(s, response.size() - offset);
        result.software_revision = std::string(s, len);
        offset += len;
        if (offset < response.size()) ++offset;
    }
    if (offset < response.size()) {
        const char* s = reinterpret_cast<const char*>(response.data() + offset);
        size_t len = strnlen(s, response.size() - offset);
        result.model_name = std::string(s, len);
    }
    result.valid = true;
    return result;
}

EsnResponse parse_esn_response(const ByteBuffer& response) {
    EsnResponse result;
    if (response.size() < 5 || response[0] != DiagCmd::ESN) {
        CFLOG_ERROR("Invalid ESN response (size=" + std::to_string(response.size()) + ", expected >= 5)", true);
        return result;
    }
    result.raw = response;
    result.esn = read_u32_le(response.data() + 1);
    result.valid = true;
    return result;
}

StatusResponse parse_status_response(const ByteBuffer& response) {
    StatusResponse result;
    if (response.empty() || response[0] != DiagCmd::STATUS) {
        CFLOG_ERROR("Invalid STATUS response (size=" + std::to_string(response.size()) + ")", true);
        return result;
    }
    result.raw = response;
    result.valid = true;
    return result;
}

} // namespace diaglogger
