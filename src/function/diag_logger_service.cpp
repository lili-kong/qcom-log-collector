// Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
// SPDX-License-Identifier: BSD 3-Clause Clear License

#include <algorithm>
#include <chrono>
#include <cstring>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <vector>
#include <algorithm>
#include <fstream>

#include <KL/Macros.h>

#ifdef TOOLS_TARGET_WINDOWS
#include <process.h>
#define getpid _getpid
#else
#include <unistd.h>
#endif

#include "diag_logger_service.h"
#include "protocol/adpl/adpl.h"
#include "version.h"
#include "diag/commands.h"
#include "diag/handshake_commands.h"
#include "diag/qdss_commands.h"
#include "diag/adpl_commands.h"

namespace QC {

DiagLoggerService::DiagLoggerService(const DiagLoggerConfig& config)
    : config_(config) {
    // Translate our config into the protocol layer's. Diag creates the
    // transport itself and owns the port trace, so the .diag capture lands in
    // output_path without this class touching a writer.
    DiagConfig diag_config;
    diag_config.port_name          = config_.port_name;
    diag_config.baud_rate          = config_.baud_rate;
    diag_config.command_timeout_ms = config_.command_timeout_ms;
    diag_config.verbose            = config_.verbose;
    diag_config.hdlc_only          = config_.hdlc_only;
    diag_config.port_trace         = true;
    diag_config.port_trace_path    = config_.output_path;
    diag_config.port_trace_metadata.app_version = kToolVersion;

    diag_ = std::make_unique<Diag>(diag_config);

    // Parse the CFG and install the async-frame sink now, so the caller can
    // open the port the moment it has the service. Failure is recorded rather
    // than thrown; start() reports it.
    prepared_ = prepare();
}

DiagLoggerService::~DiagLoggerService() {
    stop();
    // Join our own workers before diag_ dies: config_restore_loop() and
    // poll_loop() both call into it. stop() already does this, but repeat it
    // so a caller that never called stop() cannot leave a thread running
    // against a destroyed Diag.
    join_service_threads();
}

// Everything that does not need an open port. Runs from the constructor so the
// caller can drive the protocol lifecycle (request_open/wait_ready) as soon as
// it holds the service.
bool DiagLoggerService::prepare() {
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

    // ADPL needs the DPL log codes enabled in the log mask, or the accelerated
    // request succeeds but no packets stream. Merge them into the parsed masks
    // once here so both the initial send and any recovery re-send carry them.
    if (config_.enable_adpl) {
        enable_adpl_log_codes();
    }

    CFLOG_INFO("Writing to: " + config_.output_path, true);

    // Register the async-frame sink before the port can open, so no inbound
    // frame arrives ahead of it. Diag pairs sync responses itself; log/event
    // packets and QShrink4's unpaired "delayed" responses come to us.
    diag()->set_async_frame_handler(
        [this](const ByteBuffer& frame) { on_async_frame(frame); });

    return true;
}

bool DiagLoggerService::start() {
    if (!prepared_) {
        CFLOG_ERROR("Error: CFG parsing failed; cannot start logging.", true);
        return false;
    }

    // The caller owns opening the port. Confirm Diag's handshake actually
    // passed before sending anything: a port can be open while the device is
    // still silent (STATE_UNRESPONSIVE) or mid-handshake (STATE_INITIALIZING).
    if (!diag_) {
        return false;
    }
    try {
        validate_diag_state(HANDSHAKE_TIMEOUT);
    } catch (const std::runtime_error& e) {
        CFLOG_ERROR(std::string("Error: Diag protocol not ready: ") + e.what(), true);
        return false;
    }

    running_.store(true);
    {
        std::lock_guard<std::mutex> lock(last_rx_mutex_);
        last_rx_time_ = std::chrono::steady_clock::now();
    }

    if (!post_open_setup()) {
        CFLOG_ERROR("Error: Device setup failed.", true);
        running_.store(false);
        return false;
    }

    // Watchdog + keep-alive run for the whole session, across recoveries.
    restore_thread_ = std::thread(&DiagLoggerService::config_restore_loop, this);
    poll_thread_ = std::thread(&DiagLoggerService::poll_loop, this);

    logging_started_ = true;
    CLOG_INFO("Logging started. Press Ctrl+C to stop.");
    // Download QShrink4 databases in the background - never blocks logging.
    qshrink_thread_ = std::thread(&DiagLoggerService::download_qshrink_files, this);
    return true;
}

void DiagLoggerService::stop() {
    // Run exactly once. NOTE: do NOT gate this on running_ — the recovery
    // thread clears running_ itself on an unrecoverable disconnect, so gating
    // here would skip the graceful teardown (QDSS/ADPL disable, mask
    // clearing) on that exit.
    if (stop_invoked_.exchange(true)) {
        return;
    }

    // Send graceful disconnect commands only if logging was started and the
    // port is still usable. Done before running_ is cleared, since
    // send_command() refuses to send once it is false.
    if (logging_started_ && diag_ && !diag_->is_disconnected()) {
        stop_logging();
        logging_started_ = false;
    }

    // Signal our threads to stop, then join them. Joining here (rather than in
    // a separate close step) means that once stop() returns, nothing of ours is
    // still touching the protocol — so the caller is free to close the port.
    running_.store(false);
    restore_cv_.notify_all();
    poll_cv_.notify_all();
    qshrink_rx_cv_.notify_all();

    join_service_threads();

    // Stop QDSS manager if running
    if (qdss_) {
        qdss()->stop();
    }

    // Stop ADPL manager if running
    if (adpl_) {
        adpl()->stop();
    }

    print_session_summary();
}

// Final accounting, printed once from stop().
void DiagLoggerService::print_session_summary() {
    DiagStats s = stats();
    CFLOG_INFO("\n--- Logging Summary ---", true);
    CFLOG_INFO("RX packets:  " + std::to_string(s.rx_packets), true);
    CFLOG_INFO("RX bytes:    " + std::to_string(s.rx_bytes), true);
    CFLOG_INFO("TX packets:  " + std::to_string(s.tx_packets), true);
    CFLOG_INFO("TX bytes:    " + std::to_string(s.tx_bytes), true);
    CFLOG_INFO("Frame errors: " + std::to_string(s.frame_errors), true);

    // Diag capture (.diag). Prefer the actual file path from kLogger; fall back
    // to the port trace path if kLogger hasn't opened a file yet.
    std::string diag_file = KL::Logger::get_instance().get_current_ptrace_file();
    if (diag_file.empty() && diag_) {
        diag_file = diag_->ptrace_path();
    }
    CFLOG_INFO("Diag file:   " + diag_file, true);
    if (diag_) {
        CFLOG_INFO("Diag records:" + std::to_string(diag_->ptrace_record_count()), true);
        CFLOG_INFO("Diag bytes:  " + std::to_string(diag_->ptrace_bytes_written()), true);
    }

    // QDSS capture (.bin), if enabled.
    if (qdss_) {
        CFLOG_INFO("QDSS file:   " + qdss()->output_file(), true);
        CFLOG_INFO("QDSS bytes:  " + std::to_string(qdss()->bytes_captured()), true);
    }

    // ADPL capture (.adplvN), if enabled.
    if (adpl_) {
        CFLOG_INFO("ADPL file:   " + adpl()->output_file(), true);
        CFLOG_INFO("ADPL bytes:  " + std::to_string(adpl()->bytes_captured()), true);
    }
}

// Join every thread this class owns. Safe to call more than once; each join is
// guarded by joinable(). Must run before diag_ is destroyed.
void DiagLoggerService::join_service_threads() {
    // The QShrink4 worker first: running_ is already false, so its in-flight
    // send_command() returns empty and its wait loops exit.
    if (qshrink_thread_.joinable()) {
        qshrink_thread_.join();
    }
    if (poll_thread_.joinable()) {
        poll_thread_.join();
    }
    if (restore_thread_.joinable()) {
        restore_thread_.join();
    }
}

const std::unique_ptr<Protocol>& DiagLoggerService::get_protocol(ProtocolType type) const {
    switch (type) {
        case PROT_DIAG: return diag_;
        case PROT_QDSS: return qdss_;
        case PROT_ADPL: return adpl_;
        default:        break;
    }
    // Unsupported type: hand back a null handle rather than throwing, so the
    // caller's usual "test before dereference" check covers it. Function-local
    // static so the reference stays valid, and const so nothing can fill it.
    static const std::unique_ptr<Protocol> none;
    return none;
}

ProtocolState DiagLoggerService::get_protocol_state(ProtocolType type) const {
    // Reuse the lookup above so there is one place that maps a type to a
    // handle. A null handle (absent protocol) reads as DISCONNECTED, which is
    // also Protocol's own starting state.
    const std::unique_ptr<Protocol>& protocol = get_protocol(type);
    return protocol ? protocol->state() : STATE_DISCONNECTED;
}

void DiagLoggerService::validate_diag_state(std::chrono::milliseconds timeout) const {
    if (!diag_) {
        throw std::runtime_error("Diag protocol is not available.");
    }
    diag()->validate_state_ready(timeout);
}

DiagStats DiagLoggerService::stats() const {
    DiagStats s;
    // Frame-level RX counts come from our async handler; TX and the error
    // count come from the protocol layer below.
    s.rx_packets = async_rx_packets_.load(std::memory_order_relaxed);
    s.rx_bytes   = async_rx_bytes_.load(std::memory_order_relaxed);
    if (diag_) {
        s.tx_packets   = diag_->tx_packets();
        s.tx_bytes     = diag_->tx_bytes();
        s.frame_errors = diag()->frame_error_count();
    }
    return s;
}

// Called on Diag's receiver thread for every frame that did not pair with an
// outstanding request. Keep it fast: never send from here (the thread that
// would deliver the response is this one).
void DiagLoggerService::on_async_frame(const ByteBuffer& frame) {
    {
        std::lock_guard<std::mutex> lock(last_rx_mutex_);
        last_rx_time_ = std::chrono::steady_clock::now();
    }

    async_rx_packets_.fetch_add(1, std::memory_order_relaxed);
    async_rx_bytes_.fetch_add(frame.size(), std::memory_order_relaxed);

    // QShrink4 FILE_READ delayed responses arrive unpaired as SUBSYS_V2
    // (0x80) DIAG_SERV (0x12) frames. Route them to the download thread.
    if (qshrink_capturing_.load() && frame.size() >= 12 &&
        frame[0] == DiagCmd::SUBSYS_V2 && frame[1] == SubsysId::DIAG_SERV) {
        {
            std::lock_guard<std::mutex> lock(qshrink_rx_mutex_);
            qshrink_rx_queue_.push(frame);
        }
        qshrink_rx_cv_.notify_one();
        return;
    }

    // Any other async packet (log / event / message / qtrace / qmetric). Raw
    // bytes are already in the port trace, written by the protocol's rx loop;
    // the counters above are all we add.
}

ByteBuffer DiagLoggerService::send_command(const ByteBuffer& payload, uint32_t timeout_ms) {
    // Never send once shutdown has begun. This keeps background callers (e.g.
    // the QShrink4 download thread) from blocking on a response that will
    // never come, so stop()'s join completes.
    if (!running_.load() || !diag_) {
        return {};
    }
    // timeout_ms == 0 lets Diag apply config_.command_timeout_ms, which we
    // seeded from DiagLoggerConfig in the constructor.
    return diag()->send_and_wait(payload, timeout_ms);
}

// --- Configuration restore after a reconnect --------------------------------

// The caller owns the DIAG port: it reacts to device arrival/departure events
// and drives request_open()/request_close(). What it cannot do from outside
// is re-apply the device configuration a reboot wiped — the CFG log masks,
// QDSS and ADPL enables, and the DiagID setup all live in device RAM and are
// lost with the device. This watcher notices the port coming back AVAILABLE
// and re-runs post_open_setup() so logging resumes configured instead of silent.
void DiagLoggerService::config_restore_loop() {
    // post_open_setup() already ran for the first connection, so start from
    // "configured" and only act on a later reconnect.
    bool port_lost = false;

    while (running_.load()) {
        {
            std::unique_lock<std::mutex> lock(restore_mutex_);
            restore_cv_.wait_for(lock, RESTORE_POLL_INTERVAL,
                                 [this] { return !running_.load(); });
        }
        if (!running_.load()) {
            break;
        }
        if (!diag_) {
            continue;
        }

        // Track the PORT, not the protocol state. is_available() is also false
        // for STATE_UNRESPONSIVE — a device that stopped answering while the
        // port is still open — and that is a timeout the keep-alive handles,
        // not a reconnect. Reconfiguring on it would re-send every mask for a
        // device that never rebooted.
        if (!diag_->is_open()) {
            if (!port_lost) {
                CFLOG_WARNING("\nDiag port closed; waiting for it to be reopened...", true);
                port_lost = true;

                // Recovery disabled: end the session rather than waiting for
                // the port to return. stop() is not gated on running_, so the
                // graceful teardown still runs — it skips the device commands
                // because the port is gone.
                if (!config_.enable_recovery) {
                    CFLOG_ERROR("Device disconnected (recovery disabled).", true);
                    running_.store(false);
                    break;
                }
            }
            continue;
        }

        if (!port_lost) {
            continue;   // steady state, nothing to do
        }

        // The port is back. Wait for Diag's handshake to finish before sending:
        // lifecycle_open() re-runs loopback + STATUS + the Non-HDLC switch on
        // every open, and until it passes the state is INITIALIZING.
        if (!diag_->is_available()) {
            continue;
        }

        CFLOG_INFO("Diag port is back; restoring device configuration...", true);
        if (post_open_setup()) {
            CFLOG_INFO("Device configuration restored, resuming logging.", true);
            port_lost = false;
            {
                std::lock_guard<std::mutex> lock(last_rx_mutex_);
                last_rx_time_ = std::chrono::steady_clock::now();
            }
        } else {
            // Leave port_lost set so the next tick retries. The port is open
            // but unconfigured, so there is nothing to lose by trying again; a
            // device that keeps refusing simply keeps failing here until the
            // caller closes the port or the session ends.
            CFLOG_WARNING("Failed to restore device configuration; will retry.", true);
        }
    }
}


// --- Idle keep-alive --------------------------------------------------------

// Ask for STATUS when nothing has arrived for a while, to notice a link that
// went quiet without dropping. Diag serializes this against real commands, so
// a queued command is never displaced by a poll.
void DiagLoggerService::poll_loop() {
    const auto tick = std::chrono::milliseconds(250);

    while (running_.load()) {
        {
            std::unique_lock<std::mutex> lock(poll_mutex_);
            poll_cv_.wait_for(lock, tick, [this] { return !running_.load(); });
        }
        if (!running_.load()) {
            break;
        }

        // Only poll a healthy link; during recovery the port is being
        // reopened and a poll would be noise.
        if (!diag_ || !diag_->is_available()) {
            continue;
        }

        std::chrono::steady_clock::time_point rx_time;
        {
            std::lock_guard<std::mutex> lock(last_rx_mutex_);
            rx_time = last_rx_time_;
        }
        auto idle_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - rx_time).count();
        if (idle_ms < POLL_IDLE_TIMEOUT_MS) {
            continue;
        }

        verbose_log("No RX for " + std::to_string(idle_ms) + " ms - polling STATUS");
        ByteBuffer resp = send_command(build_status_request(), TX_RESPONSE_TIMEOUT_MS);
        if (resp.empty()) {
            verbose_log("Poll response timeout");
        } else {
            verbose_log("Poll response received");
        }

        // Reset the idle clock either way: on success the device answered, and
        // on failure this throttles polling to one attempt per idle window
        // instead of spinning.
        {
            std::lock_guard<std::mutex> lock(last_rx_mutex_);
            last_rx_time_ = std::chrono::steady_clock::now();
        }
    }
}


