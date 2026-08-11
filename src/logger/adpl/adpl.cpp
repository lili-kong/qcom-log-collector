// Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
// SPDX-License-Identifier: BSD 3-Clause Clear License

#include "adpl.h"
#include "diag/adpl_commands.h"
#include "transport/transport_factory.h"

#include <KL/Macros.h>
#include <chrono>
#include <cstdio>
#include <ctime>
#include <vector>

#ifdef _WIN32
#include <process.h>
#define getpid _getpid
#else
#include <unistd.h>
#endif

namespace QC {

namespace {
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

Adpl::~Adpl() {
    stop();
}

bool Adpl::open(const std::string& port_name, const std::string& output_path,
                uint32_t baud_rate, uint8_t adpl_version) {
    // Open the ADPL interface's "\DPL" USB stream. The bare device name opens
    // the interface's default stream, which is the wrong endpoint. Matches TUF
    // Usb::setUsbStream, which opens description + "\DPL" for the ADPL port.
    transport_ = create_transport(port_name, baud_rate, ADPL_DPL_STREAM);
    if (!transport_) {
        CFLOG_ERROR("Adpl: Failed to create transport for: " + port_name, true);
        return false;
    }

    if (!transport_->open()) {
        CFLOG_ERROR("Adpl: Failed to open ADPL port: "
                    + port_name + ADPL_DPL_STREAM, true);
        transport_.reset();
        return false;
    }

    // Build timestamped output file path:  <output_path>/qlc_adpl_<timestamp>__PID_<pid>.adplv<N>
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
             extension_for_version(adpl_version));

    std::string path = output_path;
    if (!path.empty() && path.back() != '/' && path.back() != '\\') {
        path += '/';
    }
    path += filename;

    if (!writer_.open(path)) {
        CFLOG_ERROR("Adpl: Failed to open output file: " + path, true);
        transport_->close();
        transport_.reset();
        return false;
    }

    CFLOG_INFO("Adpl: Opened ADPL port [" + port_name + ADPL_DPL_STREAM
               + "], writing to: " + path, true);
    return true;
}

void Adpl::start() {
    if (running_.load()) {
        return;
    }

    running_ = true;
    rx_thread_ = std::thread(&Adpl::rx_loop, this);
}

void Adpl::stop() {
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

    CFLOG_INFO("Adpl: Stopped. Total bytes captured: " +
               std::to_string(writer_.bytes_written()), true);
}

bool Adpl::reopen() {
    // Stop the RX thread first so it isn't reading transport_ while we reopen
    // the port. rx_loop uses a short read timeout, so the join returns promptly.
    if (running_.load()) {
        running_ = false;
        if (rx_thread_.joinable()) {
            rx_thread_.join();
        }
    }

    if (!transport_) {
        CFLOG_ERROR("Adpl: reopen called with no transport.", true);
        return false;
    }

    if (!transport_->reopen()) {
        CFLOG_ERROR("Adpl: Failed to reopen ADPL port for recovery.", true);
        return false;
    }

    // Resume capturing into the SAME .adplvN file — writer_ is left open, so
    // the whole session lands in one continuous file across a reboot.
    running_ = true;
    rx_thread_ = std::thread(&Adpl::rx_loop, this);
    CFLOG_INFO("Adpl: Reopened ADPL port, continuing capture to " + writer_.path(), true);
    return true;
}

// ----------------------------------------------------------------------------
// rx_loop
//
// Inbound-only read loop for the ADPL port. The \DPL wire is already a stream
// of ADPL frames (header + payload + padding, per the negotiated version), so
// raw bytes are written straight to the .adplvN file — no decode, no re-encode.
// ----------------------------------------------------------------------------
void Adpl::rx_loop() {
    static constexpr size_t RX_BUF_SIZE = 128 * 1024;  // Match diag/qdss rx buffer size
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
