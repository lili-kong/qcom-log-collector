// Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
// SPDX-License-Identifier: BSD 3-Clause Clear License
//
// DiagLogger: high-level, CLI-facing wrapper around the internal
// DiagLoggerService engine. Exposes the transport open/close lifecycle plus
// the logging start/stop commands. This header — together with
// device_discovery.h and Definitions.h — is the complete public surface of
// the DiagLogger library; DiagLoggerService and everything it depends on
// (Transport, Protocol, PtraceWriter, Qdss, Adpl, ...) is an internal
// implementation detail hidden behind this pimpl.

#ifndef QC_EXPORTS_DIAG_LOGGER_H
#define QC_EXPORTS_DIAG_LOGGER_H

#include "exports/Definitions.h"

#include <chrono>
#include <cstdint>
#include <memory>
#include <string>

namespace QC {

struct DiagLoggerConfig {
    std::string cfg_path;
    std::string port_name;
    std::string qdss_port_name;
    std::string adpl_port_name;
    std::string output_path;
    uint32_t    baud_rate = 115200;
    int         duration_seconds = 0;  // 0 = unlimited
    uint32_t    command_timeout_ms = 2000;  // Default timeout for all send_command calls
    bool        verbose = false;
    bool        hdlc_only = false;   // Force HDLC mode, skip Non-HDLC switch
    bool        enable_recovery = true;        // Reboot recovery (default ON)
    int         recovery_timeout_seconds = 120; // Max wait for device to come back
    bool        enable_qdss = false;
    bool        enable_adpl = false;         // Write .adplvX file alongside ptrace
    uint8_t     qdss_processor_mask = 0x3F;  // QdssProcessorMask::ALL
    uint8_t     adpl_version = 7;            // AdplVersion::HIGHEST_SUPPORTED
};

/// Statistics gathered during logging.
struct DiagStats {
    uint64_t rx_packets = 0;
    uint64_t rx_bytes = 0;
    uint64_t tx_packets = 0;
    uint64_t tx_bytes = 0;
    uint32_t frame_errors = 0;
};

// Internal engine. Full definition lives in function/diag_logger_service.h,
// which is not part of the public API — consumers only ever see it through
// this pimpl.
class DiagLoggerService;

// C4251: impl_ is a std::unique_ptr over the incomplete DiagLoggerService and
// has no dll-interface. It is private and never touched across the DLL
// boundary — every access happens inside src/diag_logger.cpp — so the warning
// does not apply here.
#ifdef _MSC_VER
#pragma warning(push)
#pragma warning(disable: 4251)
#endif

/// High-level diag logging API for CLI (and other) consumers.
/// Owns a DiagLoggerService and forwards the transport lifecycle plus the
/// logging start/stop commands to it.
class QLC_API DiagLogger {
public:
    explicit DiagLogger(const DiagLoggerConfig& config);
    ~DiagLogger();

    // Non-copyable
    DiagLogger(const DiagLogger&) = delete;
    DiagLogger& operator=(const DiagLogger&) = delete;

    /// Set a protocol's desired state to OPEN/CLOSED. Idempotent and
    /// non-blocking — safe to call from a device-monitor callback (see
    /// DeviceDiscovery). The actual open/close, including the connect
    /// handshake, runs on that protocol's own lifecycle worker.
    ///
    /// PROT_DIAG exists from construction. PROT_QDSS and PROT_ADPL do not
    /// exist until start() brings them up (and only when enable_qdss /
    /// enable_adpl is set), so a call naming one of those before start(), or
    /// naming any other type, is a no-op. Normally you only drive PROT_DIAG:
    /// the QDSS and ADPL ports are opened and closed as part of start()/stop()
    /// alongside their capture files.
    void request_open(ProtocolType type = PROT_DIAG);
    void request_close(ProtocolType type = PROT_DIAG);

    /// Block until the named protocol's port is open/closed, or timeout
    /// elapses. Returns the final state as a bool, so an absent protocol
    /// reports false from wait_ready() and true from wait_closed().
    ///
    /// NOTE: wait_ready() returning true means only that the PORT is open —
    /// the handshake may still be in flight. Confirm with protocol_state()
    /// or just call start(), which validates readiness itself.
    bool wait_ready(std::chrono::milliseconds timeout, ProtocolType type = PROT_DIAG);
    bool wait_closed(std::chrono::milliseconds timeout, ProtocolType type = PROT_DIAG);

    /// Current protocol-level state for a protocol type. PROT_DIAG is always
    /// present; PROT_QDSS / PROT_ADPL report STATE_DISCONNECTED until
    /// enable_qdss / enable_adpl brings them up during start().
    ProtocolState protocol_state(ProtocolType type = PROT_DIAG) const;

    /// Validate that the diag protocol is ready to carry commands, waiting up
    /// to timeout for it to reach STATE_AVAILABLE. Throws std::runtime_error
    /// if the timeout elapses first. Call once after wait_ready() succeeds,
    /// and again before any command sequence where a mid-run link loss should
    /// abort rather than be absorbed per-command.
    void validate_diag_ready(std::chrono::milliseconds timeout = std::chrono::seconds(5)) const;

    /// Run device setup and begin logging. Requires the diag port to be open
    /// and its handshake to have passed; validates that itself and fails
    /// cleanly if not.
    bool start();

    /// Stop logging: send disable commands, join worker threads, print the
    /// session summary. Idempotent — the graceful teardown runs exactly once.
    /// Does not close the port; call request_close() after this.
    void stop();

    /// Whether the logger is currently running.
    bool is_running() const;

    /// Get current statistics.
    DiagStats stats() const;

private:
    std::unique_ptr<DiagLoggerService> impl_;
};

#ifdef _MSC_VER
#pragma warning(pop)
#endif

} // namespace QC

#endif // QC_EXPORTS_DIAG_LOGGER_H
