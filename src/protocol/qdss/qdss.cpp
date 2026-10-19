// Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
// SPDX-License-Identifier: BSD 3-Clause Clear License

#include "qdss.h"
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
// USB stream suffix selecting the trace endpoint on the QDSS interface.
// Non-Windows QcDevice backends address the interface directly, with no stream
// component in the device path.
#ifdef TOOLS_TARGET_WINDOWS
constexpr const char* QDSS_TRACE_STREAM = "\\TRACE";
#else
constexpr const char* QDSS_TRACE_STREAM = "";
#endif

// The QDSS USB interface requires the "\TRACE" stream suffix on Windows to
// reach the trace endpoint — opening the bare device name yields the
// interface's default stream instead. QDS passes the suffix through untouched
// for names containing "QDSS" (QDS_WIN/scandev.cpp), so the final path becomes
// \\.\<DevDesc>\TRACE. Matches TUF Device/Manager.cpp:1984.
std::string trace_device_name(const std::string& port_name) {
    return port_name + QDSS_TRACE_STREAM;
}
} // namespace

Qdss::Qdss(const QdssConfig& config)
    : Qdss(config, create_transport(trace_device_name(config.port_name),
                                    config.baud_rate))
{
}

Qdss::Qdss(const QdssConfig& config, std::unique_ptr<Transport> transport)
    : Protocol(std::move(transport))
    , config_(config)
{
}

Qdss::~Qdss() {
    // Drive shutdown through the lifecycle worker while our vtable is still
    // valid, then close the capture file. See Protocol::stop().
    stop();
}

ByteBuffer Qdss::send_and_wait(const ByteBuffer& /*payload*/,
                               uint32_t /*timeout_ms*/) {
    // The \TRACE endpoint is inbound-only. QDSS is configured over the diag
    // port instead (see diag_logger_qdss_manager.cpp), so there is nothing to
    // send here.
    CFLOG_WARNING("Qdss: send_and_wait called on an inbound-only protocol.", true);
    return {};
}

std::string Qdss::build_output_path() const {
    // <output_path>/qlc_qdss_<timestamp>__PID_<pid>.bin
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
             "qlc_qdss_%02d_%02d_%04d_%02d_%02d_%02d_%03d__PID_%d.bin",
             tm_now.tm_mday, tm_now.tm_mon + 1, tm_now.tm_year + 1900,
             tm_now.tm_hour, tm_now.tm_min, tm_now.tm_sec,
             static_cast<int>(ms.count()), static_cast<int>(getpid()));

    std::string path = config_.output_path;
    if (!path.empty() && path.back() != '/' && path.back() != '\\') {
        path += '/';
    }
    path += filename;
    return path;
}

bool Qdss::start() {
    // Open the capture file before the port, so no received byte can arrive
    // with nowhere to go. Created once per session and left open across a
    // reconnect, so the whole session lands in one continuous .bin.
    if (!writer_.is_open()) {
        const std::string path = build_output_path();
        if (!writer_.open(path)) {
            CFLOG_ERROR("Qdss: Failed to open output file: " + path, true);
            return false;
        }
    }

    if (!is_open()) {
       request_open();
    }
    if (!wait_ready(OPEN_TIMEOUT)) {
        CFLOG_ERROR("Qdss: Failed to open QDSS port: " + description(), true);
        request_close();
        wait_closed(CLOSE_TIMEOUT);
        writer_.close();
        return false;
    }

    CFLOG_INFO("Qdss: Opened QDSS port [" + description()
               + "], writing to: " + writer_.path(), true);
    return true;
}

void Qdss::stop() {
    const bool was_open = is_open();

    // Closes the port and joins the I/O threads (including receiver_loop),
    // so nothing can write to writer_ after this returns.
    Protocol::stop(CLOSE_TIMEOUT);
    writer_.close();

    if (was_open) {
        CFLOG_INFO("Qdss: Stopped. Total bytes captured: " +
                   std::to_string(writer_.bytes_written()), true);
    }
}

bool Qdss::lifecycle_open() {
    if (!Protocol::lifecycle_open()) {
        CFLOG_ERROR("Qdss: Failed to open QDSS port: " + description(), true);
        return false;
    }
    // Nothing to handshake — an open \TRACE stream is immediately usable, and
    // the base has already marked STATE_AVAILABLE.
    return true;
}

// ----------------------------------------------------------------------------
// receiver_loop
//
// Inbound-only capture loop for the QDSS port. Takes raw bytes from the base
// RX pump and writes them straight to the .bin file. No HDLC decoding — QDSS
// data is raw binary off the wire.
// ----------------------------------------------------------------------------
void Qdss::receiver_loop() {
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