// Device setup after Diag's handshake (loopback, STATUS, Non-HDLC switch) has
// passed and the port is AVAILABLE. Re-runs on reboot recovery; the one-shot
// guards keep session-level work (the .qlc file, the QShrink4 GUID query, the
// QDSS/ADPL capture files) from repeating.
bool DiagLoggerService::post_open_setup() {
    // 1. VERNO request (device version info for .qlc metadata, non-fatal)
    {
        ByteBuffer cmd = build_verno_request();
        ByteBuffer resp = send_command(cmd);
        if (!resp.empty() && !is_error_response(resp)) {
            handshake_state_.device_info.verno = parse_verno_response(resp);
            verbose_log("Setup: VERNO response received (" +
                        std::to_string(resp.size()) + " bytes)");
        } else {
            verbose_log("Setup: VERNO - not supported (continuing)");
        }
    }

    // 2. STATUS request.
    // Diag already sent STATUS during its handshake to prove the device is
    // alive, but it discards the payload — re-send it here because the .qlc
    // metadata needs the parsed DMSS status. A BAD_CMD (0x13) response is
    // acceptable: it still confirms the device is answering.
    {
        ByteBuffer cmd = build_status_request();
        ByteBuffer resp = send_command(cmd);
        if (resp.empty()) {
            verbose_log("Setup: STATUS request - timeout");
            return false;
        } else if (resp[0] == DiagError::BAD_CMD) {
            verbose_log("Setup: STATUS request - BAD_CMD (device alive)");
        } else if (is_error_response(resp)) {
            verbose_log("Setup: STATUS request - error response");
            return false;
        } else {
            handshake_state_.device_info.status = parse_status_response(resp);
            verbose_log("Setup: STATUS response received (" +
                        std::to_string(resp.size()) + " bytes)");
        }
    }

    // 3. EXT_BUILD_ID request (informational only - does not affect the result)
    {
        ByteBuffer cmd = build_ext_build_id_request();
        ByteBuffer resp = send_command(cmd);
        if (resp.empty()) {
            verbose_log("Setup: EXT_BUILD_ID request - timeout (continuing)");
        } else if (is_error_response(resp)) {
            verbose_log("Setup: EXT_BUILD_ID request - error response (continuing)");
        } else {
            handshake_state_.device_info.ext_build_id = parse_ext_build_id_response(resp);
            verbose_log("Setup: EXT_BUILD_ID response received (" +
                        std::to_string(resp.size()) + " bytes)");
        }
    }

    // 4. ESN request (Electronic Serial Number for .qlc metadata, non-fatal)
    {
        ByteBuffer cmd = build_esn_request();
        ByteBuffer resp = send_command(cmd);
        if (!resp.empty() && !is_error_response(resp)) {
            handshake_state_.device_info.esn = parse_esn_response(resp);
            verbose_log("Setup: ESN response received (" +
                        std::to_string(resp.size()) + " bytes)");
        } else {
            verbose_log("Setup: ESN - not supported (continuing)");
        }
    }

    // The Non-HDLC framing switch happened inside Diag's handshake, before we
    // got here — everything from this point on is sent with the negotiated
    // framing, and framing_mode() reports which one for the .qlc metadata.

    // 5. Security key request
    {
        ByteBuffer cmd = build_security_key_request();
        ByteBuffer resp = send_command(cmd);
        if (resp.empty()) {
            verbose_log("Setup: Security key request - timeout");
            return false;
        } else if (is_error_response(resp)) {
            verbose_log("Setup: Security key request - error response");
        } else {
            auto payload = extract_subsystem_payload(resp);
            handshake_state_.security_keys = parse_security_key_response(payload);
            if (handshake_state_.security_keys.ok()) {
                verbose_log("Setup: Security keys loaded successfully");
            } else {
                verbose_log("Setup: Security key request failed (status=" +
                            std::to_string(handshake_state_.security_keys.status) + ")");
            }
        }
    }

    // 6. Feature query → parse capabilities
    {
        ByteBuffer cmd = build_feature_query();
        ByteBuffer resp = send_command(cmd);
        if (resp.empty()) {
            verbose_log("Setup: Feature query - timeout");
            return false;
        } else if (is_error_response(resp)) {
            verbose_log("Setup: Feature query - error response");
            return false;
        } else {
            auto payload = extract_subsystem_payload(resp);
            handshake_state_.feature_query = parse_feature_query_response(payload);
            if (handshake_state_.feature_query.valid) {
                verbose_log("Setup: Feature query - mask=0x" + ([&]{
                    std::ostringstream ss;
                    ss << std::hex << handshake_state_.feature_query.feature_mask;
                    return ss.str();
                })() + " hypervisor=" +
                    (handshake_state_.feature_query.supports_hypervisor() ? "yes" : "no") +
                    " diagid=" +
                    (handshake_state_.feature_query.supports_diagid() ? "yes" : "no"));
            } else {
                verbose_log("Setup: Feature query - invalid response");
                return false;
            }
        }
    }

    // 7. If hypervisor supported: enable features, set DiagID priority, query table
    if (handshake_state_.feature_query.valid &&
        handshake_state_.feature_query.supports_hypervisor()) {

        // Enable hypervisor features (CMD + ASYNC)
        {
            ByteBuffer cmd = build_diagid_feature_enable(HypervisorPacketFormat::BOTH);
            ByteBuffer resp = send_command(cmd);
            if (resp.empty()) {
                verbose_log("Setup: Hypervisor enable - no valid response");
                return false;
            } else if (is_error_response(resp)) {
               verbose_log("Setup: Hypervisor enable - error response");
            }
            auto payload = extract_subsystem_payload(resp);
            auto result = parse_hypervisor_enable_response(payload);
            if (result.ok()) {
                verbose_log("Setup: Hypervisor features enabled");
            } else {
                verbose_log("Setup: Hypervisor enable failed (status=" +
                            std::to_string(result.status) + ")");
            }
        }

        // Set DiagID priority (SET, diag_id=1 for apps processor)
        {
            ByteBuffer cmd = build_diagid_priority_config(
                DiagIdPriorityOp::SET, handshake_state_.default_diag_id);
            ByteBuffer resp = send_command(cmd);
            if (resp.empty()) {
                verbose_log("Setup: DiagID priority config - no valid response");
                return false;
            }
            else if (is_error_response(resp)) {
               verbose_log("Setup: DiagID priority config - error response");
            }
            auto payload = extract_subsystem_payload(resp);
            auto result = parse_diagid_priority_response(payload);
            if (result.ok()) {
                verbose_log("Setup: DiagID priority set (id=" +
                            std::to_string(handshake_state_.default_diag_id) + ")");
            } else {
                verbose_log("Setup: DiagID priority config failed (status=" +
                            std::to_string(result.status) + ")");
            }
        }

        // Query DiagID table
        {
            ByteBuffer cmd = build_diagid_table_query();
            ByteBuffer resp = send_command(cmd);
            if (resp.empty()) {
                verbose_log("Setup: DiagID table query - no valid response");
                return false;
            }
            else if (is_error_response(resp)) {
               verbose_log("Setup: DiagID table config - error response");
            }
            auto payload = extract_subsystem_payload(resp);
            handshake_state_.diagid_table = parse_diagid_table_response(payload);
            if (handshake_state_.diagid_table.valid) {
                verbose_log("Setup: DiagID table - " +
                            std::to_string(handshake_state_.diagid_table.entries.size()) + " entries");
                for (const auto& entry : handshake_state_.diagid_table.entries) {
                    verbose_log("  DiagID " + std::to_string(entry.diag_id) +
                                " = " + entry.processor_name);
                }
            } else {
                verbose_log("Setup: DiagID table - invalid response");
            }
        }
    }

    // 8. QShrink4 GUID query — once per session only. post_open_setup re-runs on reboot
    // recovery, but the download thread (launched once in start()) iterates
    // handshake_state_.qshrink_guids; re-querying here would modify that vector
    // concurrently (data race) and append duplicates. The DBs are already downloaded
    // (skip-if-exists covers re-runs), so a single query on first connect suffices.
    if (!qshrink_queried_) {
        query_qshrink_guids();
        qshrink_queried_ = true;
    }

    // 9. QDSS setup (if enabled)
    if (config_.enable_qdss) {
        if (!setup_qdss()) {
            verbose_log("Setup: QDSS setup failed.");
            return false;
        }
    }

    // 10. ADPL setup (if enabled)
    //
    // Open the ADPL trace port and start capturing BEFORE sending the enable
    // command. The device begins streaming as soon as ADPL is enabled, so the
    // receiver must already be running or the initial packets are lost. The
    // Adpl instance and its .adplvN file are created once per session. The
    // public DiagLogger API exposes reopening only for the DIAG port.
    if (config_.enable_adpl) {
        if (!setup_adpl()) {
            verbose_log("Setup: ADPL setup failed");
            return false;
        }
    }

    verbose_log("Device setup complete.");
    return true;
}

