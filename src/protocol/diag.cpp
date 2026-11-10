// Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
// SPDX-License-Identifier: BSD 3-Clause Clear License
//
// QcDevice USB transport wrapping the QDS QcDevice API for device
// discovery, open, read, write, and close.
//

#include "diag.h"

#include "version.h"
#include "diag/commands.h"
#include "diag/handshake_commands.h"
#include "diag/response_pairing.h"
#include "transport/transport_factory.h"

#include <chrono>
#include <iomanip>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <thread>
#include <utility>
#include <vector>
#include <KL/Macros.h>

namespace QC {

Diag::Diag(const DiagConfig& config)
    : Diag(config, create_transport(config.port_name, config.baud_rate))
{
}

Diag::Diag(const DiagConfig& config, std::unique_ptr<Transport> transport)
    : Protocol(std::move(transport))
    , config_(config)
{
    set_verbose(config_.verbose);
    if (config_.port_trace) {
        PtraceMetadata meta = config_.port_trace_metadata;
        if (meta.device_description.empty()) {
            meta.device_description = description();
        }
        if (meta.app_version.empty()) {
            meta.app_version = kToolVersion;
        }
        set_port_trace_metadata(meta);
        set_port_trace(true, config_.port_trace_path);
    }
}

Diag::~Diag() {
    // Drive shutdown through the lifecycle worker while our vtable is
    // still valid. See Protocol::stop().
    stop();
}

bool Diag::lifecycle_open() {
    // Base opens the trace file (if enabled), opens the port, and starts
    // TX/RX + receiver/dispatcher threads. It marks STATE_AVAILABLE on an
    // open port, but for diag the handshake must pass first — so drop back
    // to STATE_INITIALIZING until do_connection() succeeds.
    if (!Protocol::lifecycle_open()) {
        return false;
    }
    set_state(STATE_INITIALIZING);

    if (!do_connection() || open_cancelled()) {
        // The port is up but the device never answered — distinguish that
        // from an absent device so callers can tell "unplugged" from
        // "plugged in but not talking".
        if (!open_cancelled()) {
            set_state(STATE_UNRESPONSIVE);
        }
        lifecycle_close();
        return false;
    }

    set_state(STATE_AVAILABLE);
    return true;
}

bool Diag::do_connection() {
    if (open_cancelled()) {
        return false;
    }
    framing_mode_ = FramingMode::HDLC;
    hdlc_decoder_ = HdlcDecoder();
    non_hdlc_decoder_.reset();

    CFLOG_INFO("do_connection: Sending loopback...", true);
    if (!do_loopback()) {
        if (open_cancelled()) {
            return false;
        }
        CFLOG_ERROR("Error: Device did not respond to loopback.\n"
                  "  Possible causes:\n"
                  "  1. Another application (QUTS, Diag Tool, etc.) has the COM port open\n"
                  "     and is consuming responses. Close all other diag tools and retry.\n"
                  "  2. The device is not a Qualcomm diag port.\n"
                  "  3. The device is in a mode that does not respond to diag commands.\n", true);
        return false;
    }
    if (open_cancelled()) {
        return false;
    }
    CFLOG_INFO("Device connected.", true);

    CFLOG_INFO("Running device handshake...", true);
    if (!do_handshake()) {
        if (!open_cancelled()) {
            CFLOG_WARNING("Error: Device handshake failed.", true);
        }
        return false;
    }

    return true;
}

// Only one request is outstanding at a time (single pending_ slot).
// Encodes the payload with the active framing mode, then delegates the
// pending-slot mechanics to the base.
ByteBuffer Diag::send_and_wait(const ByteBuffer& payload, uint32_t timeout_ms) {
    if (timeout_ms == 0) {
        timeout_ms = config_.command_timeout_ms;
    }

    ByteBuffer frame;
    if (framing_mode_ == FramingMode::NON_HDLC) {
        frame = non_hdlc_encode(payload);
    } else {
        frame = hdlc_encode(payload);
    }

    if (!payload.empty()) {
        verbose_log("TX cmd=0x" + ([&]{
            std::ostringstream ss;
            ss << std::hex << std::setfill('0') << std::setw(2)
               << static_cast<int>(payload[0]);
            return ss.str();
        })() + " (" + std::to_string(frame.size()) + " bytes framed, " +
            (framing_mode_ == FramingMode::NON_HDLC ? "non-hdlc" : "hdlc") + ")");
    }

    ByteBuffer signature = compute_response_signature(payload);
    ByteBuffer resp = send_and_wait_frame(
        std::move(frame),
        std::move(signature),
        [](const ByteBuffer& sig, const ByteBuffer& reply) {
            return response_matches_signature(sig, reply)
                || error_response_matches_signature(sig, reply);
        },
        std::chrono::milliseconds(timeout_ms));

    if (resp.empty()) {
        if (io_running_.load() && !open_cancelled()) {
            verbose_log("  -> Timeout waiting for response");
            // Link is still up but the device stopped answering. Only
            // downgrade from AVAILABLE: during the connect handshake we are
            // INITIALIZING and a timeout there is handled by lifecycle_open().
            if (state() == STATE_AVAILABLE) {
                set_state(STATE_UNRESPONSIVE);
            }
        } else {
            verbose_log("  -> Aborted waiting for response (shutdown or TX rejected)");
        }
    } else if (state() == STATE_UNRESPONSIVE) {
        // Device started answering again.
        set_state(STATE_AVAILABLE);
    }
    return resp;
}

void Diag::set_async_frame_handler(AsyncFrameHandler handler) {
    async_frame_handler_ = std::move(handler);
}

void Diag::validate_state_ready(std::chrono::milliseconds timeout) const {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    ProtocolState current = state();
    while (current != STATE_AVAILABLE) {
        if (!io_running_.load() || std::chrono::steady_clock::now() >= deadline) {
            throw std::runtime_error(
                "Protocol did not reach AVAILABLE within "
                + std::to_string(timeout.count()) + "ms, unable to send commands. State: "
                + state_to_string(current));
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
        current = state();
    }
}

bool Diag::do_loopback() {
    {
        // Nudge the device into HDLC mode. Sent as raw bytes — already a
        // complete HDLC frame, so it bypasses the encoder. Any reply is
        // absorbed by receiver_loop() and discarded as an unpaired frame,
        // since no request is pending yet.
        static const uint8_t HDLC_LOOPBACK_RAW[] = { 0x7B, 0x2C, 0x3D, 0x7E };
        if (!submit_tx(
                ByteBuffer(HDLC_LOOPBACK_RAW, HDLC_LOOPBACK_RAW + sizeof(HDLC_LOOPBACK_RAW)))) {
            return false;
        }
        verbose_log("Sent raw HDLC loopback (4 bytes) to ensure HDLC mode");
        if (wait_for_open_cancel(std::chrono::milliseconds(200))) {
            return false;
        }
    }

    ByteBuffer request = build_loopback_request();

    static constexpr int MAX_LOOPBACK_ATTEMPTS = 3;
    for (int attempt = 0; attempt < MAX_LOOPBACK_ATTEMPTS; ++attempt) {
        if (open_cancelled()) {
            return false;
        }
        ByteBuffer response = send_and_wait(request);
        if (open_cancelled()) {
            return false;
        }
        if (!response.empty() && response[0] == DiagCmd::LOOPBACK) {
            return true;
        }
        verbose_log("Loopback attempt " + std::to_string(attempt + 1) + " failed, retrying...");
    }
    return false;
}

bool Diag::request_non_hdlc_switch() {
    ByteBuffer cmd = build_non_hdlc_switch_request();
    ByteBuffer resp = send_and_wait(cmd);

    if (resp.empty()) {
        return false;
    }

    auto payload = extract_subsystem_payload(resp);
    auto result = parse_non_hdlc_switch_response(payload);

    if (!result.ok()) {
        verbose_log("Non-HDLC switch rejected (version=" +
                    std::to_string(result.framing_version) +
                    " result=" + std::to_string(result.result_code) + ")");
        return false;
    }

    framing_mode_ = FramingMode::NON_HDLC;
    return true;
}

bool Diag::do_handshake() {
    if (open_cancelled()) {
        return false;
    }
    // STATUS request
    {
        ByteBuffer cmd = build_status_request();
        ByteBuffer resp = send_and_wait(cmd);
        if (resp.empty()) {
            verbose_log("Handshake: STATUS request — timeout");
            return false;
        } else if (resp[0] == DiagError::BAD_CMD) {
            verbose_log("Handshake: STATUS request — BAD_CMD (device alive)");
        } else if (is_error_response(resp)) {
            verbose_log("Handshake: STATUS request — error response");
            return false;
        } else {
            verbose_log("Handshake: STATUS response received (" +
                        std::to_string(resp.size()) + " bytes)");
        }
    }

    // Non-HDLC switch
    if (open_cancelled()) {
        return false;
    }
    if (config_.hdlc_only) {
        verbose_log("Handshake: HDLC-only mode enabled, NOT switching to Non-HDLC");
    } else {
        bool switched = request_non_hdlc_switch();
        if (switched) {
            verbose_log("Handshake: Switched to Non-HDLC framing");
        } else {
            verbose_log("Handshake: Non-HDLC switch not available, staying in HDLC");
        }
    }

    if (open_cancelled()) {
        return false;
    }
    verbose_log("Handshake complete.");
    return true;
}

void Diag::receiver_loop() {
    while (true) {
        ByteBuffer raw = next_rx(RX_POLL_TIMEOUT_MS);
        if (raw.empty()) {
            // Only exit once the queue is drained: base has stopped the RX
            // producer (io_running_ cleared) AND next_rx() returned nothing.
            if (!io_running_.load()) {
                break;
            }
            continue;
        }

        if (framing_mode_ == FramingMode::NON_HDLC) {
            non_hdlc_decoder_.feed(raw.data(), raw.size());
        } else {
            hdlc_decoder_.feed(raw.data(), raw.size());
        }

        while (true) {
            std::optional<ByteBuffer> frame;
            if (framing_mode_ == FramingMode::NON_HDLC) {
                frame = non_hdlc_decoder_.next_frame();
            } else {
                frame = hdlc_decoder_.next_frame();
            }
            if (!frame) {
                break;
            }
            handle_frame(*frame);
        }
    }
}

void Diag::handle_frame(const ByteBuffer& frame) {
    bool matched = try_fulfill_pending(
        frame,
        [](const ByteBuffer& sig, const ByteBuffer& reply) {
            return response_matches_signature(sig, reply)
                || error_response_matches_signature(sig, reply);
        });
    if (matched) {
        verbose_log("  -> RX paired response");
        return;
    }

    if (config_.verbose && !frame.empty()) {
        std::ostringstream ss;
        ss << "  RX async: cmd=0x" << std::hex << std::setfill('0') << std::setw(2)
           << static_cast<int>(frame[0])
           << " len=" << std::dec << frame.size();
        CFLOG_INFO(ss.str(), true);
    }

    // Hand the unpaired frame to the owner, if one registered interest. This
    // is how log/event/message packets and "delayed" unpaired responses (e.g.
    // QShrink4 FILE_READ data) reach a consumer; without a handler they are
    // dropped here.
    if (async_frame_handler_) {
        async_frame_handler_(frame);
    }
}

void Diag::verbose_log(const std::string& msg) {
    if (config_.verbose) {
        CFLOG_DEBUG("[verbose] " + msg, true);
    }
}

} // namespace QC
