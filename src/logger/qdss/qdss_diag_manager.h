// Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
// SPDX-License-Identifier: BSD 3-Clause Clear License
//
// QdssDiagManager: configures QDSS on the device by sending commands over
// the diag port.

#ifndef DIAGLOGGER_QDSS_DIAG_MANAGER_H
#define DIAGLOGGER_QDSS_DIAG_MANAGER_H

#include "diag/qdss_commands.h"
#include "diaglogger/types.h"

#include <cstdint>
#include <functional>
#include <string>

namespace QC {

/// Function type for sending a diag command and receiving a response.
using SendCommandFn = std::function<ByteBuffer(const ByteBuffer&, uint32_t timeout_ms)>;

/// Configures QDSS on the device via diag commands.
/// Handles HW acceleration setup, legacy STM fallback, and teardown.
class QdssDiagManager {
public:
    explicit QdssDiagManager(SendCommandFn send_command);

    /// Configure QDSS: try HW acceleration, fall back to legacy STM.
    /// Returns true if QDSS was configured successfully.
    bool configure_diag_over_qdss();

    /// Disable QDSS: stop HW acceleration and STM.
    void disable_diag_over_qdss();

    /// Whether HW acceleration is enabled (vs legacy STM path).
    bool is_hw_accel_enabled() const { return hw_accel_enabled_; }

    /// Get the HW acceleration info from query response.
    const QdssHwAccelInfo& hw_accel_info() const { return hw_accel_; }

    /// Get enabled masks (for status/debug reporting).
    uint32_t atb_enabled_mask() const { return atb_enabled_mask_; }
    uint32_t lpass_stm_enabled_mask() const { return lpass_stm_enabled_mask_; }
    uint32_t stm_enabled_mask() const { return stm_enabled_mask_; }

private:
    SendCommandFn send_command_;
    QdssHwAccelInfo hw_accel_;
    bool hw_accel_enabled_ = false;
    uint32_t atb_enabled_mask_ = 0;
    uint32_t lpass_stm_enabled_mask_ = 0;
    uint32_t stm_enabled_mask_ = 0;
    uint8_t processor_mask_ = QdssProcessorMask::ALL;
    uint8_t trace_sink_ = QdssSinkType::USB;  // Device-reported trace sink (default USB)

    /// Query and enable HW acceleration (ATB, LPASS STM, STM).
    bool query_hw_acceleration();

    /// Legacy Diag-over-STM configuration.
    bool query_diag_over_stm();

    /// Query diag transport and set trace sink.
    void query_diag_transport();

    /// Enable/disable STM trace (subsys=0x5A, cmd=259).
    bool enable_stm(bool enable);

    /// Start QDSS commands (entity filters, SW trace, Diag-over-STM, STM request).
    void start_qdss_commands();

    /// Stop QDSS commands.
    void stop_qdss_commands();

    /// Stop HW acceleration (disable ATB, LPASS STM, STM).
    void stop_hw_acceleration();

    /// Helper: hex-dump a response buffer for error logging.
    static std::string hex_dump(const ByteBuffer& buf);

    /// Helper: log verbose message.
    void verbose_log(const std::string& msg);
};

} // namespace QC

#endif // DIAGLOGGER_QDSS_DIAG_MANAGER_H