// ----------------------------------------------------------------------------
// setup_qdss
//
// Sequence:
//   1. Query HW acceleration capabilities
//   2. Set trace sink to USB
//   3. Enable ATB (highest priority)
//   4. Enable LPASS STM (processors not covered by ATB)
//   5. Enable STM (remaining processors)
//   6. Enable SW trace filters per processor
// ----------------------------------------------------------------------------
bool DiagLoggerService::setup_qdss() {
    verbose_log("QDSS: Starting setup...");

   // First connection: open the QDSS trace port and start capturing BEFORE
   // sending the enable commands. The device begins streaming (sync/config
   // + buffered trace) immediately once QDSS is enabled, so the receiver
   // must already be running or that initial data is lost. Matches TUF,
   // where the QDSS port is open before configureDiagOverQdss() runs
   // (Device/Protocol/Qdss.cpp).
   //
   // The Qdss instance (and its .bin file) is created ONCE per session.
   // The public DiagLogger API reopens only DIAG; setup_qdss() still re-runs
   // to reconfigure the rebooted device.
   //
   // NOTE: on that recovery path the enable commands below are sent without
   // reopening the \TRACE port through the public API. A device reboot can
   // therefore drop early trace data until the capture port is available.
    if (!qdss_) {
       QdssConfig qdss_config;
       qdss_config.port_name = config_.qdss_port_name;
       qdss_config.output_path = config_.output_path;
       qdss_config.baud_rate = config_.baud_rate;
       qdss_ = std::make_unique<Qdss>(qdss_config);
       if (!qdss()->start()) {
          CFLOG_ERROR("Error: Failed to open QDSS port.", true);
          qdss_.reset();
          return false;
       }
    }

    // Mark the control plane as touched before configuring, so stop_logging()
    // still tears down whatever a partially-successful configure left enabled.
    qdss_diag_setup_attempted_ = true;

    if (!configure_diag_over_qdss()) {
        CFLOG_ERROR("Error: QDSS configuration failed.", true);
        return false;
    }

    // Store HW accel info for .qlc metadata file
    handshake_state_.qdss_hw_accel = hw_accel_;

    verbose_log("QDSS: Setup complete.");
    return true;
}

