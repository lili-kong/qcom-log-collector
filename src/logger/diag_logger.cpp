// Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
// SPDX-License-Identifier: BSD 3-Clause Clear License

#include "diag_logger.h"
#include "transport/transport_factory.h"
#include "diaglogger/version.h"
#include "diag/commands.h"
#include "diag/handshake_commands.h"
#include "diag/qdss_commands.h"
#include "diag/adpl_commands.h"

#include <iostream>
#include <iomanip>
#include <chrono>
#include <cstring>
#include <sstream>
#include <vector>
#include <algorithm>
#include <KL/Macros.h>

#ifdef TOOLS_TARGET_WINDOWS
#include <process.h>
#define getpid _getpid
#else
#include <unistd.h>
#endif

namespace diaglogger {

DiagLogger::DiagLogger(const DiagLoggerConfig& config)
    : config_(config)
{
    transport_ = create_transport(config_.port_name, config_.baud_rate);
    transport_->set_verbose(config_.verbose);
}

DiagLogger::~DiagLogger() {
    stop();
}

bool DiagLogger::do_connection() {
    // Reset framing and decoders (no-op on first connect, needed for recovery)
    framing_mode_ = FramingMode::HDLC;
    hdlc_decoder_ = HdlcDecoder();
    non_hdlc_decoder_.reset();

    // Loopback handshake
    CFLOG_INFO("do_connection: Sending loopback...", true);
    if (!do_loopback()) {
        CFLOG_ERROR("Error: Device did not respond to loopback.\n"
                  "  Possible causes:\n"
                  "  1. Another application (QUTS, Diag Tool, etc.) has the COM port open\n"
                  "     and is consuming responses. Close all other diag tools and retry.\n"
                  "  2. The device is not a Qualcomm diag port.\n"
                  "  3. The device is in a mode that does not respond to diag commands.\n", true);
        return false;
    }
    CFLOG_INFO("Device connected.", true);

    // Perform device handshake
    CFLOG_INFO("Running device handshake...", true);
    if (!do_handshake()) {
        CFLOG_WARNING("Error: Device handshake failed.", true);
        return false;
    }

    // Write .qlc metadata file (non-fatal)
    write_qlc_file();

    // Send CFG commands
    CFLOG_INFO("Sending CFG commands...", true);
    if (!send_cfg_commands()) {
        CFLOG_WARNING("Error: CFG commands failed.", true);
        return false;
    }

    return true;
}

bool DiagLogger::start() {
    logging_started_ = true;
    CLOG_INFO("Logging started. Press Ctrl+C to stop.");
    return true;
}

void DiagLogger::stop() {
    if (!running_.load()) {
        return;  // Already stopped
    }

    // Send graceful disconnect commands only if logging was started
    if (logging_started_ && transport_ && !transport_->is_disconnected()) {
        stop_logging();
        logging_started_ = false;
    }

    // Signal threads to stop
    running_.store(false);

    // Wake transport thread in case it's waiting for recovery/close signal
    {
        std::lock_guard<std::mutex> lock(transport_mutex_);
        transport_close_requested_ = true;
    }
    transport_cv_.notify_one();

    // Close files
    ptrace_.close();

    // Print summary
    CFLOG_INFO("\n--- Logging Summary ---", true);
    CFLOG_INFO("RX packets:  " + std::to_string(stats_.rx_packets), true);
    CFLOG_INFO("RX bytes:    " + std::to_string(stats_.rx_bytes), true);
    CFLOG_INFO("TX packets:  " + std::to_string(stats_.tx_packets), true);
    CFLOG_INFO("TX bytes:    " + std::to_string(stats_.tx_bytes), true);
    CFLOG_INFO("Frame errors: " + std::to_string(stats_.frame_errors), true);
    CFLOG_INFO("Ptrace file: " + ptrace_.path(), true);
    CFLOG_INFO("Records:     " + std::to_string(ptrace_.record_count()), true);
    CFLOG_INFO("File size:   " + std::to_string(ptrace_.bytes_written()) + " bytes", true);
}

void DiagLogger::transport_ready_func() {
    bool result = transport_->open();

    if (result) {
        result = do_connection();
    }

    {
        std::lock_guard<std::mutex> lock(transport_mutex_);
        transport_open_result_ = result;
        transport_open_done_ = true;
    }
    transport_cv_.notify_one();

    if (!result) {
        transport_->close();
        return;
    }

    // Wait for close or recovery request
    while (true) {
        {
            std::unique_lock<std::mutex> lock(transport_mutex_);
            transport_cv_.wait(lock, [this] {
                return transport_close_requested_ || recovery_requested_;
            });

            if (transport_close_requested_) {
                break;
            }

            if (recovery_requested_) {
                recovery_requested_ = false;
            }
        }

        // Attempt reopen with timeout
        bool recovered = false;
        auto deadline = std::chrono::steady_clock::now() +
                        std::chrono::seconds(config_.recovery_timeout_seconds);

        while (std::chrono::steady_clock::now() < deadline && running_.load()) {
            std::this_thread::sleep_for(std::chrono::seconds(2));

            if (!running_.load()) break;

            auto remaining = std::chrono::duration_cast<std::chrono::seconds>(
                deadline - std::chrono::steady_clock::now()).count();
            CFLOG_INFO("Waiting for device... (" + std::to_string(remaining) + " seconds remaining)", true);

            if (transport_->reopen()) {
                recovered = do_connection();
                break;
            }
        }

        if (!recovered) {
            CFLOG_INFO("Recovery failed after timeout.", true);
            running_.store(false);
            break;
        }
        CFLOG_INFO("Device recovered, resuming logging.", true);

        // Discard any redundant recovery signal from rx_loop
        {
            std::lock_guard<std::mutex> lock(transport_mutex_);
            recovery_requested_ = false;
        }
    }

    transport_->close();
}

bool DiagLogger::open_transport() {
    // Parse CFG file
    CFLOG_INFO("Parsing CFG file: " + config_.cfg_path, true);
    try {
        cfg_commands_ = parse_cfg_file(config_.cfg_path);
    } catch (const std::exception& e) {
        CFLOG_ERROR(std::string("Error parsing CFG file: ") + e.what(), true);
        return false;
    }

    if (cfg_commands_.empty()) {
        CFLOG_WARNING("Warning: CFG file contains no commands", true);
    }

    // Open ptrace output file before connection so handshake is recorded
    CFLOG_INFO("Writing to: " + config_.output_path, true);
    PtraceMetadata ptrace_meta;
    ptrace_meta.device_description = transport_->description();
    ptrace_meta.app_version = kToolVersion;
    if (!ptrace_.open(config_.output_path, ptrace_meta)) {
        CFLOG_ERROR("Error: Cannot create output file " + config_.output_path, true);
        return false;
    }

    running_.store(true);
    last_rx_time_ = std::chrono::steady_clock::now();
    tx_thread_ = std::thread(&DiagLogger::tx_loop, this);
    rx_thread_ = std::thread(&DiagLogger::rx_loop, this);
    dispatcher_thread_ = std::thread(&DiagLogger::dispatcher_loop, this);

    {
        std::lock_guard<std::mutex> lock(transport_mutex_);
        transport_open_done_ = false;
        transport_close_requested_ = false;
        recovery_requested_ = false;
    }
    transport_thread_ = std::thread(&DiagLogger::transport_ready_func, this);

    std::unique_lock<std::mutex> lock(transport_mutex_);
    transport_cv_.wait(lock, [this] {
        return transport_open_done_;
    });

    if (!transport_open_result_) {
        if (transport_thread_.joinable()) {
            transport_thread_.join();
        }
        running_.store(false);
        tx_cv_.notify_one();
        if (tx_thread_.joinable()) {
            tx_thread_.join();
        }
        if (rx_thread_.joinable()) {
            rx_thread_.join();
        }
        rx_queue_cv_.notify_all();
        if (dispatcher_thread_.joinable()) {
            dispatcher_thread_.join();
        }
    }

    return transport_open_result_;
}

void DiagLogger::close_transport() {
    // Signal transport thread to close
    {
        std::lock_guard<std::mutex> lock(transport_mutex_);
        transport_close_requested_ = true;
    }
    transport_cv_.notify_one();

    if (transport_thread_.joinable()) {
        transport_thread_.join();
    }

    // Wait for TX, RX, and dispatcher threads to end
    tx_cv_.notify_one();
    if (tx_thread_.joinable()) {
        tx_thread_.join();
    }

    if (rx_thread_.joinable()) {
        rx_thread_.join();
    }

    rx_queue_cv_.notify_all();
    if (dispatcher_thread_.joinable()) {
        dispatcher_thread_.join();
    }
}


DiagStats DiagLogger::stats() const {
    return stats_;
}

ByteBuffer DiagLogger::send_command(const ByteBuffer& payload, uint32_t timeout_ms) {
    if (timeout_ms == 0) {
        timeout_ms = config_.command_timeout_ms;
    }

    auto request = std::make_unique<TxRequest>();
    request->payload = payload;
    request->timeout_ms = timeout_ms;
    std::future<ByteBuffer> future = request->promise.get_future();

    // Cancel any pending poll request — commands take priority
    poll_requested_.store(false);

    {
        std::lock_guard<std::mutex> lock(tx_mutex_);
        tx_queue_.push(std::move(request));
    }
    tx_cv_.notify_one();

    return future.get();
}

// Send STATUS (0x0C) as a keep-alive poll. Routes through send_and_wait() so
// polling shares the single request/response pairing path. Called from tx_loop().
bool DiagLogger::do_polling() {
    ByteBuffer payload = build_status_request();
    verbose_log("TX poll STATUS (0x0C)");
    ByteBuffer resp = send_and_wait(payload, TX_RESPONSE_TIMEOUT_MS);
    if (resp.empty()) {
        verbose_log("Poll response timeout");
        return false;
    }
    verbose_log("Poll response received");
    return true;
}

// Encode + write one request, register its expected response signature, and wait
// for the dispatcher to pair the response. Runs on the tx thread, so it is the
// only writer to the transport. Returns the decoded response, or empty on
// timeout/shutdown. Polls running_ every 10 ms so shutdown is responsive.
ByteBuffer DiagLogger::send_and_wait(const ByteBuffer& payload, uint32_t timeout_ms) {
    // Encode using the active framing mode
    ByteBuffer frame;
    if (framing_mode_ == FramingMode::NON_HDLC) {
        frame = non_hdlc_encode(payload);
    } else {
        frame = hdlc_encode(payload);
    }

    // Record TX to ptrace + stats
    ptrace_.write_record(Direction::TX, frame);
    ++stats_.tx_packets;
    stats_.tx_bytes += payload.size();
    if (!payload.empty()) {
        verbose_log("TX cmd=0x" + ([&]{
            std::ostringstream ss;
            ss << std::hex << std::setfill('0') << std::setw(2)
               << static_cast<int>(payload[0]);
            return ss.str();
        })() + " (" + std::to_string(frame.size()) + " bytes framed, " +
            (framing_mode_ == FramingMode::NON_HDLC ? "non-hdlc" : "hdlc") + ")");
    }

    // Register the pending request before writing, so a fast response cannot
    // race ahead of registration.
    std::promise<ByteBuffer> resp_promise;
    std::future<ByteBuffer> resp_future = resp_promise.get_future();
    {
        std::lock_guard<std::mutex> lock(pending_mutex_);
        pending_.signature = compute_response_signature(payload);
        pending_.promise = &resp_promise;
    }

    // Physical write
    size_t written = transport_->write(frame.data(), frame.size());
    if (written != frame.size()) {
        CFLOG_WARNING("Warning: Incomplete write (" + std::to_string(written) + "/" + std::to_string(frame.size()) + ")", true);
    }

    // Wait for the dispatcher to fulfill the promise, or time out.
    const auto deadline = std::chrono::steady_clock::now() +
                          std::chrono::milliseconds(timeout_ms);
    while (running_.load()) {
        if (resp_future.wait_for(std::chrono::milliseconds(10)) == std::future_status::ready) {
            std::lock_guard<std::mutex> lock(pending_mutex_);
            pending_.promise = nullptr;
            pending_.signature.clear();
            return resp_future.get();
        }
        if (std::chrono::steady_clock::now() >= deadline) {
            break;
        }
    }

    // Timeout or shutdown: clear pending under lock so a late frame won't touch
    // our (about to be destroyed) promise.
    {
        std::lock_guard<std::mutex> lock(pending_mutex_);
        pending_.promise = nullptr;
        pending_.signature.clear();
    }
    if (running_.load()) {
        verbose_log("  -> Timeout waiting for response");
    } else {
        verbose_log("  -> Aborted waiting for response (shutdown)");
    }
    return {};
}

void DiagLogger::tx_loop() {
    while (running_.load()) {
        std::unique_ptr<TxRequest> request;
        {
            std::unique_lock<std::mutex> lock(tx_mutex_);
            tx_cv_.wait(lock, [this] {
                return !tx_queue_.empty() || poll_requested_.load() || !running_.load();
            });
            if (!running_.load()) {
                break;
            }
            // No queued command but a poll was requested: send the keep-alive.
            if (tx_queue_.empty() && poll_requested_.load()) {
                poll_requested_.store(false);
                lock.unlock();
                do_polling();
                continue;
            }
            if (tx_queue_.empty()) {
                continue;
            }
            request = std::move(tx_queue_.front());
            tx_queue_.pop();
        }

        // Send and block until the dispatcher pairs the response (or timeout).
        // Blocking here is the natural single-outstanding gate: the next request
        // is not dequeued until this one resolves.
        ByteBuffer response = send_and_wait(request->payload, request->timeout_ms);
        request->promise.set_value(std::move(response));
    }

    // Drain remaining requests on shutdown (fulfill with empty responses)
    std::lock_guard<std::mutex> lock(tx_mutex_);
    while (!tx_queue_.empty()) {
        auto request = std::move(tx_queue_.front());
        tx_queue_.pop();
        request->promise.set_value({});
    }
}

bool DiagLogger::do_loopback() {
    // First send a raw pre-encoded HDLC loopback to ensure the device is
    // in HDLC mode.  This matches TUF's TxWorker::onRun() (Diag.cpp:2062):
    //   HDLC_LOOPBACK_REQUEST = { 0x7B, 0x2C, 0x3D, 0x7E }
    // which is: loopback cmd (0x7B) + CRC-16 (0x2C, 0x3D) + HDLC flag (0x7E).
    // No leading 0x7E flag — the first frame omits it.
    // Sent raw via transport->write(), bypassing our hdlc_encode().
    {
        static const uint8_t HDLC_LOOPBACK_RAW[] = { 0x7B, 0x2C, 0x3D, 0x7E };
        transport_->write(HDLC_LOOPBACK_RAW, sizeof(HDLC_LOOPBACK_RAW));
        verbose_log("Sent raw HDLC loopback (4 bytes) to ensure HDLC mode");
        // Wait for device to fall back to HDLC mode (matches TUF's
        // DEVICE_HDLC_RESET_TIMER = 200ms sleep after the raw loopback).
        std::this_thread::sleep_for(std::chrono::milliseconds(200));

        // Drain any raw loopback response bytes from the ring buffer so the
        // HDLC decoder in send_command() starts with a clean slate.
        // The raw response (e.g. {7b 2c 3d 7e}) doesn't have a leading 0x7E,
        // so the HDLC decoder can't parse it into a frame — just discard it.
        uint8_t drain_buf[64];
        transport_->read(drain_buf, sizeof(drain_buf), 0);
    }

    ByteBuffer request = build_loopback_request();

    // Try up to 3 times with 2s timeout each
    for (int attempt = 0; attempt < 3; ++attempt) {
        ByteBuffer response = send_command(request);
        if (!response.empty() && response[0] == DiagCmd::LOOPBACK) {
            return true;
        }
        verbose_log("Loopback attempt " + std::to_string(attempt + 1) + " failed, retrying...");
    }
    return false;
}

bool DiagLogger::request_non_hdlc_switch() {
    // Send Non-HDLC switch request (in HDLC mode).
    // The request and response are both HDLC-framed.
    // On success, subsequent TX/RX use Non-HDLC framing.
    ByteBuffer cmd = build_non_hdlc_switch_request();
    ByteBuffer resp = send_command(cmd);

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

    // Switch both TX and RX to Non-HDLC framing
    framing_mode_ = FramingMode::NON_HDLC;
    return true;
}

bool DiagLogger::do_handshake() {
    // Follows QUTS connect() sequence (Diag.cpp:8526-8538).

    // 1. VERNO request (device version info for .qlc metadata, non-fatal)
    {
        ByteBuffer cmd = build_verno_request();
        ByteBuffer resp = send_command(cmd);
        if (!resp.empty() && !is_error_response(resp)) {
            handshake_state_.device_info.verno = parse_verno_response(resp);
            verbose_log("Handshake: VERNO response received (" +
                        std::to_string(resp.size()) + " bytes)");
        } else {
            verbose_log("Handshake: VERNO — not supported (continuing)");
        }
    }

    // 2. STATUS request
    // A BAD_CMD (0x13) response is acceptable — it still confirms the device is alive.
    {
        ByteBuffer cmd = build_status_request();
        ByteBuffer resp = send_command(cmd);
        if (resp.empty()) {
            verbose_log("Handshake: STATUS request — timeout");
            return false;
        } else if (resp[0] == DiagError::BAD_CMD) {
            verbose_log("Handshake: STATUS request — BAD_CMD (device alive)");
        } else if (is_error_response(resp)) {
            verbose_log("Handshake: STATUS request — error response");
            return false;
        } else {
            handshake_state_.device_info.status = parse_status_response(resp);
            verbose_log("Handshake: STATUS response received (" +
                        std::to_string(resp.size()) + " bytes)");
        }
    }

    // 3. EXT_BUILD_ID request (informational only — does not affect handshake result)
    {
        ByteBuffer cmd = build_ext_build_id_request();
        ByteBuffer resp = send_command(cmd);
        if (resp.empty()) {
            verbose_log("Handshake: EXT_BUILD_ID request — timeout (continuing)");
        } else if (is_error_response(resp)) {
            verbose_log("Handshake: EXT_BUILD_ID request — error response (continuing)");
        } else {
            handshake_state_.device_info.ext_build_id = parse_ext_build_id_response(resp);
            verbose_log("Handshake: EXT_BUILD_ID response received (" +
                        std::to_string(resp.size()) + " bytes)");
        }
    }

    // 4. ESN request (Electronic Serial Number for .qlc metadata, non-fatal)
    {
        ByteBuffer cmd = build_esn_request();
        ByteBuffer resp = send_command(cmd);
        if (!resp.empty() && !is_error_response(resp)) {
            handshake_state_.device_info.esn = parse_esn_response(resp);
            verbose_log("Handshake: ESN response received (" +
                        std::to_string(resp.size()) + " bytes)");
        } else {
            verbose_log("Handshake: ESN — not supported (continuing)");
        }
    }

    // 5. Non-HDLC switch (sent in HDLC, response in HDLC, then switch)
    //    Skipped when --hdlc is set, matching TUF's enableHdlcOnly behavior
    //    (Diag::requestSwitchToNonHdlc returns early when HDLC_ONLY is enabled).
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

    // 6. Security key request
    {
        ByteBuffer cmd = build_security_key_request();
        ByteBuffer resp = send_command(cmd);
        if (resp.empty()) {
            verbose_log("Handshake: Security key request — timeout");
            return false;
        } else if (is_error_response(resp)) {
            verbose_log("Handshake: Security key request — error response");
        } else {
            auto payload = extract_subsystem_payload(resp);
            handshake_state_.security_keys = parse_security_key_response(payload);
            if (handshake_state_.security_keys.ok()) {
                verbose_log("Handshake: Security keys loaded successfully");
            } else {
                verbose_log("Handshake: Security key request failed (status=" +
                            std::to_string(handshake_state_.security_keys.status) + ")");
            }
        }
    }

    // 7. Feature query → parse capabilities
    {
        ByteBuffer cmd = build_feature_query();
        ByteBuffer resp = send_command(cmd);
        if (resp.empty()) {
            verbose_log("Handshake: Feature query — timeout");
            return false;
        } else if (is_error_response(resp)) {
            verbose_log("Handshake: Feature query — error response");
            return false;
        } else {
            auto payload = extract_subsystem_payload(resp);
            handshake_state_.feature_query = parse_feature_query_response(payload);
            if (handshake_state_.feature_query.valid) {
                verbose_log("Handshake: Feature query — mask=0x" + ([&]{
                    std::ostringstream ss;
                    ss << std::hex << handshake_state_.feature_query.feature_mask;
                    return ss.str();
                })() + " hypervisor=" +
                    (handshake_state_.feature_query.supports_hypervisor() ? "yes" : "no") +
                    " diagid=" +
                    (handshake_state_.feature_query.supports_diagid() ? "yes" : "no"));
            } else {
                verbose_log("Handshake: Feature query — invalid response");
                return false;
            }
        }
    }

    // 8. If hypervisor supported: enable features, set DiagID priority, query table
    if (handshake_state_.feature_query.valid &&
        handshake_state_.feature_query.supports_hypervisor()) {

        // Enable hypervisor features (CMD + ASYNC)
        {
            ByteBuffer cmd = build_diagid_feature_enable(HypervisorPacketFormat::BOTH);
            ByteBuffer resp = send_command(cmd);
            if (resp.empty()) {
                verbose_log("Handshake: Hypervisor enable — no valid response");
                return false;
            } else if (is_error_response(resp)) {
               verbose_log("Handshake: Hypervisor enable — error response");
            }
            auto payload = extract_subsystem_payload(resp);
            auto result = parse_hypervisor_enable_response(payload);
            if (result.ok()) {
                verbose_log("Handshake: Hypervisor features enabled");
            } else {
                verbose_log("Handshake: Hypervisor enable failed (status=" +
                            std::to_string(result.status) + ")");
            }
        }

        // Set DiagID priority (SET, diag_id=1 for apps processor)
        {
            ByteBuffer cmd = build_diagid_priority_config(
                DiagIdPriorityOp::SET, handshake_state_.default_diag_id);
            ByteBuffer resp = send_command(cmd);
            if (resp.empty()) {
                verbose_log("Handshake: DiagID priority config — no valid response");
                return false;
            }
            else if (is_error_response(resp)) {
               verbose_log("Handshake: DiagID priority config — error response");
            }
            auto payload = extract_subsystem_payload(resp);
            auto result = parse_diagid_priority_response(payload);
            if (result.ok()) {
                verbose_log("Handshake: DiagID priority set (id=" +
                            std::to_string(handshake_state_.default_diag_id) + ")");
            } else {
                verbose_log("Handshake: DiagID priority config failed (status=" +
                            std::to_string(result.status) + ")");
            }
        }

        // Query DiagID table
        {
            ByteBuffer cmd = build_diagid_table_query();
            ByteBuffer resp = send_command(cmd);
            if (resp.empty()) {
                verbose_log("Handshake: DiagID table query — no valid response");
                return false;
            }
            else if (is_error_response(resp)) {
               verbose_log("Handshake: DiagID table config — error response");
            }
            auto payload = extract_subsystem_payload(resp);
            handshake_state_.diagid_table = parse_diagid_table_response(payload);
            if (handshake_state_.diagid_table.valid) {
                verbose_log("Handshake: DiagID table — " +
                            std::to_string(handshake_state_.diagid_table.entries.size()) + " entries");
                for (const auto& entry : handshake_state_.diagid_table.entries) {
                    verbose_log("  DiagID " + std::to_string(entry.diag_id) +
                                " = " + entry.processor_name);
                }
            } else {
                verbose_log("Handshake: DiagID table — invalid response");
            }
        }
    }

    // 8b. QShrink4 GUID query
    query_qshrink_guids();

    // TODO: Phase 2
    // // 9. QDSS setup (if enabled)
    // if (config_.enable_qdss) {
    //     if (!setup_qdss()) {
    //         return false;
    //     }
    // }

    // 10. ADPL setup (if enabled)
    if (config_.enable_adpl) {
        if (!setup_adpl()) {
            return false;
        }
    }

    verbose_log("Handshake complete.");
    return true;
}

bool DiagLogger::setup_qdss() {
    verbose_log("QDSS: Starting setup...");

    // 1. Query HW acceleration capabilities
    {
        ByteBuffer cmd = build_qdss_hw_accel_query();
        ByteBuffer resp = send_command(cmd);
        if (resp.empty() || is_error_response(resp)) {
            verbose_log("QDSS: HW accel query — failed");
            return false;
        }
        auto payload = extract_subsystem_payload(resp);
        handshake_state_.qdss_hw_accel = parse_qdss_hw_accel_response(payload);
        verbose_log("QDSS: HW accel query — STM=" +
                    std::string(handshake_state_.qdss_hw_accel.stm_supported ? "yes" : "no") +
                    " ATB=" +
                    std::string(handshake_state_.qdss_hw_accel.atb_supported ? "yes" : "no"));
    }

    // 2. Enable HW acceleration for STM if supported
    if (handshake_state_.qdss_hw_accel.stm_supported) {
        ByteBuffer cmd = build_qdss_hw_accel_enable(
            QdssHwAccelType::STM, handshake_state_.qdss_hw_accel.stm_diagid_mask);
        ByteBuffer resp = send_command(cmd);
        if (resp.empty() || is_error_response(resp)) {
            verbose_log("QDSS: HW accel STM enable — failed");
            return false;
        }
        verbose_log("QDSS: HW accel STM enabled");
    }

    // 3. Enable Diag-over-STM
    {
        ByteBuffer cmd = build_qdss_stm_enable(config_.qdss_processor_mask);
        ByteBuffer resp = send_command(cmd);
        if (resp.empty() || is_error_response(resp)) {
            verbose_log("QDSS: Diag-over-STM enable — failed");
            return false;
        }
        verbose_log("QDSS: Diag-over-STM enabled (mask=0x" + ([&]{
            std::ostringstream ss;
            ss << std::hex << static_cast<int>(config_.qdss_processor_mask);
            return ss.str();
        })() + ")");
    }

    // 4. Set trace sink to USB
    {
        ByteBuffer cmd = build_qdss_set_trace_sink(QdssSinkType::USB);
        ByteBuffer resp = send_command(cmd);
        if (resp.empty() || is_error_response(resp)) {
            verbose_log("QDSS: Set trace sink — failed");
            return false;
        }
        verbose_log("QDSS: Trace sink set to USB");
    }

    // 5. Enable SW trace filters per processor
    uint8_t mask = config_.qdss_processor_mask;
    uint8_t processors[] = {
        QdssProcessorMask::MODEM, QdssProcessorMask::ADSP,
        QdssProcessorMask::WCNSS, QdssProcessorMask::APSS,
        QdssProcessorMask::SLPI,  QdssProcessorMask::CDSP
    };
    for (uint8_t proc : processors) {
        if (mask & proc) {
            ByteBuffer cmd = build_qdss_filter_sw_trace(proc, true);
            if (!cmd.empty()) {
                ByteBuffer resp = send_command(cmd, 1000);
                if (resp.empty() || is_error_response(resp)) {
                    verbose_log("QDSS: SW trace filter failed for processor 0x" + ([&]{
                        std::ostringstream ss;
                        ss << std::hex << static_cast<int>(proc);
                        return ss.str();
                    })());
                    return false;
                }
                verbose_log("QDSS: SW trace filter enabled for processor 0x" + ([&]{
                    std::ostringstream ss;
                    ss << std::hex << static_cast<int>(proc);
                    return ss.str();
                })());
            }
        }
    }

    verbose_log("QDSS: Setup complete.");
    return true;
}

bool DiagLogger::setup_adpl() {
    verbose_log("ADPL: Starting setup (version=" +
                std::to_string(config_.adpl_version) + ")...");

    ByteBuffer cmd = build_adpl_enable(config_.adpl_version);
    ByteBuffer resp = send_command(cmd);
    if (resp.empty()) {
        verbose_log("ADPL: Enable — no response");
        return false;
    }

    auto payload = extract_subsystem_payload(resp);
    auto result = parse_adpl_enable_response(payload);
    if (!result.ok()) {
        verbose_log("ADPL: Enable failed (status=" +
                    std::to_string(result.status) + ")");
        return false;
    }

    handshake_state_.adpl_negotiated_version = result.version;
    verbose_log("ADPL: Enabled (negotiated version=" +
                std::to_string(result.version) + ")");
    return true;
}

bool DiagLogger::stop_logging() {
    CFLOG_INFO("Sending disconnect commands...", true);
    bool all_ok = true;

    // Matches TUF Diag::disconnect() teardown sequence (Diag.cpp:8600-8658).

    // 1. Disable ADPL if it was enabled
    if (config_.enable_adpl && handshake_state_.adpl_negotiated_version > 0) {
        ByteBuffer cmd = build_adpl_disable();
        ByteBuffer resp = send_command(cmd, 1000);
        all_ok &= (!resp.empty() && !is_error_response(resp));
        verbose_log("Disconnect: ADPL disable — " +
                    std::string(resp.empty() ? "no response" : "done"));
    }

    // 2. Disable QDSS if it was enabled
    if (config_.enable_qdss) {
        // Disable Diag-over-STM
        {
            ByteBuffer cmd = build_qdss_stm_disable();
            ByteBuffer resp = send_command(cmd, 1000);
            all_ok &= (!resp.empty() && !is_error_response(resp));
            verbose_log("Disconnect: QDSS STM disable — " +
                        std::string(resp.empty() ? "no response" : "done"));
        }

        // Disable all HW acceleration types at once
        {
            ByteBuffer cmd = build_qdss_hw_accel_disable(QdssHwAccelType::ALL);
            ByteBuffer resp = send_command(cmd, 1000);
            all_ok &= (!resp.empty() && !is_error_response(resp));
            verbose_log("Disconnect: QDSS HW accel disable — " +
                        std::string(resp.empty() ? "no response" : "done"));
        }
    }

    // 3. Clear all masks (matches TUF sendMaskConfigurationCommands with empty masks)

    // 3a. Disable log masks (all equipment IDs 0-15)
    {
        auto disable_cmds = build_log_config_disable_all();
        for (const auto& cmd : disable_cmds) {
            ByteBuffer resp = send_command(cmd, 500);
            all_ok &= (!resp.empty() && !is_error_response(resp));
        }
        verbose_log("Disconnect: Log masks disabled");
    }

    // 3b. Disable event reporting + clear event mask
    {
        ByteBuffer cmd = build_event_report(false);
        ByteBuffer resp = send_command(cmd, 1000);
        all_ok &= (!resp.empty() && !is_error_response(resp));
        verbose_log("Disconnect: Event reporting disabled");
    }
    {
        ByteBuffer cmd = build_set_event_mask_disable();
        ByteBuffer resp = send_command(cmd, 1000);
        all_ok &= (!resp.empty() && !is_error_response(resp));
        verbose_log("Disconnect: Event mask cleared");
    }

    // 3c. Disable debug messages (EXT_MSG_CFG)
    {
        ByteBuffer cmd = build_ext_msg_cfg_disable();
        ByteBuffer resp = send_command(cmd, 1000);
        all_ok &= (!resp.empty() && !is_error_response(resp));
        verbose_log("Disconnect: Debug message mask disabled");
    }

    // 3d. Disable QTrace
    {
        ByteBuffer cmd = build_qtrace_disable();
        ByteBuffer resp = send_command(cmd, 1000);
        all_ok &= (!resp.empty() && !is_error_response(resp));
        verbose_log("Disconnect: QTrace disabled");
    }

    // 3e. Disable QshMetrics
    {
        ByteBuffer cmd = build_qshmetrics_disable();
        ByteBuffer resp = send_command(cmd, 1000);
        all_ok &= (!resp.empty() && !is_error_response(resp));
        verbose_log("Disconnect: QshMetrics disabled");
    }

    // 4. STATUS poll to flush — ensures device acknowledges all mask changes
    {
        ByteBuffer cmd = build_status_request();
        ByteBuffer resp = send_command(cmd, 1000);
        all_ok &= (!resp.empty() && !is_error_response(resp));
        verbose_log("Disconnect: STATUS flush poll sent");
    }

    CFLOG_INFO("Disconnect commands sent.", true);
    return all_ok;
}
    
bool DiagLogger::query_qshrink_guids() {
    verbose_log("QShrink4: Querying GUID tables...");

    const auto& peripherals = get_qshrink_peripherals();
    for (const auto& periph : peripherals) {
        bool got_result = false;

        // Try protocol versions 3, 2, 1
        for (uint16_t version = 3; version >= 1 && !got_result; --version) {
            static constexpr int MAX_EFS_RETRIES = 3;
            for (int retry = 0; retry < MAX_EFS_RETRIES; ++retry) {
                ByteBuffer cmd = build_qshrink_file_list(periph.subcmd, version);
                ByteBuffer resp = send_command(cmd);

                if (resp.empty()) {
                    verbose_log("QShrink4: " + std::string(periph.name) +
                                " v" + std::to_string(version) + " — no response");
                    break;  // no response, try next version
                }

                auto payload = extract_subsystem_payload(resp);
                auto result = parse_qshrink_file_list(payload);

                if (!result.valid) {
                    verbose_log("QShrink4: " + std::string(periph.name) +
                                " v" + std::to_string(version) + " — invalid response");
                    break;  // invalid, try next version
                }

                // EFS busy/retry errors (status 6 or 8)
                if (result.status == 6 || result.status == 8) {
                    verbose_log("QShrink4: " + std::string(periph.name) +
                                " v" + std::to_string(version) +
                                " — EFS error (status=" + std::to_string(result.status) +
                                "), retry " + std::to_string(retry + 1));
                    continue;  // retry same version
                }

                if (result.ok() && !result.files.empty()) {
                    for (const auto& file : result.files) {
                        handshake_state_.qshrink_guids.push_back(file);
                    }
                    // Populate GUID in matching diagid_table entry
                    for (auto& de : handshake_state_.diagid_table.entries) {
                        if (de.processor_name == periph.name) {
                            de.guid = result.files[0].guid;
                            break;
                        }
                    }
                    verbose_log("QShrink4: " + std::string(periph.name) +
                                " v" + std::to_string(version) + " — " +
                                std::to_string(result.files.size()) + " file(s)");
                    got_result = true;
                } else {
                    verbose_log("QShrink4: " + std::string(periph.name) +
                                " v" + std::to_string(version) +
                                " — status=" + std::to_string(result.status) +
                                ", " + std::to_string(result.files.size()) + " files");
                    break;  // non-retryable status, try next version
                }
                break;  // got a valid response, stop retrying
            }
        }
    }

    verbose_log("QShrink4: Total GUIDs collected: " +
                std::to_string(handshake_state_.qshrink_guids.size()));
    return !handshake_state_.qshrink_guids.empty();
}

bool DiagLogger::send_cfg_commands() {
    for (const auto& cmd : cfg_commands_) {
        if (cmd.empty()) continue;
        ByteBuffer resp = send_command(cmd);
        if (resp.empty()) {
            return false;
        }
    }
    return true;
}

void DiagLogger::rx_loop() {
    static constexpr size_t RX_LOOP_BUFFER_SIZE = 128 * 1024;  // Match USB read size
    std::vector<uint8_t> buf(RX_LOOP_BUFFER_SIZE);
    auto start_time = std::chrono::steady_clock::now();

    while (running_.load()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));

