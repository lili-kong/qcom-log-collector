// Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
// SPDX-License-Identifier: BSD 3-Clause Clear License
//
// Qdss: QDSS trace-capture protocol layer. An inbound-only Protocol — the
// \TRACE endpoint is read-only, so there is no framing, no handshake and no
// request/response pairing. The receiver loop writes raw wire bytes straight
// to a .bin file. Diag-over-QDSS configuration travels on the diag port
// instead (see diag_logger_qdss_manager.cpp), not here.

#ifndef QC_PROTOCOL_QDSS_H
#define QC_PROTOCOL_QDSS_H

#include "types.h"
#include "file_writer/binary_writer.h"
#include "protocol/protocol.h"
#include "transport/transport.h"

#include <chrono>
#include <cstdint>
#include <memory>
#include <string>

namespace QC {

struct QdssConfig {
    /// QDSS device name from the composite device's protocols, WITHOUT the
    /// \TRACE stream suffix — Qdss appends that itself.
    std::string port_name;
    /// Directory the timestamped .bin capture file is created in.
    std::string output_path;
    uint32_t    baud_rate = 115200;
};

/// Drives the QDSS trace port: opens the \TRACE stream, runs the inbound-only
/// receiver loop, and writes raw binary data to a .bin file.
class Qdss : public Protocol {
public:
    /// Build with the default QcDevice transport for the \TRACE stream of
    /// config.port_name.
    explicit Qdss(const QdssConfig& config);

    /// Build over a caller-supplied transport (tests, alternate links).
    Qdss(const QdssConfig& config, std::unique_ptr<Transport> transport);

    ~Qdss() override;

    // Non-copyable
    Qdss(const Qdss&) = delete;
    Qdss& operator=(const Qdss&) = delete;

    /// Open the output file (once per session) and the QDSS port, then wait
    /// for the capture threads to come up. Returns true once capturing.
    bool start();

    /// Close the port, join the capture threads, then close the output file.
    void stop();

    /// Whether the QDSS port is currently open and capturing.
    bool is_running() const { return is_open(); }

    /// Total bytes captured so far. Valid after stop() too.
    size_t bytes_captured() const { return writer_.bytes_written(); }

    /// Path to the .bin output file.
    const std::string& output_file() const { return writer_.path(); }

    /// Inbound-only: the \TRACE endpoint never carries requests. Always
    /// returns an empty buffer.
    ByteBuffer send_and_wait(const ByteBuffer& payload,
                             uint32_t timeout_ms = 0) override;

protected:
    /// Opens the port and starts the base I/O threads, with QDSS logging.
    bool lifecycle_open() override;

    /// Drains raw received bytes into the .bin file. No decoding — QDSS data
    /// is raw binary off the wire.
    void receiver_loop() override;

private:
    static constexpr std::chrono::seconds OPEN_TIMEOUT{10};
    static constexpr std::chrono::seconds CLOSE_TIMEOUT{5};
    static constexpr uint32_t RX_POLL_TIMEOUT_MS = 100;

    QdssConfig   config_;
    BinaryWriter writer_;

    /// Build the timestamped .bin path under config_.output_path.
    std::string build_output_path() const;
};

} // namespace QC

#endif // QC_PROTOCOL_QDSS_H