// QDSS configuration functions are in src/function/diag_logger_qdss_manager.cpp

// ----------------------------------------------------------------------------
// setup_adpl
//
// Sequence:
//   1. First connection: create Adpl, open its \DPL transport + .adplvN file,
//      and start the RX thread BEFORE sending the enable command.
//   2. Send the ADPL enable subsystem command (IPA/ACCELERATED_IPA_REQUEST).
//   3. Store the negotiated protocol version for stop_logging()'s disable path.
//
// Reopening the \DPL port after a device loss is not exposed through the
// public DiagLogger request_open() API.
// ----------------------------------------------------------------------------
bool DiagLoggerService::setup_adpl() {
    if (!config_.enable_adpl) {
        return false;
    }

    verbose_log("ADPL: Starting setup (requested version=" +
                std::to_string(config_.adpl_version) + ")...");

    auto hex_dump = [](const ByteBuffer& b) {
        std::ostringstream ss;
        for (size_t i = 0; i < b.size() && i < 40; ++i)
            ss << ' ' << std::hex << std::setw(2) << std::setfill('0') << static_cast<int>(b[i]);
        ss << " (len=" << std::dec << b.size() << ")";
        return ss.str();
    };

    // 1. Enable ADPL over the diag port FIRST, and read the NEGOTIATED version.
    // The device may downgrade (e.g. request v7 -> negotiate v6). The capture file's
    // .adplvN extension must reflect the negotiated version, so it is opened only
    // after this. No ADPL data flows on \DPL until the enable succeeds, so opening
    // the capture afterwards loses nothing.
    ByteBuffer cmd = build_adpl_enable(config_.adpl_version);
    CFLOG_INFO("ADPL enable req:" + hex_dump(cmd), true);   // TEMP DIAGNOSTIC
    ByteBuffer resp = send_command(cmd);
    CFLOG_INFO("ADPL enable resp:" + hex_dump(resp), true); // TEMP DIAGNOSTIC
    if (resp.empty()) {
        verbose_log("ADPL: Enable - no response");
        return false;
    }

    auto payload = extract_subsystem_payload(resp);
    auto result = parse_adpl_enable_response(payload);
    if (!result.ok()) {
        CFLOG_INFO("ADPL: Enable failed (status=" +
                   std::to_string(result.status) + ")", true);   // TEMP DIAGNOSTIC
        return false;
    }

    handshake_state_.adpl_negotiated_version = result.version;
    CFLOG_INFO("ADPL: Enabled (negotiated version=" +
               std::to_string(result.version) + ")", true);   // TEMP DIAGNOSTIC

    // 2. Open the \DPL capture with the NEGOTIATED version (so the file extension
    // matches the actual wire format). Created once per session; the public
    // DiagLogger request_open() API does not reopen this capture port.
    if (!adpl_) {
        AdplConfig adpl_config;
        adpl_config.port_name    = config_.adpl_port_name;
        adpl_config.output_path  = config_.output_path;
        adpl_config.baud_rate    = config_.baud_rate;
        adpl_config.adpl_version = result.version;
        adpl_ = std::make_unique<Adpl>(adpl_config);
        if (!adpl()->start()) {
            CFLOG_ERROR("Error: Failed to open ADPL port.", true);
            adpl_.reset();
            return false;
        }
    }

    return true;
}

