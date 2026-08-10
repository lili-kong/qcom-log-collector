// Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
// SPDX-License-Identifier: BSD 3-Clause Clear License
//
// Qdss: manages QDSS port transport, RX thread, and binary file output.
// Owns the QDSS port lifecycle independently from DiagLogger's diag port.

#ifndef DIAGLOGGER_QDSS_H
#define DIAGLOGGER_QDSS_H

#include "file_writer/binary_writer.h"
#include "transport/transport.h"

#include <atomic>
#include <cstdint>
#include <memory>
#include <string>
#include <thread>

namespace QC {

/// Manages the QDSS port: opens transport, runs inbound-only RX thread,
/// writes raw binary data to .bin file.
class Qdss {
public:
    Qdss() = default;
    ~Qdss();

    // Non-copyable
    Qdss(const Qdss&) = delete;
    Qdss& operator=(const Qdss&) = delete;

    /// Open the QDSS transport and binary output file.
    /// port_name is the QDSS device name from the composite device's protocols;
    /// output_path is the directory for the .bin output file.
    /// Returns true if transport and file opened successfully.
    bool open(const std::string& port_name, const std::string& output_path,
              uint32_t baud_rate);

    /// Start the QDSS RX thread (inbound-only, no TX).
    void start();

    /// Reopen the QDSS port after a device reboot (recovery), continuing capture
    /// into the SAME .bin file. Stops the RX thread, reopens the transport, then
    /// restarts the RX thread. The output file/writer is left open. Returns true
    /// on success. Matches TUF, which reopens the QDSS I/O and reconfigures rather
    /// than creating a new capture file per reconnect.
    bool reopen();

    /// Stop the RX thread and close transport/writer.
    void stop();

    /// Whether the manager is currently running.
    bool is_running() const { return running_.load(); }

    /// Total bytes captured so far.
    size_t bytes_captured() const { return writer_.bytes_written(); }

    /// Path to the .bin output file.
    const std::string& output_file() const { return writer_.path(); }

private:
    std::unique_ptr<Transport> transport_;
    BinaryWriter writer_;
    std::thread rx_thread_;
    std::atomic<bool> running_{false};

    /// RX loop: reads from QDSS transport, writes raw bytes to .bin file.
    void rx_loop();
};

} // namespace QC

#endif // DIAGLOGGER_QDSS_H