        // Check duration limit
        if (config_.duration_seconds > 0) {
            auto elapsed = std::chrono::steady_clock::now() - start_time;
            auto elapsed_s = std::chrono::duration_cast<std::chrono::seconds>(elapsed).count();
            if (elapsed_s >= config_.duration_seconds) {
                CFLOG_INFO("\nDuration limit reached (" + std::to_string(config_.duration_seconds) + "s)", true);
                running_.store(false);
                break;
            }
        }

        // Read from ring buffer (filled by rx_worker from USB)
        size_t n = transport_->read(buf.data(), buf.size(), 100);
        if (n > 0) {
            std::lock_guard<std::mutex> rx_lock(last_rx_mutex_);
            last_rx_time_ = std::chrono::steady_clock::now();
        }
        if (n == 0) {
            // Check if RX has been idle long enough to request a poll
            if (!poll_requested_.load()) {
                std::chrono::steady_clock::time_point rx_time;
                {
                    std::lock_guard<std::mutex> rx_lock(last_rx_mutex_);
                    rx_time = last_rx_time_;
                }
                auto idle_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                    std::chrono::steady_clock::now() - rx_time).count();
                if (idle_ms >= POLL_IDLE_TIMEOUT_MS) {
                    verbose_log("No RX for " + std::to_string(idle_ms)
                                + " ms — requesting poll");
                    poll_requested_.store(true);
                    tx_cv_.notify_one();
                }
            }