bool DiagLoggerService::stop_logging() {
    CFLOG_INFO("Sending disconnect commands...", true);
    bool all_ok = true;

    // 1. Disable ADPL if it was enabled
    if (config_.enable_adpl && handshake_state_.adpl_negotiated_version > 0) {
        ByteBuffer cmd = build_adpl_disable();
        ByteBuffer resp = send_command(cmd, 1000);
        all_ok &= (!resp.empty() && !is_error_response(resp));
        verbose_log("Disconnect: ADPL disable - " +
                    std::string(resp.empty() ? "no response" : "done"));
    }

    // 2. Disable QDSS if it was enabled
    if (config_.enable_qdss && qdss_diag_setup_attempted_) {
        disable_diag_over_qdss();
    }

    // 3. Clear all masks

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

    // 4. STATUS poll to flush - ensures device acknowledges all mask changes
    {
        ByteBuffer cmd = build_status_request();
        ByteBuffer resp = send_command(cmd, 1000);
        all_ok &= (!resp.empty() && !is_error_response(resp));
        verbose_log("Disconnect: STATUS flush poll sent");
    }

    CFLOG_INFO("Disconnect commands sent.", true);
    return all_ok;
}
    
bool DiagLoggerService::query_qshrink_guids() {
    verbose_log("QShrink4: Querying GUID tables...");

    // Dump up to 80 bytes so a few FILE_LIST entries (16B hdr + 20B/entry) are
    // captured - lets us confirm the per-version entry layout on a device.
    auto hex_dump = [](const ByteBuffer& b) {
        std::ostringstream ss;
        for (size_t i = 0; i < b.size() && i < 80; ++i)
            ss << ' ' << std::hex << std::setw(2) << std::setfill('0') << static_cast<int>(b[i]);
        ss << " (len=" << std::dec << b.size() << ")";
        return ss.str();
    };

    // The FILE_LIST immediate response carries the GUIDs but ZERO file sizes; the
    // real sizes arrive in a delayed SUBSYS_V2 response (TUF processes FILE_LIST
    // only from the delayed path for v2/v3). Capture delayed frames the same way
    // the download worker captures FILE_READ chunks, and wait for the FILE_LIST one.
    auto wait_delayed_file_list = [&](uint16_t subcmd) -> ByteBuffer {
        auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
        while (running_.load()) {
            std::unique_lock<std::mutex> lock(qshrink_rx_mutex_);
            bool got = qshrink_rx_cv_.wait_for(lock, std::chrono::milliseconds(500),
                [&] { return !qshrink_rx_queue_.empty() || !running_.load(); });
            if (!running_.load()) return {};
            if (!got || qshrink_rx_queue_.empty()) {
                if (std::chrono::steady_clock::now() >= deadline) return {};
                continue;
            }
            ByteBuffer frame = std::move(qshrink_rx_queue_.front());
            qshrink_rx_queue_.pop();
            lock.unlock();

            SubsysV2Header hdr = parse_subsys_v2_header(frame);
            if (!hdr.valid || hdr.command != subcmd || frame.size() < 16) continue;
            uint16_t op = static_cast<uint16_t>(frame[14]) | (static_cast<uint16_t>(frame[15]) << 8);
            if (op != HandshakeCmd::QSR4_OP_FILE_LIST) continue;
            return frame;   // delayed FILE_LIST response for this peripheral
        }
        return {};
    };

    // Enable delayed-frame capture for the whole GUID query; only FILE_LIST commands
    // are sent here, so capturing unpaired SUBSYS_V2 DIAG_SERV frames is safe.
    {
        std::lock_guard<std::mutex> lock(qshrink_rx_mutex_);
        std::queue<ByteBuffer> empty;
        std::swap(qshrink_rx_queue_, empty);
    }
    qshrink_capturing_.store(true);

    const auto& peripherals = get_qshrink_peripherals();
    for (const auto& periph : peripherals) {
        bool got_result = false;
        const std::string comp = qshrink_component_label(periph.subcmd);
        uint16_t negotiated_version = 0;
        size_t negotiated_files = 0;

        // Try protocol versions 3, 2, 1
        for (uint16_t version = 3; version >= 1 && !got_result; --version) {
            static constexpr int MAX_EFS_RETRIES = 3;
            for (int retry = 0; retry < MAX_EFS_RETRIES; ++retry) {
                // Drop any delayed frames left over from a prior attempt.
                {
                    std::lock_guard<std::mutex> lock(qshrink_rx_mutex_);
                    std::queue<ByteBuffer> empty;
                    std::swap(qshrink_rx_queue_, empty);
                }
                ByteBuffer cmd = build_qshrink_file_list(periph.subcmd, version);
                verbose_log("QShrink4 [" + comp + "] FILE_LIST v" + std::to_string(version) +
                            " req:" + hex_dump(cmd));
                ByteBuffer resp = send_command(cmd);

                if (resp.empty()) {
                    verbose_log("QShrink4 [" + comp + "] FILE_LIST v" +
                                std::to_string(version) + " - no response");
                    break;  // no response, try next version
                }

                verbose_log("QShrink4 [" + comp + "] FILE_LIST v" + std::to_string(version) +
                            " resp:" + hex_dump(resp));

                auto result = parse_qshrink_file_list(resp, version);

                if (!result.valid) {
                    verbose_log("QShrink4 [" + comp + "] FILE_LIST v" +
                                std::to_string(version) + " - invalid response");
                    break;  // invalid, try next version
                }

                // EFS busy/retry errors (status 6 or 8)
                if (result.status == 6 || result.status == 8) {
                    verbose_log("QShrink4 [" + comp + "] FILE_LIST v" +
                                std::to_string(version) +
                                " - EFS error (status=" + std::to_string(result.status) +
                                "), retry " + std::to_string(retry + 1));
                    continue;  // retry same version
                }

                if (result.ok() && !result.files.empty()) {
                    // The immediate response gives GUIDs with size 0. If it is not the
                    // final response (resp_id != 0), the real sizes come in a delayed
                    // FILE_LIST response - wait for it and re-parse.
                    QshrinkFileListResult sized = result;   // fallback: immediate (size 0)
                    SubsysV2Header imm_hdr = parse_subsys_v2_header(resp);
                    if (!imm_hdr.is_final()) {
                        ByteBuffer delayed = wait_delayed_file_list(periph.subcmd);
                        if (!delayed.empty()) {
                            verbose_log("QShrink4 [" + comp + "] FILE_LIST v" +
                                        std::to_string(version) + " delayed resp:" +
                                        hex_dump(delayed));
                            auto dresult = parse_qshrink_file_list(delayed, version);
                            if (dresult.valid && !dresult.files.empty()) {
                                sized = dresult;   // real file sizes
                            }
                        } else {
                            CFLOG_INFO("QShrink4 [" + comp + "] FILE_LIST v" +
                                       std::to_string(version) +
                                       " - no delayed response (sizes unknown)", true);
                        }
                    }

                    for (auto file : sized.files) {
                        file.peripheral_subcmd = periph.subcmd;
                        // file.version is already the device-echoed version from the
                        // parser - do NOT overwrite with the requested version.
                        handshake_state_.qshrink_guids.push_back(file);
                    }
                    // Populate GUID in matching diagid_table entry
                    for (auto& de : handshake_state_.diagid_table.entries) {
                        if (de.processor_name == periph.name) {
                            de.guid = sized.files[0].guid;
                            break;
                        }
                    }
                    negotiated_version = sized.version;  // echoed version
                    negotiated_files = sized.files.size();
                    got_result = true;
                } else {
                    // Visible on stdout: shows each version the device rejected and
                    // its status, so the downgrade path (e.g. v3->v2) is traceable.
                    CFLOG_INFO("QShrink4 [" + comp + "] FILE_LIST v" +
                               std::to_string(version) +
                               " - rejected (status=" + std::to_string(result.status) +
                               ", " + std::to_string(result.files.size()) + " files)", true);
                    break;  // non-retryable status, try next version
                }
                break;  // got a valid response, stop retrying
            }
        }

        // Always-visible per-component negotiation result.
        if (got_result) {
            CFLOG_INFO("QShrink4 [" + comp + "] FILE_LIST negotiated v" +
                       std::to_string(negotiated_version) + " - " +
                       std::to_string(negotiated_files) + " file(s)", true);
        } else {
            CFLOG_INFO("QShrink4 [" + comp + "] FILE_LIST - no version accepted (skipped)", true);
        }
    }

    qshrink_capturing_.store(false);
    {
        std::lock_guard<std::mutex> lock(qshrink_rx_mutex_);
        std::queue<ByteBuffer> empty;
        std::swap(qshrink_rx_queue_, empty);
    }

    verbose_log("QShrink4: Total GUIDs collected: " +
                std::to_string(handshake_state_.qshrink_guids.size()));
    return !handshake_state_.qshrink_guids.empty();
}

