// Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
// SPDX-License-Identifier: BSD 3-Clause Clear License

#include "adpl.h"
#include "diag/adpl_commands.h"
#include "transport/transport_factory.h"

#include <KL/Macros.h>
#include <chrono>
#include <cstdio>
#include <ctime>
#include <utility>
#include <vector>

#ifdef _WIN32
#include <process.h>
#define getpid _getpid
#else
#include <unistd.h>
#endif

namespace QC {

namespace {
// USB stream suffix selecting the data-protocol-logging endpoint on the ADPL
// interface. Non-Windows QcDevice backends address the interface directly, with
// no stream component in the device path.
#ifdef TOOLS_TARGET_WINDOWS
constexpr const char* ADPL_DPL_STREAM = "\\DPL";
#else
constexpr const char* ADPL_DPL_STREAM = "";
#endif

// The bare device name opens the interface's default stream, which is the
// wrong endpoint. Matches TUF Usb::setUsbStream, which opens
// description + "\DPL" for the ADPL port.
std::string dpl_device_name(const std::string& port_name) {
    return port_name + ADPL_DPL_STREAM;
}

// File extension per ADPL protocol version.
const char* extension_for_version(uint8_t version) {
    switch (version) {
        case AdplVersion::VERSION1: return ".adpl";
        case AdplVersion::VERSION2: return ".adplv2";
        case AdplVersion::VERSION3: return ".adplv3";
        case AdplVersion::VERSION4: return ".adplv4";
        case AdplVersion::VERSION5: return ".adplv5";
        case AdplVersion::VERSION6: return ".adplv6";
        case AdplVersion::VERSION7: return ".adplv7";
        default: return ".adpl";
    }
}
} // namespace

Adpl::Adpl(const AdplConfig& config)
    : Adpl(config, create_transport(dpl_device_name(config.port_name),
                                    config.baud_rate))
{
}

Adpl::Adpl(const AdplConfig& config, std::unique_ptr<Transport> transport)
    : Protocol(std::move(transport))
    , config_(config)
{
}

Adpl::~Adpl() {
    // Drive shutdown through the lifecycle worker while our vtable is still
    // valid, then close the capture file. See Protocol::stop().
    stop();
}

ByteBuffer Adpl::send_and_wait(const ByteBuffer& /*payload*/,
                               uint32_t /*timeout_ms*/) {
    // The \DPL endpoint is inbound-only. ADPL is enabled and disabled over the
    // diag port instead, so there is nothing to send here.
    CFLOG_WARNING("Adpl: send_and_wait called on an inbound-only protocol.", true);
    return {};
}

std::string Adpl::build_output_path() const {
    // <output_path>/qlc_adpl_<timestamp>__PID_<pid>.adplvN
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
    char filename[160];
    snprintf(filename, sizeof(filename),
             "qlc_adpl_%02d_%02d_%04d_%02d_%02d_%02d_%03d__PID_%d%s",
             tm_now.tm_mday, tm_now.tm_mon + 1, tm_now.tm_year + 1900,
             tm_now.tm_hour, tm_now.tm_min, tm_now.tm_sec,
             static_cast<int>(ms.count()), static_cast<int>(getpid()),
             extension_for_version(config_.adpl_version));

    std::string path = config_.output_path;
    if (!path.empty() && path.back() != '/' && path.back() != '\\') {
        path += '/';
    }
    path += filename;
    return path;
}

bool Adpl::start() {
    // Open the capture file before the port, so no received byte can arrive
    // with nowhere to go. Created once per session and left open across a
    // reconnect, so the whole session lands in one continuous .adplvN.
    if (!writer_.is_open()) {
        const std::string path = build_output_path();
        if (!writer_.open(path)) {
            CFLOG_ERROR("Adpl: Failed to open output file: " + path, true);
            return false;
        }
    }

    if (is_open()) {
       request_open();
    }
    if (!wait_ready(OPEN_TIMEOUT)) {
        CFLOG_ERROR("Adpl: Failed to open ADPL port: " + description(), true);
        request_close();
        wait_closed(CLOSE_TIMEOUT);
        writer_.close();
        return false;
    }

    CFLOG_INFO("Adpl: Opened ADPL port [" + description()
               + "], writing to: " + writer_.path(), true);
    return true;
}

void Adpl::stop() {
    const bool was_open = is_open();

    // Closes the port and joins the I/O threads (including receiver_loop),
    // so nothing can write to writer_ after this returns.
    Protocol::stop(CLOSE_TIMEOUT);
    writer_.close();

    if (was_open) {
        CFLOG_INFO("Adpl: Stopped. Total bytes captured: " +
                   std::to_string(writer_.bytes_written()), true);
    }
}

bool Adpl::lifecycle_open() {
    if (!Protocol::lifecycle_open()) {
        CFLOG_ERROR("Adpl: Failed to open ADPL port: " + description(), true);
        return false;
    }
    // Nothing to handshake — an open \DPL stream is immediately usable, and
    // the base has already marked STATE_AVAILABLE.
    return true;
}

// ----------------------------------------------------------------------------
// receiver_loop
//
// Inbound-only capture loop for the ADPL port. The \DPL wire is already a
// stream of ADPL frames (header + payload + padding, per the negotiated
// version), so raw bytes go straight to the .adplvN file — no decode, no
// re-encode.
// ----------------------------------------------------------------------------
void Adpl::receiver_loop() {
    while (true) {
        ByteBuffer raw = next_rx(RX_POLL_TIMEOUT_MS);
        if (raw.empty()) {
            // Only exit once the queue is drained: the base has stopped the
            // RX producer (io_running_ cleared) AND next_rx() returned
            // nothing, so no captured bytes are dropped on shutdown.
            if (!io_running_.load()) {
                break;
            }
            continue;
        }

        writer_.write(raw.data(), raw.size());
    }
}

} // namespace QC
