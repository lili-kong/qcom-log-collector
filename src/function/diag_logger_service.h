// Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
// SPDX-License-Identifier: BSD 3-Clause Clear License
//
// qualcomm-log-collector orchestrator: connect → configure → receive loop → shutdown.
//
// Owns a Diag protocol object (which owns the transport and provides the
// TX/RX pumps, framing, handshake and response pairing) and layers the
// logging session on top: CFG masks, QDSS/ADPL setup, QShrink4 download,
// .qlc metadata, reboot recovery and the idle keep-alive.

#ifndef QC_DIAG_LOGGER_SERVICE_H
#define QC_DIAG_LOGGER_SERVICE_H

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <functional>
#include <memory>
#include <mutex>
#include <queue>
#include <string>
#include <thread>

#include "diag/adpl_commands.h"
#include "diag/cfg_parser.h"
#include "diag/handshake_commands.h"
#include "diag/qdss_commands.h"
#include "diag/response_pairing.h"
// DiagLoggerConfig and DiagStats are part of the public API: the engine takes
// the caller's config and reports stats back unchanged, so both live in
// exports/diag_logger.h rather than being redefined here.
#include "exports/diag_logger.h"
#include "types.h"
#include "file_writer/qlc_writer.h"
#include "file_writer/qsr4_writer.h"
// FramingMode comes from protocol/diag.h — it is the Diag layer's concern,
// not this orchestrator's.
#include "protocol/adpl/adpl.h"
#include "protocol/diag.h"
#include "protocol/qdss/qdss.h"

namespace QC {

/// State gathered during the post-open setup sequence.
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

class DiagLoggerService {
public:
    explicit DiagLoggerService(const DiagLoggerConfig& config);
    ~DiagLoggerService();

    // Non-copyable
    DiagLoggerService(const DiagLoggerService&) = delete;
    DiagLoggerService& operator=(const DiagLoggerService&) = delete;

    /// Prepare everything that does not need the port: parse the CFG file and
    /// register the async-frame sink. Called from the constructor so a caller
    /// can drive the protocol lifecycle immediately.
    /// Returns false if the CFG file could not be parsed.
    bool prepare();

    /// Run the post-open device setup, launch the session's worker threads,
    /// and mark logging active. Requires the diag protocol to have reached
    /// STATE_AVAILABLE — open it via get_protocol(PROT_DIAG)->request_open()
    /// and confirm with get_protocol_state(PROT_DIAG) first.
    /// Returns true on success.
    bool start();

    /// Stop logging: send disable commands, signal and join this service's
    /// worker threads, and print the session summary. Idempotent — the
    /// graceful teardown runs exactly once.
    ///
    /// Does NOT close the port: the caller owns that, through
    /// get_protocol(PROT_DIAG)->request_close(). Call stop() first so the
    /// disable commands still have a live port to go out on.
    void stop();

    /// Whether the logger is currently running.
    bool is_running() const { return running_.load(); }

    /// Get current statistics.
    DiagStats stats() const;

    /// The Protocol instance driving a given protocol type, for callers that
    /// want uniform lifecycle control (request_open/request_close/wait_ready/
    /// state) without knowing the concrete class.
    ///
    /// PROT_DIAG is present from construction. PROT_QDSS and PROT_ADPL are
    /// null until --enable-qdss / --enable-adpl bring them up in
    /// post_open_setup(), and null again if their start() failed. Any other
    /// type yields a null handle. Always test the returned handle before
    /// dereferencing it.
    ///
    /// The reference is valid for this service's lifetime; ownership stays
    /// here.
    const std::unique_ptr<Protocol>& get_protocol(ProtocolType type) const;

    /// Current protocol-level state for a given protocol type.
    ///
    /// Reports STATE_DISCONNECTED when that protocol is not present — a type
    /// this service does not drive, or QDSS/ADPL before --enable-qdss /
    /// --enable-adpl brings it up. That matches Protocol's own initial state,
    /// so "absent" and "not connected" read the same to a caller that is only
    /// asking whether it can send.
    ProtocolState get_protocol_state(ProtocolType type) const;

    /// Validate that the diag protocol is in a valid state for sending
    /// commands, waiting up to timeout for it to reach STATE_AVAILABLE.
    /// Throws std::runtime_error if the timeout elapses first.
    ///
    /// Call before a command sequence where a mid-run link loss should abort
    /// the whole operation rather than be absorbed one timed-out command at a
    /// time. A brief drop is given a chance to recover before this fails.
    void validate_diag_state(std::chrono::milliseconds timeout = std::chrono::seconds(5)) const;

private:
    /// Print the end-of-session counters and output file paths. Called once
    /// from stop().
    void print_session_summary();

