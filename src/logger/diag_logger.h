// SPDX-License-Identifier: Apache-2.0
// Copyright 2024 Qualcomm Innovation Center, Inc.
//
// qualcomm-log-collector orchestrator: connect → configure → receive loop → shutdown.

#ifndef DIAGLOGGER_DIAG_LOGGER_H
#define DIAGLOGGER_DIAG_LOGGER_H

#include "diaglogger/types.h"
#include "transport/transport.h"
#include "hdlc/hdlc.h"
#include "hdlc/non_hdlc.h"
#include "ptrace/ptrace_writer.h"
#include "diag/cfg_parser.h"
#include "diag/handshake_commands.h"
#include "diag/qdss_commands.h"
#include "diag/adpl_commands.h"
#include "diag/response_pairing.h"
#include "qlc/qlc_writer.h"

#include <atomic>
#include <chrono>
#include <thread>
#include <memory>
#include <string>
#include <functional>
#include <mutex>
#include <condition_variable>
#include <queue>
#include <future>

namespace diaglogger {

struct DiagLoggerConfig {
    std::string cfg_path;
    std::string port_name;
    std::string output_path;
    uint32_t    baud_rate = 115200;
    int         duration_seconds = 0;  // 0 = unlimited
    uint32_t    command_timeout_ms = 2000;  // Default timeout for all send_command calls
    bool        verbose = false;
    bool        hdlc_only = false;   // Force HDLC mode, skip Non-HDLC switch
    bool        enable_recovery = true;        // Reboot recovery (default ON)
    int         recovery_timeout_seconds = 120; // Max wait for device to come back
    bool        enable_qdss = false;
    bool        enable_adpl = false;
    uint8_t     qdss_processor_mask = 0x3F;  // QdssProcessorMask::ALL
    uint8_t     adpl_version = 7;            // AdplVersion::HIGHEST_SUPPORTED
};

/// State gathered during the handshake sequence.
struct HandshakeState {
    FeatureQueryResult feature_query;
    QdssHwAccelInfo qdss_hw_accel;
    uint8_t adpl_negotiated_version = 0;
    uint8_t default_diag_id = 1;  // apps processor

    // Metadata for .qlc file
    DiagIdTableResult diagid_table;
    SecurityKeyResult security_keys;
    QlcDeviceInfo device_info;
    std::vector<QshrinkFileEntry> qshrink_guids;
};

/// Active framing mode for serial communication.
enum class FramingMode {
    HDLC,      // Standard HDLC (0x7E delimiters, CRC-16, byte-stuffing)
    NON_HDLC   // Length-prefixed (0x7E + version + length + payload + 0x7E, no CRC)
};

/// Statistics gathered during logging.
struct DiagStats {
    uint64_t rx_packets = 0;
    uint64_t rx_bytes = 0;
    uint64_t tx_packets = 0;
    uint64_t tx_bytes = 0;
    uint32_t frame_errors = 0;
};

class DiagLogger {
public:
    explicit DiagLogger(const DiagLoggerConfig& config);
    ~DiagLogger();

    // Non-copyable
    DiagLogger(const DiagLogger&) = delete;
    DiagLogger& operator=(const DiagLogger&) = delete;

    /// Open transport, launch worker threads, run connection handshake.
    bool open_transport();

    /// Join worker threads and close the transport.
    void close_transport();


    /// Start logging: parse CFG, mark logging active.
    /// Requires open_transport() to be called first.
    /// Returns true on success.
    bool start();

    /// Stop logging: send disable commands, signal threads to stop.
    void stop();

    /// Whether the logger is currently running.
    bool is_running() const { return running_.load(); }

    /// Get current statistics.
    DiagStats stats() const;

private:
    DiagLoggerConfig config_;
    std::unique_ptr<Transport> transport_;
    HdlcDecoder hdlc_decoder_;
    NonHdlcDecoder non_hdlc_decoder_;
    FramingMode framing_mode_ = FramingMode::HDLC;
    PtraceWriter ptrace_;
    std::vector<ByteBuffer> cfg_commands_;
    HandshakeState handshake_state_;

    std::atomic<bool> running_{false};
    bool logging_started_ = false;
    std::thread rx_thread_;
    DiagStats stats_;

    // Transport lifecycle thread
    std::thread transport_thread_;
    bool transport_open_done_{false};
    bool transport_open_result_{false};
    bool transport_close_requested_{false};
    bool recovery_requested_{false};
    std::mutex transport_mutex_;
    std::condition_variable transport_cv_;

    void transport_ready_func();

    // TX thread and queue
    struct TxRequest {
        ByteBuffer payload;
        uint32_t timeout_ms;
        std::promise<ByteBuffer> promise;
    };

    std::thread tx_thread_;
    std::queue<std::unique_ptr<TxRequest>> tx_queue_;
    std::mutex tx_mutex_;
    std::condition_variable tx_cv_;