void DiagLoggerService::download_qshrink_files() {
    const auto& files = handshake_state_.qshrink_guids;
    if (files.empty()) {
        verbose_log("QShrink4 download: no GUIDs collected, nothing to download");
        return;
    }

    // Resolve output directory prefix once (same logic as write_qlc_file()).
    std::string dir = config_.output_path;
    if (!dir.empty() && dir.back() != '/' && dir.back() != '\\') dir += '/';

    auto hex_dump = [](const ByteBuffer& b) {
        std::ostringstream ss;
        for (size_t i = 0; i < b.size() && i < 40; ++i)
            ss << ' ' << std::hex << std::setw(2) << std::setfill('0') << static_cast<int>(b[i]);
        ss << " (len=" << std::dec << b.size() << ")";
        return ss.str();
    };

    for (const auto& entry : files) {
        if (!running_.load()) return;

        std::string guid_str = guid_to_string(entry.guid);
        const std::string comp = qshrink_component_label(entry.peripheral_subcmd);

        // A FILE_LIST entry with size 0 is NOT present on the device (it lives on the
        // server / must be supplied by the user). TUF removes these from the download
        // candidates rather than FILE_OPEN/READ them - doing so returns an error and a
        // 0-byte read. Skip them here too.
        if (entry.file_length == 0) {
            CFLOG_INFO("QShrink4 [" + comp + "] skip guid=" + guid_str +
                       " (FILE_LIST size=0, not on device)", true);
            continue;
        }

        // Skip the download if the decompressed .qsr4 already exists in the output
        // folder (matches TUF's doesFileExist check). The saved name uses the guid from
        // the .qdb header, which is the FILE_LIST guid in GUID mixed-endian order
        // (Data1/2/3 byte-reversed); apply that swap to predict the filename.
        std::array<uint8_t, 16> qsr4_guid = entry.guid;
        std::swap(qsr4_guid[0], qsr4_guid[3]);
        std::swap(qsr4_guid[1], qsr4_guid[2]);
        std::swap(qsr4_guid[4], qsr4_guid[5]);
        std::swap(qsr4_guid[6], qsr4_guid[7]);
        std::string existing_path = dir + "msg_hash_" + guid_to_string(qsr4_guid) + ".qsr4";
        if (std::ifstream(existing_path, std::ios::binary).good()) {
            CFLOG_INFO("QShrink4 [" + comp + "] skip guid=" + guid_str +
                       " (already downloaded: " + existing_path + ")", true);
            continue;
        }

        // FILE_LIST negotiates the version; file ops downgrade v3->v2 (device only
        // supports OPEN/READ/CLOSE at v2). For v2/v3 the device answers OPEN/READ/CLOSE
        // on the DELAYED path (like FILE_LIST): the immediate response is only an ack
        // with placeholder values (fd=0 for OPEN, status=5 for READ) and must be
        // ignored. TUF's immediate dispatch handles these ops only for version 1, so
        // the REAL fd / data come from the delayed responses.
        uint16_t version = (entry.version == 3) ? 2 : entry.version;

        auto clear_qshrink_queue = [&]() {
            std::lock_guard<std::mutex> lock(qshrink_rx_mutex_);
            std::queue<ByteBuffer> empty;
            std::swap(qshrink_rx_queue_, empty);
        };
        // Wait for a DELAYED SUBSYS_V2 response for this peripheral with the given op.
        auto wait_delayed = [&](uint16_t want_op, int timeout_ms) -> ByteBuffer {
            auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
            while (running_.load()) {
                std::unique_lock<std::mutex> lock(qshrink_rx_mutex_);
                bool got = qshrink_rx_cv_.wait_for(lock, std::chrono::milliseconds(300),
                    [&] { return !qshrink_rx_queue_.empty() || !running_.load(); });
                if (!running_.load()) return {};
                if (!got || qshrink_rx_queue_.empty()) {
                    if (std::chrono::steady_clock::now() >= deadline) return {};
                    continue;
                }
                ByteBuffer f = std::move(qshrink_rx_queue_.front());
                qshrink_rx_queue_.pop();
                lock.unlock();
                SubsysV2Header h = parse_subsys_v2_header(f);
                if (!h.valid || h.command != entry.peripheral_subcmd || f.size() < 16) continue;
                uint16_t op = static_cast<uint16_t>(f[14]) | (static_cast<uint16_t>(f[15]) << 8);
                if (op != want_op) continue;
                return f;
            }
            return {};
        };

        qshrink_capturing_.store(true);

        // ---- FILE_OPEN: the real fd is in the DELAYED response ----
        CFLOG_INFO("QShrink4 [" + comp + "] FILE_OPEN guid=" + guid_str + " v" +
                   std::to_string(version) + " (FILE_LIST v" + std::to_string(entry.version) +
                   ") size=" + std::to_string(entry.file_length) + " bytes", true);
        clear_qshrink_queue();
        ByteBuffer open_req = build_qshrink_file_open(entry.peripheral_subcmd, version, entry.guid);
        verbose_log("QShrink4 [" + comp + "] FILE_OPEN req:" + hex_dump(open_req));
        send_command(open_req);   // immediate ack (placeholder fd) - ignored
        ByteBuffer open_del = wait_delayed(HandshakeCmd::QSR4_OP_FILE_OPEN, 5000);
        if (open_del.empty()) {
            std::string msg = "QShrink4 [" + comp + "] download FAILED (open): guid=" + guid_str
                + " (no delayed response)";
            CFLOG_ERROR(msg, true);
            std::cout << msg << std::endl;
            qshrink_capturing_.store(false);
            clear_qshrink_queue();
            continue;
        }
        verbose_log("QShrink4 [" + comp + "] FILE_OPEN delayed resp:" + hex_dump(open_del));
        auto open_r = parse_qshrink_file_open_response(open_del);
        if (!open_r.ok()) {
            std::string msg = "QShrink4 [" + comp + "] download FAILED (open): guid=" + guid_str
                + " status=" + std::to_string(open_r.status);
            CFLOG_ERROR(msg, true);
            std::cout << msg << std::endl;
            qshrink_capturing_.store(false);
            clear_qshrink_queue();
            continue;
        }
        uint16_t fd = open_r.file_descriptor;
        CFLOG_INFO("QShrink4 [" + comp + "] FILE_OPEN ok, fd=" + std::to_string(fd), true);

        // ---- FILE_READ: request the whole file; data arrives as DELAYED responses ----
        // The immediate response is a placeholder ack (status 5) and is ignored. The
        // device streams the file as delayed SUBSYS_V2 FILE_READ responses (each a chunk
        // with its own absolute offset/numBytes/status); reassemble by offset.
        ByteBuffer qdb(entry.file_length, 0);
        size_t total_bytes = 0;
        bool read_ok = true;

        auto place_chunk = [&](const ByteBuffer& frame) -> size_t {
            auto rr = parse_qshrink_file_read_response(frame);
            if (!rr.valid || rr.status != 0 || rr.data.empty()) return 0;
            if (static_cast<size_t>(rr.offset) + rr.data.size() > qdb.size()) return 0;
            std::copy(rr.data.begin(), rr.data.end(), qdb.begin() + rr.offset);
            total_bytes += rr.data.size();
            return rr.data.size();
        };

        clear_qshrink_queue();
        ByteBuffer read_req =
            build_qshrink_file_read(entry.peripheral_subcmd, version, fd, entry.file_length, 0);
        verbose_log("QShrink4 [" + comp + "] FILE_READ req:" + hex_dump(read_req));
        send_command(read_req);   // immediate ack - ignored; data comes delayed

        bool done = false, first_del_logged = false;
        auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(25);
        while (!done && running_.load()) {
            std::unique_lock<std::mutex> lock(qshrink_rx_mutex_);
            bool got = qshrink_rx_cv_.wait_for(lock, std::chrono::milliseconds(500),
                [&] { return !qshrink_rx_queue_.empty() || !running_.load(); });
            if (!running_.load()) { read_ok = false; break; }
            if (!got || qshrink_rx_queue_.empty()) {
                if (std::chrono::steady_clock::now() >= deadline) break;
                continue;
            }
            ByteBuffer frame = std::move(qshrink_rx_queue_.front());
            qshrink_rx_queue_.pop();
            lock.unlock();

            SubsysV2Header hdr = parse_subsys_v2_header(frame);
            if (!hdr.valid || hdr.command != entry.peripheral_subcmd || frame.size() < 16) continue;
            uint16_t op = static_cast<uint16_t>(frame[14]) | (static_cast<uint16_t>(frame[15]) << 8);
            if (op != HandshakeCmd::QSR4_OP_FILE_READ) continue;
            if (!first_del_logged) {
                verbose_log("QShrink4 [" + comp + "] FILE_READ delayed resp:" + hex_dump(frame));
                first_del_logged = true;
            }
            place_chunk(frame);
            if (hdr.is_final() || total_bytes >= entry.file_length) done = true;
        }
        read_ok = read_ok && (total_bytes >= entry.file_length);

        // ---- FILE_CLOSE: send and consume its delayed response so it cannot mis-pair
        // with the next file's FILE_OPEN (they share the same subsys+command signature).
        clear_qshrink_queue();
        ByteBuffer close_req = build_qshrink_file_close(entry.peripheral_subcmd, version, fd);
        verbose_log("QShrink4 [" + comp + "] FILE_CLOSE req:" + hex_dump(close_req));
        send_command(close_req);
        wait_delayed(HandshakeCmd::QSR4_OP_FILE_CLOSE, 1500);

        qshrink_capturing_.store(false);
        clear_qshrink_queue();

        if (!read_ok || total_bytes < entry.file_length) {
            std::string msg = "QShrink4 [" + comp + "] download FAILED (read): guid=" + guid_str
                + " got " + std::to_string(total_bytes) + "/" + std::to_string(entry.file_length)
                + " bytes";
            CFLOG_ERROR(msg, true);
            std::cout << msg << std::endl;
            continue;
        }
        CFLOG_INFO("QShrink4 [" + comp + "] FILE_READ ok: " + std::to_string(total_bytes) +
                   "/" + std::to_string(entry.file_length) + " bytes", true);

        // Validate + decompress + save
        std::array<uint8_t, 16> file_guid{};
        if (!validate_qdb_header(qdb, file_guid)) {
            std::string msg = "QShrink4 [" + comp + "] download FAILED (bad .qdb header): guid=" + guid_str;
            CFLOG_ERROR(msg, true);
            std::cout << msg << std::endl;
            continue;
        }
        std::string out_path = dir + "msg_hash_" + guid_to_string(file_guid) + ".qsr4";
        if (!decompress_qdb_to_qsr4(qdb, out_path)) {
            std::string msg = "QShrink4 [" + comp + "] download FAILED (decompress): guid=" + guid_str;
            CFLOG_ERROR(msg, true);
            std::cout << msg << std::endl;
            continue;
        }

        CFLOG_INFO("QShrink4 [" + comp + "] saved: " + out_path, true);
    }
}