            // Check if device disconnected — signal transport thread once
            if (transport_->is_disconnected()) {
                if (config_.enable_recovery) {
                    std::lock_guard<std::mutex> lock(transport_mutex_);
                    if (!recovery_requested_) {
                        CFLOG_WARNING("\nDevice disconnected, signalling recovery...", true);
                        recovery_requested_ = true;
                        transport_cv_.notify_one();
                    }
                } else {
                    CFLOG_ERROR("\nDevice disconnected (recovery disabled).", true);
                    running_.store(false);
                    break;
                }
            }
            continue;
        }

        // Write raw wire-format bytes to ptrace BEFORE decoding.
        // PortTraceReader::storeDiagCommand() does HDLC/NonHDLC decoding internally.
        ptrace_.write_record(Direction::RX, buf.data(), n);

        // Feed to the active decoder
        if (framing_mode_ == FramingMode::NON_HDLC) {
            non_hdlc_decoder_.feed(buf.data(), n);
        } else {
            hdlc_decoder_.feed(buf.data(), n);
        }

        // Push every complete frame onto the response queue for the dispatcher.
        // rx_loop is the sole producer; dispatcher_loop is the sole consumer.
        {
            std::lock_guard<std::mutex> qlock(rx_queue_mutex_);
            while (true) {
                std::optional<ByteBuffer> frame;
                if (framing_mode_ == FramingMode::NON_HDLC) {
                    frame = non_hdlc_decoder_.next_frame();
                } else {
                    frame = hdlc_decoder_.next_frame();
                }
                if (!frame) break;
                rx_queue_.push(std::move(*frame));
            }
        }
        rx_queue_cv_.notify_one();

        // Update error counts
        stats_.frame_errors = hdlc_decoder_.crc_error_count() +
                              non_hdlc_decoder_.error_count();
    }
}

