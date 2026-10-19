// Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
// SPDX-License-Identifier: BSD 3-Clause Clear License
//
// Adpl: ADPL (\DPL) capture protocol layer. An inbound-only Protocol and a
// structural mirror of Qdss — same BinaryWriter, same receiver-loop model,
// same recovery semantics. The wire is already a stream of ADPL frames
// (header + payload + padding, per the negotiated version), so raw bytes go
// straight to a .adplvN file with no decode and no re-encode. ADPL is
// enabled/disabled over the diag port, not here.

#ifndef QC_PROTOCOL_ADPL_H
#define QC_PROTOCOL_ADPL_H

#include "types.h"
#include "file_writer/binary_writer.h"
#include "protocol/protocol.h"
#include "transport/transport.h"

#include <chrono>
#include <cstdint>
#include <memory>
#include <string>

namespace QC {

struct AdplConfig {
    /// ADPL device name from the composite device's protocols, WITHOUT the
    /// \DPL stream suffix — Adpl appends that itself.
    std::string port_name;
    /// Directory the timestamped .adplvN capture file is created in.
    std::string output_path;
    uint32_t    baud_rate = 115200;
    /// Negotiated ADPL protocol version. Selects only the output file's
    /// extension — raw bytes are written verbatim regardless of version.
    uint8_t     adpl_version = 0;
};

/// Drives the ADPL port: opens the \DPL stream, runs the inbound-only
/// receiver loop, and writes raw wire bytes to a .adplvN file.
class Adpl : public Protocol {
public:
    /// Build with the default QcDevice transport for the \DPL stream of
    /// config.port_name.
    explicit Adpl(const AdplConfig& config);

    /// Build over a caller-supplied transport (tests, alternate links).
    Adpl(const AdplConfig& config, std::unique_ptr<Transport> transport);

    ~Adpl() override;

    // Non-copyable
    Adpl(const Adpl&) = delete;
    Adpl& operator=(const Adpl&) = delete;

    /// Open the output file (once per session) and the ADPL port, then wait
    /// for the capture threads to come up. Returns true once capturing.
    bool start();

    /// Close the port, join the capture threads, then close the output file.
    void stop();

    /// Whether the ADPL port is currently open and capturing.
    bool is_running() const { return is_open(); }

    /// Total bytes captured so far. Valid after stop() too.
    size_t bytes_captured() const { return writer_.bytes_written(); }

    /// Path to the ADPL output file.
    const std::string& output_file() const { return writer_.path(); }

    /// Inbound-only: the \DPL endpoint never carries requests. Always returns
    /// an empty buffer.
    ByteBuffer send_and_wait(const ByteBuffer& payload,
                             uint32_t timeout_ms = 0) override;

protected:
    /// Opens the port and starts the base I/O threads, with ADPL logging.
    bool lifecycle_open() override;

    /// Drains raw received bytes into the .adplvN file. No decoding — the
    /// wire format is written verbatim.
    void receiver_loop() override;

private:
    static constexpr std::chrono::seconds OPEN_TIMEOUT{10};
    static constexpr std::chrono::seconds CLOSE_TIMEOUT{5};
    static constexpr uint32_t RX_POLL_TIMEOUT_MS = 100;

    AdplConfig   config_;
    BinaryWriter writer_;

    /// Build the timestamped .adplvN path under config_.output_path.
    std::string build_output_path() const;
};

} // namespace QC

#endif // QC_PROTOCOL_ADPL_H
