// SPDX-License-Identifier: Apache-2.0
// Copyright 2024 Qualcomm Innovation Center, Inc.

#include "adpl_commands.h"
#include <cstring>

namespace diaglogger {

// --- Command builders ---

ByteBuffer build_adpl_enable(uint8_t version) {
    // subsys=0x5A (IPA), cmd=0x4000 (ACCELERATED_IPA_REQUEST)
    // Payload: [1] dpl_command=ENABLE(1), [1] requested_version
    ByteBuffer payload = { AdplDplCommand::ENABLE, version };
    return build_subsystem_command(SubsysId::IPA, AdplCmd::ACCELERATED_IPA_REQUEST, payload);
}

ByteBuffer build_adpl_disable() {
    // Payload: [1] dpl_command=DISABLE(0)
    ByteBuffer payload = { AdplDplCommand::DISABLE };
    return build_subsystem_command(SubsysId::IPA, AdplCmd::ACCELERATED_IPA_REQUEST, payload);
}

AdplEnableResponse parse_adpl_enable_response(const ByteBuffer& payload) {
    AdplEnableResponse resp;
    // Response payload (after 4-byte subsys header): [1] status, [1] version
    if (payload.empty()) return resp;
    resp.status = payload[0];
    if (payload.size() >= 2) {
        resp.version = payload[1];
    }
    return resp;
}

// --- ADPL packet decoder ---

uint8_t detect_adpl_version(const uint8_t* data, size_t len) {
    if (!data || len < 4) return 0;

    // V1/V2: first byte is version (1 or 2), second byte is mux_id
    if (data[0] == AdplVersion::VERSION1 || data[0] == AdplVersion::VERSION2) {
        return data[0];
    }

    // V3-V5: statusOpcode at offset 0 is 0x10
    if (data[0] == AdplStatusOpcode::VALID_V3_V5) {
        // Distinguish V3/V4/V5 by header length field or other markers
        // V3: 8-byte header, V4: 12-byte header, V5: 16-byte header
        if (len >= 16) return AdplVersion::VERSION5;
        if (len >= 12) return AdplVersion::VERSION4;
        return AdplVersion::VERSION3;
    }

    // V6/V7: statusOpcode at offset 0 is 0x0C or 0x0D
    if (data[0] == AdplStatusOpcode::VALID_V6_V7a ||
        data[0] == AdplStatusOpcode::VALID_V6_V7b) {
        // V7 has a 24-byte header, V6 has a 20-byte header
        // Distinguish by checking if there's enough data for V7 timestamp fields
        if (len >= 24) return AdplVersion::VERSION7;
        return AdplVersion::VERSION6;
    }

    return 0;
}

size_t adpl_header_size(uint8_t version) {
    switch (version) {
        case AdplVersion::VERSION1: return 4;   // version(1) + mux_id(1) + length(2)
        case AdplVersion::VERSION2: return 4;   // version(1) + mux_id(1) + length(2)
        case AdplVersion::VERSION3: return 8;   // statusOpcode(1) + mux_id(1) + length(2) + seq(2) + pad(2)
        case AdplVersion::VERSION4: return 12;  // V3 + timestamp_lo(4)
        case AdplVersion::VERSION5: return 16;  // V3 + timestamp(8)
        case AdplVersion::VERSION6: return 20;  // statusOpcode(1) + mux_id(1) + length(2) + seq(2) + pad(2) + timestamp(8) + reserved(4)
        case AdplVersion::VERSION7: return 24;  // statusOpcode(1) + mux_id(1) + length(2) + seq(2) + pad(2) + timestamp(8) + reserved(8)
        default: return 0;
    }
}

size_t adpl_padding(uint8_t version, size_t payload_len) {
    // V6: 8-byte aligned payload
    if (version == AdplVersion::VERSION6) {
        size_t remainder = payload_len % 8;
        return (remainder == 0) ? 0 : (8 - remainder);
    }
    // V7: 16-byte aligned payload
    if (version == AdplVersion::VERSION7) {
        size_t remainder = payload_len % 16;
        return (remainder == 0) ? 0 : (16 - remainder);
    }
    // V1-V5: no padding
    return 0;
}

static uint16_t read_le16(const uint8_t* p) {
    return static_cast<uint16_t>(p[0]) | (static_cast<uint16_t>(p[1]) << 8);
}

static uint32_t read_le32(const uint8_t* p) {
    return static_cast<uint32_t>(p[0])
         | (static_cast<uint32_t>(p[1]) << 8)
         | (static_cast<uint32_t>(p[2]) << 16)
         | (static_cast<uint32_t>(p[3]) << 24);
}

static uint64_t read_le64(const uint8_t* p) {
    return static_cast<uint64_t>(read_le32(p))
         | (static_cast<uint64_t>(read_le32(p + 4)) << 32);
}

AdplPacket decode_adpl_packet(uint8_t version, const uint8_t* data, size_t len) {
    AdplPacket pkt;
    pkt.version = version;

    size_t hdr_size = adpl_header_size(version);
    if (hdr_size == 0 || len < hdr_size) return pkt;

    uint16_t payload_len = 0;

    switch (version) {
        case AdplVersion::VERSION1:
        case AdplVersion::VERSION2: {
            // [0] version, [1] mux_id, [2..3] length (LE)
            pkt.mux_id = data[1];
            payload_len = read_le16(data + 2);
            break;
        }
        case AdplVersion::VERSION3: {
            // [0] statusOpcode, [1] mux_id, [2..3] length, [4..5] sequence, [6..7] pad
            if (data[0] != AdplStatusOpcode::VALID_V3_V5) return pkt;
            pkt.mux_id = data[1];
            payload_len = read_le16(data + 2);
            pkt.sequence_number = read_le16(data + 4);
            break;
        }
        case AdplVersion::VERSION4: {
            // [0] statusOpcode, [1] mux_id, [2..3] length, [4..5] sequence, [6..7] pad, [8..11] timestamp_lo
            if (data[0] != AdplStatusOpcode::VALID_V3_V5) return pkt;
            pkt.mux_id = data[1];
            payload_len = read_le16(data + 2);
            pkt.sequence_number = read_le16(data + 4);
            pkt.timestamp_us = static_cast<uint64_t>(read_le32(data + 8));
            break;
        }
        case AdplVersion::VERSION5: {
            // [0] statusOpcode, [1] mux_id, [2..3] length, [4..5] sequence, [6..7] pad, [8..15] timestamp
            if (data[0] != AdplStatusOpcode::VALID_V3_V5) return pkt;
            pkt.mux_id = data[1];
            payload_len = read_le16(data + 2);
            pkt.sequence_number = read_le16(data + 4);
            pkt.timestamp_us = read_le64(data + 8);
            break;
        }
        case AdplVersion::VERSION6: {
            // [0] statusOpcode, [1] mux_id, [2..3] length, [4..5] sequence, [6..7] pad,
            // [8..15] timestamp, [16..19] reserved
            if (data[0] != AdplStatusOpcode::VALID_V6_V7a &&
                data[0] != AdplStatusOpcode::VALID_V6_V7b) return pkt;
            pkt.mux_id = data[1];
            payload_len = read_le16(data + 2);
            pkt.sequence_number = read_le16(data + 4);
            pkt.timestamp_us = read_le64(data + 8);
            break;
        }
        case AdplVersion::VERSION7: {
            // [0] statusOpcode, [1] mux_id, [2..3] length, [4..5] sequence, [6..7] pad,
            // [8..15] timestamp, [16..23] reserved
            if (data[0] != AdplStatusOpcode::VALID_V6_V7a &&
                data[0] != AdplStatusOpcode::VALID_V6_V7b) return pkt;
            pkt.mux_id = data[1];
            payload_len = read_le16(data + 2);
            pkt.sequence_number = read_le16(data + 4);
            pkt.timestamp_us = read_le64(data + 8);
            break;
        }
        default:
            return pkt;
    }

    // Validate we have enough data for the payload
    size_t pad = adpl_padding(version, payload_len);
    if (len < hdr_size + payload_len) return pkt;

    // Extract payload
    pkt.diag_payload.assign(data + hdr_size, data + hdr_size + payload_len);
    pkt.valid = true;
    return pkt;
}

size_t adpl_frame_size(uint8_t version, const uint8_t* data, size_t len) {
    size_t hdr_size = adpl_header_size(version);
    if (hdr_size == 0 || len < hdr_size) return 0;

    // Payload length is at offset 2 (LE 16-bit) for all versions
    uint16_t payload_len = read_le16(data + 2);
    size_t pad = adpl_padding(version, payload_len);

    return hdr_size + payload_len + pad;
}

} // namespace diaglogger