    /// Join every thread this class owns (QShrink4 worker, keep-alive,
    /// recovery watchdog). Idempotent, and must run before diag_ is
    /// destroyed since those threads call into it.
    void join_service_threads();

    // Typed views of the protocol handles below. The members are stored as
    // the Protocol base so get_protocol() can hand out a uniform reference;
    // these recover the concrete type for the protocol-specific calls
    // (Diag::send_and_wait, Qdss::start, Adpl::bytes_captured, ...). The
    // casts are safe because this class is the only thing that constructs
    // them, and it always constructs the matching type.
    Diag* diag() const { return static_cast<Diag*>(diag_.get()); }
    Qdss* qdss() const { return static_cast<Qdss*>(qdss_.get()); }
    Adpl* adpl() const { return static_cast<Adpl*>(adpl_.get()); }

    DiagLoggerConfig config_;
    // Diag owns the transport, the TX/RX pumps, framing, the connect
    // handshake and response pairing. This class drives it through
    // request_open()/send_and_wait() and never touches a Transport directly.
    //
    // All three are held as unique_ptr<Protocol> rather than their concrete
    // types so get_protocol() can return one uniform reference type; use the
    // diag()/qdss()/adpl() views above for type-specific calls.
    std::unique_ptr<Protocol> diag_;
    std::unique_ptr<Protocol> qdss_;
    std::unique_ptr<Protocol> adpl_;
    std::vector<ByteBuffer> cfg_commands_;
    HandshakeState handshake_state_;

    // Session flag, distinct from Protocol's io_running_ (which is protected
    // and tracks only the port's I/O threads). This one spans the whole
    // logging session, backs the public is_running(), and is what the
    // QShrink4 worker and the recovery/poll threads check for shutdown.
    std::atomic<bool> running_{false};
    std::atomic<bool> stop_invoked_{false};   // ensures stop()/teardown runs exactly once
    bool prepared_ = false;      // CFG parsed and async sink registered
    bool logging_started_ = false;
    // One-shot guards: these run on the first connection only, NOT on reboot
    // recovery (post_open_setup re-runs on recovery). qlc_written_ keeps a
    // single .qlc file; qshrink_queried_ avoids re-querying the GUID list
    // while the download thread (launched once in start()) is iterating it —
    // which would be a data race.
    bool qlc_written_ = false;
    bool qshrink_queried_ = false;
    std::thread qshrink_thread_;

    // Frame-level RX statistics. Protocol counts raw wire bytes; these count
    // decoded frames, tallied in on_async_frame(). Both feed stats().
    std::atomic<uint64_t> async_rx_packets_{0};
    std::atomic<uint64_t> async_rx_bytes_{0};

    // --- Configuration restore after a reconnect -------------------------
    // The caller owns opening and closing the port (it reacts to device
    // arrival/departure events). This watcher notices the port coming back
    // AVAILABLE and re-applies the device configuration that a reboot wiped:
    // CFG masks, QDSS/ADPL enable, DiagID setup. Without it a reconnected
    // device would be open but silent, because the masks live in device RAM.
    std::thread restore_thread_;
    std::mutex restore_mutex_;
    std::condition_variable restore_cv_;

    /// Watches the diag protocol's state and re-runs post_open_setup() each
    /// time the port transitions back to AVAILABLE after having been lost.
    void config_restore_loop();

    // --- Idle keep-alive -------------------------------------------------
    // Ask the device for STATUS (0x0C) when nothing has arrived for
    // POLL_IDLE_TIMEOUT_MS, to notice a silently wedged link.
    std::thread poll_thread_;
    std::chrono::steady_clock::time_point last_rx_time_;
    std::mutex last_rx_mutex_;
    // Separate from restore_mutex_/restore_cv_ so a restore wait and the
    // poll tick never contend on the same lock.
    std::mutex poll_mutex_;
    std::condition_variable poll_cv_;

    void poll_loop();

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

    // How long to wait for the port's handshake to complete. Opening and
    // closing the port is the caller's job, so no open/close budget lives
    // here beyond this.
    static constexpr std::chrono::seconds HANDSHAKE_TIMEOUT{10};
    // How often the restore watcher samples the diag protocol's state.
    static constexpr std::chrono::milliseconds RESTORE_POLL_INTERVAL{250};

    // --- Async frame routing ---------------------------------------------
    // Installed on Diag before the port opens. Diag pairs sync responses
    // itself; everything else (log/event/message packets, and QShrink4's
    // unpaired "delayed" responses) arrives here.
    void on_async_frame(const ByteBuffer& frame);

