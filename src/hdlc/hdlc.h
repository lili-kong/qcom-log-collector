// Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
// SPDX-License-Identifier: BSD 3-Clause Clear License
//
// HDLC encoder and streaming decoder for Qualcomm diag protocol.

#ifndef DIAGLOGGER_HDLC_H
#define DIAGLOGGER_HDLC_H

#include "diaglogger/types.h"
#include <optional>

namespace diaglogger {

// HDLC framing constants
constexpr uint8_t HDLC_FLAG   = 0x7E;
constexpr uint8_t HDLC_ESCAPE = 0x7D;
constexpr uint8_t HDLC_ESCAPE_MASK = 0x20;

/// Encode a raw diag payload into an HDLC frame.
/// Appends CRC-16, escapes special bytes, wraps in 0x7E delimiters.
ByteBuffer hdlc_encode(const uint8_t* data, size_t len);
ByteBuffer hdlc_encode(const ByteBuffer& data);

/// Streaming HDLC decoder. Feed raw serial bytes; extract complete frames.
class HdlcDecoder {
public:
    HdlcDecoder() = default;

    /// Feed raw bytes from the serial port.
    void feed(const uint8_t* data, size_t len);
    void feed(const ByteBuffer& data);

    /// Extract the next complete decoded frame (CRC-stripped payload).
    /// Returns empty optional if no complete frame is available.
    std::optional<ByteBuffer> next_frame();

    /// Number of CRC errors encountered so far.
    uint32_t crc_error_count() const { return crc_errors_; }

private:
    ByteBuffer accumulator_;
    std::vector<ByteBuffer> completed_frames_;
    bool escape_next_ = false;
    uint32_t crc_errors_ = 0;

    void process_byte(uint8_t byte);
    void finalize_frame();
};

} // namespace diaglogger

#endif // DIAGLOGGER_HDLC_H
