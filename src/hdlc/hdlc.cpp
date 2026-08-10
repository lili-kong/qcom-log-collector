// Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
// SPDX-License-Identifier: BSD 3-Clause Clear License

#include <KL/Macros.h>
#include <sstream>

#include "hdlc.h"
#include "crc16.h"

namespace QC {

ByteBuffer hdlc_encode(const uint8_t* data, size_t len) {
    // Compute CRC over the raw payload
    uint16_t crc = crc16(data, len);

    // Build unescaped payload + CRC (little-endian)
    ByteBuffer raw;
    raw.reserve(len + 2);
    raw.insert(raw.end(), data, data + len);
    raw.push_back(static_cast<uint8_t>(crc & 0xFF));
    raw.push_back(static_cast<uint8_t>((crc >> 8) & 0xFF));

    // Escape and frame — matches TUF's FrameStream_Hdlc.cpp format:
    // escaped_payload + escaped_CRC + trailing_0x7E (no leading 0x7E).
    // The trailing flag of the PREVIOUS frame serves as the start delimiter
    // for the current frame.  The very first frame in a session starts
    // implicitly (the raw HDLC loopback sent by do_loopback() provides
    // the initial 0x7E delimiter via its trailing flag byte).
    ByteBuffer frame;
    frame.reserve(raw.size() * 2 + 1);
    for (uint8_t byte : raw) {
        if (byte == HDLC_FLAG || byte == HDLC_ESCAPE) {
            frame.push_back(HDLC_ESCAPE);
            frame.push_back(byte ^ HDLC_ESCAPE_MASK);
        } else {
            frame.push_back(byte);
        }
    }
    frame.push_back(HDLC_FLAG);
    return frame;
}

ByteBuffer hdlc_encode(const ByteBuffer& data) {
    return hdlc_encode(data.data(), data.size());
}

void HdlcDecoder::feed(const uint8_t* data, size_t len) {
    for (size_t i = 0; i < len; ++i) {
        process_byte(data[i]);
    }
}

void HdlcDecoder::feed(const ByteBuffer& data) {
    feed(data.data(), data.size());
}

void HdlcDecoder::process_byte(uint8_t byte) {
    if (byte == HDLC_FLAG) {
        // 0x7E = frame delimiter.  Finalize whatever has been accumulated.
        // Matches TUF's frame format: payload + CRC + 0x7E (no leading flag).
        // Frames that start implicitly (without a leading 0x7E) are common
        // in the Qualcomm diag protocol.
        if (!accumulator_.empty()) {
            finalize_frame();
        }
        escape_next_ = false;
        return;
    }

    if (byte == HDLC_ESCAPE) {
        escape_next_ = true;
        return;
    }

    if (escape_next_) {
        byte ^= HDLC_ESCAPE_MASK;
        escape_next_ = false;
    }

    accumulator_.push_back(byte);
}

void HdlcDecoder::finalize_frame() {
    // Need at least 2 bytes for CRC + 1 byte payload
    if (accumulator_.size() < 3) {
        accumulator_.clear();
        return;
    }

    // Verify CRC: compute over payload, compare with trailing 2 bytes
    size_t payload_len = accumulator_.size() - 2;
    uint16_t computed_crc = crc16(accumulator_.data(), payload_len);
    uint16_t received_crc = static_cast<uint16_t>(accumulator_[payload_len])
                          | (static_cast<uint16_t>(accumulator_[payload_len + 1]) << 8);

    if (computed_crc != received_crc) {
        ++crc_errors_;
        FLOG_DEBUG("HDLC CRC error: computed=0x" + ([&]{
            std::ostringstream ss;
            ss << std::hex << computed_crc;
            return ss.str();
        })() + " received=0x" + ([&]{
            std::ostringstream ss;
            ss << std::hex << received_crc;
            return ss.str();
        })());
        accumulator_.clear();
        return;
    }

    // Strip CRC, store completed payload
    accumulator_.resize(payload_len);
    completed_frames_.push_back(std::move(accumulator_));
    accumulator_.clear();
}

std::optional<ByteBuffer> HdlcDecoder::next_frame() {
    if (completed_frames_.empty()) {
        return std::nullopt;
    }
    ByteBuffer frame = std::move(completed_frames_.front());
    completed_frames_.erase(completed_frames_.begin());
    return frame;
}

} // namespace QC
