// Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
// SPDX-License-Identifier: BSD 3-Clause Clear License

#include <cstring>

#include "adpl_commands.h"

namespace QC {

// --- Command builders ---

ByteBuffer build_adpl_enable(uint8_t version) {
    // subsys=0x5A (IPA), cmd=0x4000 (ACCELERATED_IPA_REQUEST)
    // Payload format (from TUF AdplDiagManager):
    //   [0] dplVersion     - requested DPL version (1-7)
    //   [1] packetLimit    - 0 for enable
    //   [2-3] maxPacketSize - 15360 (0x3C00) little-endian
    //   [4] dplCommand     - 1 = ENABLE
    //   [5] reserved       - 0
    constexpr uint16_t MAX_PACKET_SIZE = 15360;  // 0x3C00
    ByteBuffer payload = {
        version,                                          // dplVersion
        0,                                                // packetLimit (0 for enable)
        static_cast<uint8_t>(MAX_PACKET_SIZE & 0xFF),     // maxPacketSize low byte
        static_cast<uint8_t>((MAX_PACKET_SIZE >> 8) & 0xFF), // maxPacketSize high byte
        AdplDplCommand::ENABLE,                           // dplCommand = 1
        0                                                 // reserved
    };
    return build_subsystem_command(SubsysId::IPA, AdplCmd::ACCELERATED_IPA_REQUEST, payload);
}

ByteBuffer build_adpl_disable() {
    // Payload format (from TUF AdplDiagManager):
    //   [0] dplVersion     - HIGHEST_SUPPORTED
    //   [1] packetLimit    - 4 for disable
    //   [2-3] maxPacketSize - 15360 (0x3C00) little-endian
    //   [4] dplCommand     - 0 = DISABLE
    //   [5] reserved       - 0
    constexpr uint16_t MAX_PACKET_SIZE = 15360;  // 0x3C00
    ByteBuffer payload = {
        AdplVersion::HIGHEST_SUPPORTED,                   // dplVersion
        4,                                                // packetLimit (4 for disable)
        static_cast<uint8_t>(MAX_PACKET_SIZE & 0xFF),     // maxPacketSize low byte
        static_cast<uint8_t>((MAX_PACKET_SIZE >> 8) & 0xFF), // maxPacketSize high byte
        AdplDplCommand::DISABLE,                          // dplCommand = 0
        0                                                 // reserved
    };
    return build_subsystem_command(SubsysId::IPA, AdplCmd::ACCELERATED_IPA_REQUEST, payload);
}

AdplEnableResponse parse_adpl_enable_response(const ByteBuffer& payload) {
    AdplEnableResponse resp;
    // Response payload (after 4-byte subsys header): [0] version, [1] status
    // Based on TUF AdplDiagManager which reads "DPL Version" then "DPL Status"
    if (payload.empty()) return resp;
    resp.version = payload[0];
    if (payload.size() >= 2) {
        resp.status = payload[1];
    }
    return resp;
}

// --- ADPL packet decoder ---
static uint16_t read_le16(const uint8_t* p) {
    return static_cast<uint16_t>(p[0]) | (static_cast<uint16_t>(p[1]) << 8);
}

static uint16_t read_be16(const uint8_t* p) {
    // AdplV1Header.m_packetLength is byte-swapped (big-endian) in Adpl.cpp receiveData().
    return static_cast<uint16_t>((static_cast<uint16_t>(p[0]) << 8) | static_cast<uint16_t>(p[1]));
}

static uint32_t read_le32(const uint8_t* p) {
    return static_cast<uint32_t>(p[0])
         | (static_cast<uint32_t>(p[1]) << 8)
         | (static_cast<uint32_t>(p[2]) << 16)
         | (static_cast<uint32_t>(p[3]) << 24);
}

// 48-bit little-endian read (dplTimeOfDay is a 48-bit bitfield in V2..V7 headers).
static uint64_t read_le48(const uint8_t* p) {
    uint64_t v = 0;
    for (int i = 0; i < 6; ++i) {
        v |= (static_cast<uint64_t>(p[i]) << (i * 8));
    }
    return v;
}

uint8_t detect_adpl_version(const uint8_t* data, size_t len) {
    // NOTE: The reference implementation (TUF Device::Protocol::Adpl::receiveData) never
    // derives the ADPL version from packet bytes -- it always switches on the version
    // negotiated in the enable response (AdplDiagManager::getDplVersion()). Callers should
    // do the same. This helper can only classify the statusOpcode *family*: it cannot tell
    // V3/V4/V5 apart, nor V6 from V7, because those share identical header layouts
    // (AdplV3Header == AdplV5Header == AdplV6Header == 16 bytes in Frame.h).
    if (!data || len < 1) return 0;

    // V3/V4/V5: statusOpcode occupies the whole first byte and equals 0x10.
    if (data[0] == AdplStatusOpcode::VALID_V3_V5) {
        return AdplVersion::VERSION3;  // family representative
    }

    // V6/V7: statusOpcode is only the low nibble of the first byte (0x0C or 0x0D);
    // the high nibble carries the timeOfDay bits (AdplV6Header.m_timeOfDay).
    uint8_t opcode = static_cast<uint8_t>(data[0] & 0x0F);
    if (opcode == AdplStatusOpcode::VALID_V6_V7a ||
        opcode == AdplStatusOpcode::VALID_V6_V7b) {
        return AdplVersion::VERSION6;  // family representative
    }

    return 0;
}

size_t adpl_header_size(uint8_t version) {
    // Sizes fixed by TOOLS_STATIC_ASSERTs in Frame.h; V1 from AdplV1Header in Adpl.cpp.
    switch (version) {
        case AdplVersion::VERSION1: return 4;   // AdplV1Header: padCd:6/reserved:2 + muxId + packetLength(2,BE)
        case AdplVersion::VERSION2: return 32;  // AdplV2Header (TOOLS_STATIC_ASSERT(32 == sizeof))
        case AdplVersion::VERSION3: return 16;  // AdplV3Header (16); V3 and V4 share the struct
        case AdplVersion::VERSION4: return 16;  // AdplV3Header (V3 == V4)
        case AdplVersion::VERSION5: return 16;  // AdplV5Header (16)
        case AdplVersion::VERSION6: return 16;  // AdplV6Header (16); V6 and V7 share the struct
        case AdplVersion::VERSION7: return 16;  // AdplV6Header (V6 == V7)
        default: return 0;
    }
}

size_t adpl_padding(uint8_t version, size_t payload_len) {
    // Alignment rules from Adpl.cpp receiveData(): V2 -> 32-bit, V3/V4/V5/V6 -> 8-byte,
    // V7 -> 16-byte. V1 carries its pad count in the header's m_padCd field (no
    // separate alignment padding), so this returns 0 for V1.
    switch (version) {
        case AdplVersion::VERSION2: {  // pad to 32-bit (4-byte) boundary
            size_t remainder = payload_len % 4;
            return (remainder == 0) ? 0 : (4 - remainder);
        }
        case AdplVersion::VERSION3:
        case AdplVersion::VERSION4:
        case AdplVersion::VERSION5:
        case AdplVersion::VERSION6: {  // pad to 8-byte boundary
            size_t remainder = payload_len % 8;
            return (remainder == 0) ? 0 : (8 - remainder);
        }
        case AdplVersion::VERSION7: {  // pad to 16-byte boundary
            size_t remainder = payload_len % 16;
            return (remainder == 0) ? 0 : (16 - remainder);
        }
        default:  // V1 (and unknown): no alignment padding
            return 0;
    }
}

// Reads the payload length field from an ADPL header per the version's struct layout.
static size_t adpl_read_payload_len(uint8_t version, const uint8_t* data) {
    switch (version) {
        // AdplV1Header: packetLength is big-endian at [2..3]; m_padCd (low 6 bits of
        // byte 0) is added to it (Adpl.cpp: payloadLength = byteSwap(len) + m_padCd).
        case AdplVersion::VERSION1:
            return static_cast<size_t>(read_be16(data + 2)) +
                   static_cast<size_t>(data[0] & 0x3F);

        // AdplV2Header: m_packetLength (Uint16) at offset 4, little-endian.
        case AdplVersion::VERSION2:
            return read_le16(data + 4);

        // Adpl{V3/V4/V5/V6/V7}Header: m_packetLength is the top 16 bits of the 64-bit word at
        // offset 8 (m_dplTimeOfDay:48 | m_packetLength:16) -> bytes [14..15], little-endian.
        default:
            return read_le16(data + 14);
    }
}

AdplPacket decode_adpl_packet(uint8_t version, const uint8_t* data, size_t len) {
    AdplPacket pkt;
    pkt.version = version;

    size_t hdr_size = adpl_header_size(version);
    if (hdr_size == 0 || len < hdr_size) return pkt;

    size_t payload_len = 0;

    switch (version) {
        case AdplVersion::VERSION1: {
            // AdplV1Header: 
            // [0] padCd:6|reserved:2, 
            // [1] muxId, 
            // [2..3] packetLength (BE)
            pkt.mux_id = data[1];
            payload_len = adpl_read_payload_len(version, data);
            break;
        }
        case AdplVersion::VERSION2: {
            // AdplV2Header (32B): 
            // [0] statusOpcode, 
            // [4..5] packetLength (LE), 
            // [9] muxId,
            // [18..23] dplTimeOfDay (48b), 
            // [24] sequenceNumber
            // rest of the bytes are reserved
            pkt.mux_id = data[9];
            payload_len = adpl_read_payload_len(version, data);
            pkt.sequence_number = data[24];
            pkt.timestamp_us = read_le48(data + 18);
            break;
        }
        case AdplVersion::VERSION3:
        case AdplVersion::VERSION4:
        case AdplVersion::VERSION5: {
            // AdplV3Header/AdplV5Header (16B): 
            // [0] statusOpcode(0x10), 
            // [1] sequenceNumber,
            // [5] muxId, 
            // [8..13] dplTimeOfDay (48b), 
            // [14..15] packetLength (LE)
            // rest of the bytes are reserved
            if (data[0] != AdplStatusOpcode::VALID_V3_V5) return pkt;
            pkt.sequence_number = data[1];
            pkt.mux_id = data[5];
            pkt.timestamp_us = read_le48(data + 8);
            payload_len = adpl_read_payload_len(version, data);
            break;
        }
        case AdplVersion::VERSION6:
        case AdplVersion::VERSION7: {
            // AdplV6Header (16B): 
            // [0] statusOpcode:4|timeOfDay:4, 
            // [1] sequenceNumber,
            // [5] muxId, 
            // [8..13] dplTimeOfDay (48b), 
            // [14..15] packetLength (LE)
            // rest of the bytes are reserved
            // Full timestamp = dplTimeOfDay | (timeOfDay << 48)  (as per Adpl.cpp receiveData function).
            uint8_t opcode = static_cast<uint8_t>(data[0] & 0x0F);
            if (opcode != AdplStatusOpcode::VALID_V6_V7a &&
                opcode != AdplStatusOpcode::VALID_V6_V7b) return pkt;
            uint8_t time_of_day_hi = static_cast<uint8_t>(data[0] >> 4);
            pkt.sequence_number = data[1];
            pkt.mux_id = data[5];
            pkt.timestamp_us = read_le48(data + 8) |
                               (static_cast<uint64_t>(time_of_day_hi) << 48);
            payload_len = adpl_read_payload_len(version, data);
            break;
        }
        default:
            return pkt;
    }

    // Validate we have enough data for the header + payload (padding may be absent
    // at the tail of a buffer, matching the reference's std::min clamp).
    if (len < hdr_size + payload_len) return pkt;

    // Extract payload
    pkt.diag_payload.assign(data + hdr_size, data + hdr_size + payload_len);
    pkt.valid = true;
    return pkt;
}

size_t adpl_frame_size(uint8_t version, const uint8_t* data, size_t len) {
    size_t hdr_size = adpl_header_size(version);
    if (hdr_size == 0 || len < hdr_size) return 0;

    // Payload length field location/endianness depends on the version's header struct.
    size_t payload_len = adpl_read_payload_len(version, data);
    size_t pad = adpl_padding(version, payload_len);

    return hdr_size + payload_len + pad;
}

} // namespace QC
