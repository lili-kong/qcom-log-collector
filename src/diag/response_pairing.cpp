// Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
// SPDX-License-Identifier: BSD 3-Clause Clear License

#include "diag/response_pairing.h"

namespace diaglogger {

ByteBuffer compute_response_signature(const ByteBuffer& request_payload) {
    if (request_payload.empty()) {
        return {};
    }
    const uint8_t cmd = request_payload[0];
    if (cmd == PairingCmd::SUBSYS || cmd == PairingCmd::SUBSYS_V2) {
        const size_t n = request_payload.size() < 4 ? request_payload.size() : 4;
        return ByteBuffer(request_payload.begin(), request_payload.begin() + n);
    }
    return ByteBuffer{cmd};
}

bool response_matches_signature(const ByteBuffer& signature, const ByteBuffer& frame) {
    if (signature.empty()) {
        return false;
    }
    if (frame.size() < signature.size()) {
        return false;
    }
    for (size_t i = 0; i < signature.size(); ++i) {
        if (frame[i] != signature[i]) {
            return false;
        }
    }
    return true;
}

static bool is_error_cmd(uint8_t cmd) {
    return cmd == DiagError::BAD_CMD
        || cmd == DiagError::BAD_PARM
        || cmd == DiagError::BAD_LEN
        || cmd == DiagError::BAD_DEV
        || cmd == DiagError::BAD_MODE
        || cmd == DiagError::BAD_SPC_MODE;
}

bool error_response_matches_signature(const ByteBuffer& signature, const ByteBuffer& frame) {
    if (signature.empty() || frame.size() < 2) {
        return false;
    }
    if (!is_error_cmd(frame[0])) {
        return false;
    }
    // Error response format: [error_code][original_cmd_code][...]
    // Match original_cmd_code against the first byte of the signature.
    return frame[1] == signature[0];
}

bool is_error_response(const ByteBuffer& frame) {
    if (frame.empty()) {
        return false;
    }
    return is_error_cmd(frame[0]);
}

} // namespace diaglogger