// Consume rx_queue_: pair sync responses to the outstanding request, route the
// rest as async packets. Async frames are counted here (rx_packets/rx_bytes);
// paired responses are returned to the waiting send_and_wait() instead.
void DiagLogger::dispatcher_loop() {
    while (true) {
        ByteBuffer frame;
        {
            std::unique_lock<std::mutex> lock(rx_queue_mutex_);
            rx_queue_cv_.wait(lock, [this] {
                return !rx_queue_.empty() || !running_.load();
            });
            if (rx_queue_.empty()) {
                if (!running_.load()) break;
                continue;
            }
            frame = std::move(rx_queue_.front());
            rx_queue_.pop();
        }

        // Try to pair with the single outstanding request.
        // Also matches error/NAK responses (BAD_CMD, BAD_PARM, etc.) which have
        // a different first byte but carry the original cmd code at frame[1].
        // Both valid and error responses are returned to the caller as-is;
        // the caller decides how to interpret them. Only timeout returns empty.
        bool paired = false;
        {
            std::lock_guard<std::mutex> lock(pending_mutex_);
            if (pending_.promise != nullptr &&
                (response_matches_signature(pending_.signature, frame) ||
                 error_response_matches_signature(pending_.signature, frame))) {
                pending_.promise->set_value(frame);
                pending_.promise = nullptr;
                pending_.signature.clear();
                paired = true;
            }
        }
        if (paired) {
            verbose_log("  -> RX paired response");
            continue;
        }

        // Async packet (log / event / message / qtrace / qmetric). Raw bytes are
        // already in ptrace (written by rx_loop); here we only update stats.
        ++stats_.rx_packets;
        stats_.rx_bytes += frame.size();
        if (config_.verbose && !frame.empty()) {
            std::ostringstream ss;
            ss << "  RX async: cmd=0x" << std::hex << std::setfill('0') << std::setw(2)
               << static_cast<int>(frame[0])
               << " len=" << std::dec << frame.size();
            CFLOG_INFO(ss.str(), true);
        }
    }
}



