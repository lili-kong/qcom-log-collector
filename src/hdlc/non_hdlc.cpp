// SPDX-License-Identifier: Apache-2.0
// Copyright 2024 Qualcomm Innovation Center, Inc.

#include "non_hdlc.h"

namespace diaglogger {

ByteBuffer non_hdlc_encode(const uint8_t* data, size_t len) {
    // Frame format: [0x7E][version=0x01][len_lo][len_hi][payload...][0x7E]
    // Length field is the payload length only (little-endian 16-bit).
    ByteBuffer frame;
    frame.reserve(4 + len + 1);  // header(4) + payload + trailing flag(1)

    frame.push_back(NON_HDLC_FLAG);
    frame.push_back(NON_HDLC_VERSION);
    frame.push_back(static_cast<uint8_t>(len & 0xFF));
    frame.push_back(static_cast<uint8_t>((len >> 8) & 0xFF));
    frame.insert(frame.end(), data, data + len);
    frame.push_back(NON_HDLC_FLAG);

    return frame;
}

ByteBuffer non_hdlc_encode(const ByteBuffer& data) {
    return non_hdlc_encode(data.data(), data.size());
}

void NonHdlcDecoder::feed(const uint8_t* data, size_t len) {
    for (size_t i = 0; i < len; ++i) {
        process_byte(data[i]);
    }
}

void NonHdlcDecoder::feed(const ByteBuffer& data) {
    feed(data.data(), data.size());
}

void NonHdlcDecoder::process_byte(uint8_t byte) {
    switch (state_) {
    case State::WAIT_FLAG:
        if (byte == NON_HDLC_FLAG) {
            state_ = State::WAIT_VERSION;
        }
        break;

    case State::WAIT_VERSION:
        if (byte == NON_HDLC_FLAG) {
            // Consecutive flags — stay in WAIT_VERSION (the new flag
            // could be the start of a frame, and next byte is version).
            break;
        }
        if (byte == NON_HDLC_VERSION) {
            version_ = byte;
            state_ = State::WAIT_LEN_LO;
        } else {
            // Unexpected version — treat as error, resync
            ++errors_;
            state_ = State::WAIT_FLAG;
        }
        break;

    case State::WAIT_LEN_LO:
        expected_len_ = byte;
        state_ = State::WAIT_LEN_HI;
        break;

    case State::WAIT_LEN_HI:
        expected_len_ |= static_cast<uint16_t>(byte) << 8;
        if (expected_len_ == 0) {
            // Zero-length frame: expect trailing flag immediately
            state_ = State::WAIT_END_FLAG;
        } else {
            accumulator_.clear();
            accumulator_.reserve(expected_len_);
            state_ = State::READING_PAYLOAD;
        }
        break;

    case State::READING_PAYLOAD:
        accumulator_.push_back(byte);
        if (accumulator_.size() == expected_len_) {
            state_ = State::WAIT_END_FLAG;
        }
        break;

    case State::WAIT_END_FLAG:
        if (byte == NON_HDLC_FLAG) {
            // Frame complete — store payload
            completed_frames_.push_back(std::move(accumulator_));
            accumulator_.clear();
            // The trailing 0x7E also serves as the start flag for the next frame
            state_ = State::WAIT_VERSION;
        } else {
            // Expected trailing flag but got data — frame error.
            // The data stream may be corrupted or we're misaligned.
            ++errors_;
            accumulator_.clear();
            state_ = State::WAIT_FLAG;
        }
        break;
    }
}

std::optional<ByteBuffer> NonHdlcDecoder::next_frame() {
    if (completed_frames_.empty()) {
        return std::nullopt;
    }
    ByteBuffer frame = std::move(completed_frames_.front());
    completed_frames_.erase(completed_frames_.begin());
    return frame;
}

void NonHdlcDecoder::reset() {
    state_ = State::WAIT_FLAG;
    version_ = 0;
    expected_len_ = 0;
    accumulator_.clear();
    completed_frames_.clear();
    // Don't reset error count — it's cumulative
}

} // namespace diaglogger
