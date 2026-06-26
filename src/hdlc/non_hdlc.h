// SPDX-License-Identifier: Apache-2.0
// Copyright 2024 Qualcomm Innovation Center, Inc.
//
// Non-HDLC (length-prefixed) encoder and streaming decoder.
//
// Non-HDLC frame format:
//   [0x7E] [version=0x01] [len_lo] [len_hi] [payload...] [0x7E]
//
// Unlike HDLC:
//   - No CRC
//   - No byte-stuffing/escaping — 0x7E can appear in payload
//   - Frame boundaries determined by length, not by 0x7E delimiters
//   - The trailing 0x7E is both the end of the current frame and the start
//     of the next frame (shared delimiter)

#ifndef DIAGLOGGER_NON_HDLC_H
#define DIAGLOGGER_NON_HDLC_H

#include "diaglogger/types.h"
#include <optional>

namespace diaglogger {

// Non-HDLC framing constants
constexpr uint8_t NON_HDLC_FLAG    = 0x7E;
constexpr uint8_t NON_HDLC_VERSION = 0x01;

/// Encode a raw diag payload into a Non-HDLC frame.
/// Format: [0x7E][0x01][len_lo][len_hi][payload][0x7E]
/// Length field covers only the payload (not the header or trailing flag).
ByteBuffer non_hdlc_encode(const uint8_t* data, size_t len);
ByteBuffer non_hdlc_encode(const ByteBuffer& data);

/// Streaming Non-HDLC decoder. Feed raw serial bytes; extract complete frames.
/// Mirrors HdlcDecoder API for interchangeable use.
class NonHdlcDecoder {
public:
    NonHdlcDecoder() = default;

    /// Feed raw bytes from the serial port.
    void feed(const uint8_t* data, size_t len);
    void feed(const ByteBuffer& data);

    /// Extract the next complete decoded frame (payload only).
    /// Returns empty optional if no complete frame is available.
    std::optional<ByteBuffer> next_frame();

    /// Number of framing errors encountered so far.
    uint32_t error_count() const { return errors_; }

    /// Reset decoder state (e.g. when switching back to HDLC).
    void reset();

private:
    // Decoder state machine
    enum class State {
        WAIT_FLAG,       // Waiting for initial 0x7E
        WAIT_VERSION,    // Got flag, expecting version byte
        WAIT_LEN_LO,    // Got version, expecting length low byte
        WAIT_LEN_HI,    // Got length low, expecting length high byte
        READING_PAYLOAD, // Reading payload bytes
        WAIT_END_FLAG    // Payload complete, expecting trailing 0x7E
    };

    State state_ = State::WAIT_FLAG;
    uint8_t version_ = 0;
    uint16_t expected_len_ = 0;
    ByteBuffer accumulator_;
    std::vector<ByteBuffer> completed_frames_;
    uint32_t errors_ = 0;

    void process_byte(uint8_t byte);
};

} // namespace diaglogger

#endif // DIAGLOGGER_NON_HDLC_H
