// Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
// SPDX-License-Identifier: BSD 3-Clause Clear License
//
// Diag: diag-protocol layer. Layers HDLC / non-HDLC framing, the
// connect-time handshake, and paired request/response over a raw-byte
// Transport owned by the Protocol base. Business logic (MBN, xQCN, etc.)
// uses this class via send_and_wait().

#ifndef QC_PROTOCOL_DIAG_H
#define QC_PROTOCOL_DIAG_H

#include "types.h"
#include "protocol/protocol.h"
#include "hdlc/hdlc.h"
#include "hdlc/non_hdlc.h"
#include "file_writer/ptrace_writer.h"

#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace QC {

/// Active framing mode for serial communication.
enum class FramingMode {
    HDLC,      // Standard HDLC (0x7E delimiters, CRC-16, byte-stuffing)
    NON_HDLC   // Length-prefixed (0x7E + version + length + payload + 0x7E, no CRC)
};

struct DiagConfig {
    std::string     port_name;
    uint32_t        baud_rate = 115200;
    uint32_t        command_timeout_ms = 2000;
    bool            verbose = false;
    bool            hdlc_only = false;
    bool            port_trace = false;
    std::string     port_trace_path;
    PtraceMetadata  port_trace_metadata;   // optional; description filled in if empty
};

class Diag : public Protocol {
public:
    /// Build with the default QcDevice transport for config.port_name.
    explicit Diag(const DiagConfig& config);

    /// Build over a caller-supplied transport (tests, alternate links).
    Diag(const DiagConfig& config, std::unique_ptr<Transport> transport);

    ~Diag() override;

    // Non-copyable
    Diag(const Diag&) = delete;
    Diag& operator=(const Diag&) = delete;

    /// Encode a diag payload with the active framing, send it, and wait for
    /// its paired response. timeout_ms=0 uses the configured default.
    /// Returns empty on timeout / transport-not-running.
    ByteBuffer send_and_wait(const ByteBuffer& payload,
                             uint32_t timeout_ms = 0) override;

    /// Called with every decoded frame that did NOT pair with an outstanding
    /// request — log/event/message packets, and protocol-specific "delayed"
    /// responses that arrive unpaired (e.g. QShrink4 FILE_READ data).
    ///
    /// Without a handler such frames are counted and dropped. Install one
    /// BEFORE request_open() so no frame can arrive ahead of registration.
    ///
    /// Invoked on the receiver thread — keep it fast and non-blocking, and
    /// do not call back into send_and_wait() from it (the receiver thread is
    /// what fulfills the response this would wait on, so that self-deadlocks).
    using AsyncFrameHandler = std::function<void(const ByteBuffer&)>;
    void set_async_frame_handler(AsyncFrameHandler handler);

    /// Framing negotiated with the device. HDLC until a Non-HDLC switch is
    /// accepted during the handshake. Recorded in .qlc metadata.
    FramingMode framing_mode() const { return framing_mode_; }

    /// Total framing errors seen by whichever decoder is active: HDLC CRC
    /// failures plus Non-HDLC resync events.
    uint32_t frame_error_count() const {
        return hdlc_decoder_.crc_error_count() + non_hdlc_decoder_.error_count();
    }

    /// Wait for the protocol to reach STATE_AVAILABLE, polling up to
    /// timeout. Throws std::runtime_error if the timeout elapses without
    /// the protocol becoming available. Call before a command when a
    /// mid-sequence link loss (STATE_UNRESPONSIVE) should abort the whole
    /// operation rather than be absorbed per-command — a brief drop is
    /// given a chance to recover before failing.
    void validate_state_ready(std::chrono::milliseconds timeout = std::chrono::seconds(5)) const;


protected:
    // Opens the port + runs the connection handshake.
    bool lifecycle_open() override;

    // Pulls raw bytes from the transport, unframes them, then pairs sync
    // responses and routes async packets.
    void receiver_loop() override;

private:
    DiagConfig        config_;
    FramingMode       framing_mode_ = FramingMode::HDLC;
    HdlcDecoder       hdlc_decoder_;
    NonHdlcDecoder    non_hdlc_decoder_;

    // Invoked from handle_frame() for every unpaired frame. Set once before
    // request_open() and not changed while the receiver thread runs, so no
    // lock is needed to read it.
    AsyncFrameHandler async_frame_handler_;

    static constexpr uint32_t RX_POLL_TIMEOUT_MS = 100;

    // Route a decoded frame: pair or treat as async.
    void handle_frame(const ByteBuffer& frame);

    // Handshake sub-steps.
    bool do_connection();
    bool do_loopback();
    bool do_handshake();
    bool request_non_hdlc_switch();

    void verbose_log(const std::string& msg);
};

} // namespace QC

#endif // QC_PROTOCOL_DIAG_H
