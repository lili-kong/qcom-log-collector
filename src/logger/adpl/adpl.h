// Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
// SPDX-License-Identifier: BSD 3-Clause Clear License
//
// Adpl: manages ADPL port transport, RX thread, and raw ADPL binary file
// output. Owns the ADPL (\DPL) port lifecycle independently from DiagLogger's
// diag port. Structural mirror of Qdss — same BinaryWriter, same RX-thread
// model, same recovery semantics.

#ifndef DIAGLOGGER_ADPL_H
#define DIAGLOGGER_ADPL_H

#include "file_writer/binary_writer.h"
#include "transport/transport.h"

#include <atomic>
#include <cstdint>
#include <memory>
#include <string>
#include <thread>

namespace QC {

/// Manages the ADPL port: opens transport, runs inbound-only RX thread,
/// writes raw wire bytes to a .adplvN file. The wire format is already a
/// stream of ADPL frames (header + payload + padding, per the negotiated
/// version), so no re-encoding is needed.
class Adpl {
public:
    Adpl() = default;
    ~Adpl();

    // Non-copyable
    Adpl(const Adpl&) = delete;
    Adpl& operator=(const Adpl&) = delete;

    /// Open the ADPL transport and output file.
    /// port_name is the ADPL device name from the composite device's protocols;
    /// output_path is the directory for the .adplvN output file;
    /// adpl_version selects only the output file's extension (raw bytes are
    /// written verbatim regardless of version).
    /// Returns true if transport and file opened successfully.
    bool open(const std::string& port_name, const std::string& output_path,
              uint32_t baud_rate, uint8_t adpl_version);

    /// Start the ADPL RX thread.
    void start();

    /// Reopen the ADPL port after a device reboot (recovery), continuing
    /// capture into the SAME .adplvN file. Stops the RX thread, reopens the
    /// transport, then restarts the RX thread. Writer is left open. Matches
    /// Qdss::reopen so ADPL and QDSS share a single continuous capture across
    /// a reboot.
    bool reopen();

    /// Stop the RX thread and close transport/writer.
    void stop();

    /// Whether the manager is currently running.
    bool is_running() const { return running_.load(); }

    /// Total bytes captured so far.
    size_t bytes_captured() const { return writer_.bytes_written(); }

    /// Path to the ADPL output file.
    const std::string& output_file() const { return writer_.path(); }

private:
    std::unique_ptr<Transport> transport_;
    BinaryWriter writer_;
    std::thread rx_thread_;
    std::atomic<bool> running_{false};

    /// RX loop: reads from ADPL transport, writes raw bytes to .adplvN file.
    void rx_loop();
};

} // namespace QC

#endif // DIAGLOGGER_ADPL_H