void DiagLoggerService::enable_adpl_log_codes() {
    // DPL log codes the device needs enabled to emit ADPL data (ADPLSteps.docx step 6/7):
    //   0x11EB  LOG_DATA_PROTOCOL_LOGGING_C  ("the data itself")
    //   0x1951  LOG_DATA_ADPL_SIO_CONFIG     (for ADPLv2+ de-mux/reconstruct)
    // Both are equipment-ID 1. The mask bit index is the log-item number (code & 0xFFF),
    // LSB-first within each byte.
    static constexpr uint8_t kEquip = 1;
    static const uint16_t kDplLogCodes[] = { 0x11EB, 0x1951 };

    auto is_equip1_setmask = [](const ByteBuffer& c) {
        if (c.size() < 16 || c[0] != DiagCmd::LOG_CONFIG) return false;
        uint32_t op    = c[4] | (c[5] << 8) | (c[6] << 16) | (uint32_t(c[7]) << 24);
        uint32_t equip = c[8] | (c[9] << 8) | (c[10] << 16) | (uint32_t(c[11]) << 24);
        return op == LogConfigOp::SET_MASK && equip == kEquip;
    };

    ByteBuffer* target = nullptr;
    for (auto& c : cfg_commands_) {
        if (is_equip1_setmask(c)) { target = &c; break; }
    }

    if (!target) {
        // No equip-1 mask in the .cfg — build one covering just the DPL codes.
        uint32_t max_item = 0;
        for (uint16_t code : kDplLogCodes)
            max_item = std::max<uint32_t>(max_item, code & 0x0FFF);
        uint32_t last_item = max_item + 1;
        std::vector<uint8_t> mask((last_item + 7) / 8, 0);
        for (uint16_t code : kDplLogCodes) {
            uint32_t item = code & 0x0FFF;
            mask[item / 8] |= static_cast<uint8_t>(1u << (item % 8));
        }
        cfg_commands_.push_back(build_log_config_set_mask(kEquip, last_item, mask));
        verbose_log("ADPL: added equip-1 log mask for DPL codes 0x11EB, 0x1951");
        return;
    }

    // Merge the DPL bits into the existing equip-1 mask.
    ByteBuffer& c = *target;
    uint32_t last_item = c[12] | (c[13] << 8) | (c[14] << 16) | (uint32_t(c[15]) << 24);
    for (uint16_t code : kDplLogCodes) {
        uint32_t item = code & 0x0FFF;
        if (item >= last_item) {
            last_item = item + 1;
            c[12] = static_cast<uint8_t>(last_item & 0xFF);
            c[13] = static_cast<uint8_t>((last_item >> 8) & 0xFF);
            c[14] = static_cast<uint8_t>((last_item >> 16) & 0xFF);
            c[15] = static_cast<uint8_t>((last_item >> 24) & 0xFF);
        }
        size_t need_bytes = (last_item + 7) / 8;
        if (16 + need_bytes > c.size()) {
            c.resize(16 + need_bytes, 0);
        }
        c[16 + item / 8] |= static_cast<uint8_t>(1u << (item % 8));
    }
    verbose_log("ADPL: enabled DPL log codes 0x11EB, 0x1951 in equip-1 mask");
}

bool DiagLoggerService::send_cfg_commands() {
    for (const auto& cmd : cfg_commands_) {
        if (cmd.empty()) continue;
        ByteBuffer resp = send_command(cmd);
        if (resp.empty()) {
            return false;
        }
    }
    return true;
}

void DiagLoggerService::write_qlc_file() {
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
    meta.hdlc_data_type = (diag_ && diag()->framing_mode() == FramingMode::HDLC) ? 1 : 0;
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

void DiagLoggerService::verbose_log(const std::string& msg) {
    if (config_.verbose) {
        CFLOG_DEBUG("[verbose] " + msg, true);
    }
}

} // namespace QC