    // QShrink4 FILE_READ delivers data via asynchronous "delayed" SUBSYS_V2
    // responses that are not paired to the request. While a download is
    // capturing, on_async_frame() routes those frames here for the download
    // thread.
    std::queue<ByteBuffer> qshrink_rx_queue_;
    std::mutex qshrink_rx_mutex_;
    std::condition_variable qshrink_rx_cv_;
    std::atomic<bool> qshrink_capturing_{false};

    // Send a command and wait for its paired response. Forwards to
    // Diag::send_and_wait(), which applies the active framing, records TX to
    // the port trace, and pairs the reply. Returns the decoded response
    // payload, or empty on timeout / shutdown.
    ByteBuffer send_command(const ByteBuffer& payload, uint32_t timeout_ms = 0);

    // Device setup performed once the port is open and Diag's handshake
    // (loopback, STATUS, Non-HDLC switch) has passed: device info for the
    // .qlc metadata, security keys, feature query, hypervisor/DiagID,
    // QShrink4 GUIDs, QDSS/ADPL, then the CFG masks. Re-runs after a
    // successful reboot recovery; the one-shot guards above keep the
    // session-level work from repeating.
    bool post_open_setup();

    // Setup QDSS (HW accel, STM, trace sink, SW trace filters).
    // Called from post_open_setup() when --enable-qdss is set.
    // Returns true if QDSS setup succeeded.
    bool setup_qdss();

    // ---- QDSS configuration over the diag port -----------------------------
    // Defined in src/function/diag_logger_qdss_manager.cpp. These drive the
    // control plane (this diag port); Qdss above owns the \TRACE data plane.

    // Configure QDSS: try HW acceleration, fall back to legacy STM.
    // Returns true if QDSS was configured successfully.
    bool configure_diag_over_qdss();

    // Disable QDSS: stop HW acceleration and STM.
    void disable_diag_over_qdss();

    // Query and enable HW acceleration (ATB, LPASS STM, STM).
    bool query_hw_acceleration();

    // Legacy Diag-over-STM configuration.
    bool query_diag_over_stm();

    // Query diag transport and set trace sink.
    void query_diag_transport();

    // Enable/disable STM trace (subsys=0x5A, cmd=259).
    bool enable_stm(bool enable);

    // Start QDSS commands (entity filters, SW trace, Diag-over-STM, STM request).
    void start_qdss_commands();

    // Stop QDSS commands.
    void stop_qdss_commands();

    // Stop HW acceleration (disable ATB, LPASS STM, STM).
    void stop_hw_acceleration();

    // QDSS control-plane state. hw_accel_ is copied into
    // handshake_state_.qdss_hw_accel for the .qlc metadata (fields 10/11).
    QdssHwAccelInfo hw_accel_;
    bool     hw_accel_enabled_ = false;
    uint32_t atb_enabled_mask_ = 0;
    uint32_t lpass_stm_enabled_mask_ = 0;
    uint32_t stm_enabled_mask_ = 0;
    uint8_t  qdss_trace_sink_ = QdssSinkType::USB;  // Device-reported trace sink (default USB)
    // Set once setup_qdss() begins configuring, so stop_logging() knows whether
    // there is anything to tear down (previously implied by the manager existing).
    bool     qdss_diag_setup_attempted_ = false;

    // Setup ADPL (enable accelerated data path logging).
    // Called from post_open_setup() when --enable-adpl is enabled.
    // Returns true if ADPL was enabled successfully.
    bool setup_adpl();

    // Merge the DPL log codes (0x11EB, 0x1951) into the equip-ID-1 LOG_CONFIG
    // SET_MASK in cfg_commands_ so the device actually generates ADPL data packets
    // (ADPLSteps.docx step 6/7). Called once when --enable-adpl is set, before the
    // masks are sent. Without this, the accelerated IPA request succeeds but no data
    // streams on \DPL.
    void enable_adpl_log_codes();

    // Send parsed CFG commands to the device.
    bool send_cfg_commands();

    // Send graceful disconnect commands to the device (disable masks,
    // QDSS, ADPL, etc.) before shutting down. Must be called while the
    // port is still open. Returns true if all commands were acknowledged.
    bool stop_logging();

    // Query QShrink4 GUID tables from each peripheral.
    // Returns true if at least one peripheral responded.
    bool query_qshrink_guids();

    // Background worker: download each collected QShrink4 .qdb, decompress to
    // .qsr4, and save it into the output directory. Runs on qshrink_thread_.
    // Never blocks logging; per-file failures are logged and skipped.
    void download_qshrink_files();

    // Write the .qlc metadata file alongside the ptrace output.
    void write_qlc_file();

    // Log to stderr if verbose mode is on.
    void verbose_log(const std::string& msg);
};

} // namespace QC

#endif // QC_DIAG_LOGGER_SERVICE_H