void DiagLogger::write_qlc_file() {
    // Generate timestamped filename matching ptrace naming convention
    auto now = std::chrono::system_clock::now();
    auto time_t_now = std::chrono::system_clock::to_time_t(now);
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        now.time_since_epoch()) % 1000;
    std::tm tm_now{};
#ifdef _WIN32
    localtime_s(&tm_now, &time_t_now);
#else
    localtime_r(&time_t_now, &tm_now);
#endif
    char filename[128];
    snprintf(filename, sizeof(filename),
             "qlc_%02d_%02d_%04d_%02d_%02d_%02d_%03d__PID_%d.qlc",
             tm_now.tm_mday, tm_now.tm_mon + 1, tm_now.tm_year + 1900,
             tm_now.tm_hour, tm_now.tm_min, tm_now.tm_sec,
             static_cast<int>(ms.count()), static_cast<int>(getpid()));

    std::string qlc_path = config_.output_path;
    if (!qlc_path.empty() && qlc_path.back() != '/' && qlc_path.back() != '\\') {
        qlc_path += '/';
    }
    qlc_path += filename;

    QlcMetadata meta;
    meta.hdlc_data_type = (framing_mode_ == FramingMode::HDLC) ? 1 : 0;
    meta.qshrink_guids = handshake_state_.qshrink_guids;
    meta.diagid_table = handshake_state_.diagid_table;
    meta.security_keys = handshake_state_.security_keys;
    meta.qdss_hw_accel = handshake_state_.qdss_hw_accel;
    meta.device_info = handshake_state_.device_info;

    if (QlcWriter::write(qlc_path, meta)) {
        verbose_log("QLC metadata written: " + qlc_path);
    } else {
        CFLOG_WARNING("Failed to write QLC metadata file: " + qlc_path, true);
    }
}

void DiagLogger::verbose_log(const std::string& msg) {
    if (config_.verbose) {
        CFLOG_DEBUG("[verbose] " + msg, true);
    }
}

} // namespace diaglogger
