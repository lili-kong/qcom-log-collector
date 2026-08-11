// Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
// SPDX-License-Identifier: BSD 3-Clause Clear License

#include "qdss.h"
#include "transport/transport_factory.h"

#include <KL/Macros.h>
#include <chrono>
#include <cstdio>
#include <ctime>

#ifdef _WIN32
#include <process.h>
#define getpid _getpid
#else
#include <unistd.h>
#endif

namespace QC {

Qdss::~Qdss() {
    stop();
}

bool Qdss::open(const std::string& port_name, const std::string& output_path,
                uint32_t baud_rate) {
    // Create and open transport for the QDSS port.  The QDSS USB interface
    // requires the "\TRACE" stream suffix on Windows to reach the trace
    // endpoint — opening the bare device name yields the interface's default
    // stream instead.  QDS passes the suffix through untouched for names
    // containing "QDSS" (QDS_WIN/scandev.cpp), so the final path becomes
    // \\.\<DevDesc>\TRACE.  Matches TUF Device/Manager.cpp:1984.
    transport_ = create_transport(port_name, baud_rate, QDSS_TRACE_STREAM);
    if (!transport_) {
        CFLOG_ERROR("Qdss: Failed to create transport for: " + port_name, true);
        return false;
    }

    if (!transport_->open()) {
        CFLOG_ERROR("Qdss: Failed to open QDSS port: "
                    + port_name + QDSS_TRACE_STREAM, true);
        transport_.reset();
        return false;
    }

    // Build timestamped output file path: <output_path>/qlc_qdss_<timestamp>__PID_<pid>.bin
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

    std::string bin_path = output_path;
    if (!bin_path.empty() && bin_path.back() != '/' && bin_path.back() != '\\') {
        bin_path += '/';
    }
    bin_path += filename;

    if (!writer_.open(bin_path)) {
        CFLOG_ERROR("Qdss: Failed to open output file: " + bin_path, true);
        transport_->close();
        transport_.reset();
        return false;
    }

    CFLOG_INFO("Qdss: Opened QDSS port [" + port_name + QDSS_TRACE_STREAM
               + "], writing to: " + bin_path, true);
    return true;
}

void Qdss::start() {
    if (running_.load()) {
        return;
    }

    running_ = true;
    rx_thread_ = std::thread(&Qdss::rx_loop, this);
}

void Qdss::stop() {
    if (!running_.load()) {
        return;
    }

    running_ = false;

    if (rx_thread_.joinable()) {
        rx_thread_.join();
    }

    writer_.close();

    if (transport_) {
        transport_->close();
        transport_.reset();
    }

    CFLOG_INFO("Qdss: Stopped. Total bytes captured: " +
               std::to_string(writer_.bytes_written()), true);
}

bool Qdss::reopen() {
    // Stop the RX thread first so it isn't reading transport_ while we reopen the
    // port. rx_loop uses a short read timeout, so the join returns promptly.
    if (running_.load()) {
        running_ = false;
        if (rx_thread_.joinable()) {
            rx_thread_.join();
        }
    }

    if (!transport_) {
        CFLOG_ERROR("Qdss: reopen called with no transport.", true);
        return false;
    }

    if (!transport_->reopen()) {
        CFLOG_ERROR("Qdss: Failed to reopen QDSS port for recovery.", true);
        return false;
    }

    // Resume capturing into the SAME .bin file — writer_ is left open, so the
    // whole session lands in one continuous file across a reboot.
    running_ = true;
    rx_thread_ = std::thread(&Qdss::rx_loop, this);
    CFLOG_INFO("Qdss: Reopened QDSS port, continuing capture to " + writer_.path(), true);
    return true;
}

// ----------------------------------------------------------------------------
// rx_loop
//
// Inbound-only read loop for the QDSS port. Reads raw binary data from the
// transport and writes directly to the .bin file. No HDLC decoding — QDSS
// data is raw binary off the wire.
// ----------------------------------------------------------------------------
void Qdss::rx_loop() {
    static constexpr size_t RX_BUF_SIZE = 128 * 1024;  // Match diag rx_loop buffer size
    static constexpr uint32_t READ_TIMEOUT_MS = 100;

    std::vector<uint8_t> buf(RX_BUF_SIZE);

    while (running_.load()) {
        size_t n = transport_->read(buf.data(), buf.size(), READ_TIMEOUT_MS);
        if (n == 0) {
            continue;
        }

        writer_.write(buf.data(), n);
    }
}

} // namespace QC