    // RX response queue: rx_loop (producer) -> dispatcher_loop (consumer).
    // Every decoded frame is pushed here first; the dispatcher pairs sync
    // responses to the outstanding request and routes async packets.
    std::thread dispatcher_thread_;
    std::queue<ByteBuffer> rx_queue_;
    std::mutex rx_queue_mutex_;
    std::condition_variable rx_queue_cv_;

    // Single outstanding request awaiting a paired response. tx_loop registers
    // it (under pending_mutex_); the dispatcher fulfills promise on a signature
    // match. At most one is active at a time (the natural request gate).
    struct PendingCommand {
        ByteBuffer signature;                      // bytes the response must match
        std::promise<ByteBuffer>* promise = nullptr;  // owned by the waiting send_and_wait()
    };
    PendingCommand pending_;
    std::mutex pending_mutex_;

    // Per-request response timeout (also used by the keep-alive poll).
    static constexpr uint32_t TX_RESPONSE_TIMEOUT_MS = 2000;

    // Polling: request a STATUS (0x0C) keep-alive when idle (no RX for
    // POLL_IDLE_TIMEOUT). Bumped above the response timeout so the poll never
    // fires while a previous response could still be arriving.
    static constexpr uint32_t POLL_IDLE_TIMEOUT_BASE_MS = 10000;  // 10 seconds
    static constexpr uint32_t POLL_IDLE_TIMEOUT_MS =
        (TX_RESPONSE_TIMEOUT_MS >= POLL_IDLE_TIMEOUT_BASE_MS)
            ? TX_RESPONSE_TIMEOUT_MS + 1000
            : POLL_IDLE_TIMEOUT_BASE_MS;
    std::chrono::steady_clock::time_point last_rx_time_;
    std::mutex last_rx_mutex_;
    std::atomic<bool> poll_requested_{false};

    // Send a command synchronously: enqueue a request for the tx thread and block
    // until the dispatcher pairs the response. The tx thread encodes/writes the
    // frame via send_and_wait() and records TX to ptrace; rx_loop records RX.
    // Returns the decoded response payload, or empty on timeout.
    ByteBuffer send_command(const ByteBuffer& payload, uint32_t timeout_ms = 0);

    // Perform loopback + device handshake sequence (runs on transport thread).
    bool do_connection();

    // Perform the loopback handshake.
    bool do_loopback();

    // Perform the full device handshake sequence (STATUS, EXT_BUILD_ID,
    // Non-HDLC switch, security keys, feature query, hypervisor/DiagID setup).
    // Returns true if the essential steps succeeded.
    bool do_handshake();

    // Attempt to switch to Non-HDLC framing mode.
    // Returns true if the device accepted the switch.
    // On success, framing_mode_ is set to NON_HDLC.
    bool request_non_hdlc_switch();

    // Setup QDSS (HW accel, STM, trace sink, SW trace filters).
    // Called from do_handshake() when --qdss is enabled.
    // Returns true if QDSS setup succeeded.
    bool setup_qdss();

    // Setup ADPL (enable accelerated data path logging).
    // Called from do_handshake() when --adpl is enabled.
    // Returns true if ADPL was enabled successfully.
    bool setup_adpl();

    // Send parsed CFG commands to the device.
    bool send_cfg_commands();

    // Send STATUS (0x0C) as a keep-alive poll. Builds the request and routes it
    // through send_and_wait() like any other command. Called from tx_loop().
    // Returns true if poll response was received.
    bool do_polling();

    // Send one request on the tx thread: encode, record TX, write to transport,
    // register the pending signature, and wait for the dispatcher to pair the
    // response. Returns the decoded response, or empty on timeout/shutdown.
    ByteBuffer send_and_wait(const ByteBuffer& payload, uint32_t timeout_ms);

    // Dispatcher loop (runs in dispatcher_thread_): pops rx_queue_, pairs sync
    // responses to the pending request, and routes async packets.
    void dispatcher_loop();

    // Transmit loop (runs in tx_thread_).
    // Pops requests from tx_queue_, performs the physical send and
    // waits for the response, then fulfills the promise.
    void tx_loop();

    // Receive loop (runs in rx_thread_).
    void rx_loop();



    // Send graceful disconnect commands to the device (disable masks,
    // QDSS, ADPL, etc.) before shutting down. Must be called while TX
    // thread is still running. Returns true if all commands were acknowledged.
    bool stop_logging();

    // Query QShrink4 GUID tables from each peripheral.
    // Returns true if at least one peripheral responded.
    bool query_qshrink_guids();

    // Write the .qlc metadata file alongside the ptrace output.
    void write_qlc_file();

    // Log to stderr if verbose mode is on.
    void verbose_log(const std::string& msg);
};

} // namespace diaglogger

#endif // DIAGLOGGER_DIAG_LOGGER_H
