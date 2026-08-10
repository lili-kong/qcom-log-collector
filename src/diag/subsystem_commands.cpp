// Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
// SPDX-License-Identifier: BSD 3-Clause Clear License

#include "subsystem_commands.h"

namespace QC {

ByteBuffer build_subsystem_command(uint8_t subsys_id, uint16_t subcmd,
                                   const ByteBuffer& payload) {
    return build_subsystem_command(subsys_id, subcmd,
                                  payload.data(), payload.size());
}

ByteBuffer build_subsystem_command(uint8_t subsys_id, uint16_t subcmd,
                                   const uint8_t* payload, size_t payload_len) {
    ByteBuffer pkt;
    pkt.reserve(4 + payload_len);

    // [1] cmd_id = 0x4B
    pkt.push_back(DiagCmd::SUBSYS);
    // [1] subsystem_id
    pkt.push_back(subsys_id);
    // [2] subsystem_cmd (little-endian)
    pkt.push_back(static_cast<uint8_t>(subcmd & 0xFF));
    pkt.push_back(static_cast<uint8_t>((subcmd >> 8) & 0xFF));
    // [N] payload
    if (payload && payload_len > 0) {
        pkt.insert(pkt.end(), payload, payload + payload_len);
    }

    return pkt;
}

ByteBuffer build_subsystem_v2_command(uint8_t subsys_id, uint16_t subcmd,
                                      const ByteBuffer& payload) {
    ByteBuffer pkt;
    pkt.reserve(4 + payload.size());

    // [1] cmd_id = 0x80 (SUBSYS_V2)
    pkt.push_back(DiagCmd::SUBSYS_V2);
    // [1] subsystem_id
    pkt.push_back(subsys_id);
    // [2] subsystem_cmd (little-endian)
    pkt.push_back(static_cast<uint8_t>(subcmd & 0xFF));
    pkt.push_back(static_cast<uint8_t>((subcmd >> 8) & 0xFF));
    // [N] payload
    if (!payload.empty()) {
        pkt.insert(pkt.end(), payload.begin(), payload.end());
    }

    return pkt;
}

bool is_valid_subsystem_response(const ByteBuffer& response,
                                 uint8_t subsys_id, uint16_t subcmd) {
    if (response.size() < 4) { return false; }
    if (response[0] != DiagCmd::SUBSYS) { return false; }
    if (response[1] != subsys_id) { return false; }
    uint16_t resp_cmd = static_cast<uint16_t>(response[2])
                      | (static_cast<uint16_t>(response[3]) << 8);
    return resp_cmd == subcmd;
}

ByteBuffer extract_subsystem_payload(const ByteBuffer& response) {
    if (response.size() <= 4) { return {}; }
    return ByteBuffer(response.begin() + 4, response.end());
}

} // namespace QC
