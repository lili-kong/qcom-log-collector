// Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
// SPDX-License-Identifier: BSD 3-Clause Clear License
//
// QcDevice USB transport wrapping the QDS QcDevice API for device
// discovery, open, read, write, and close.
//

#include "qcdevice_transport.h"
#include "transport_manager.h"
#include "qdpublic.h"

#include <algorithm>
#include <chrono>
#include <cstring>
#include <iomanip>
#include <sstream>
#include <thread>
#include <KL/Macros.h>

// Format a hex dump of the first N bytes for diagnostic logging.
static std::string hex_dump(const uint8_t* data, size_t len, size_t max_bytes = 32) {
    std::ostringstream ss;
    size_t n = (std::min)(len, max_bytes);
    for (size_t i = 0; i < n; ++i) {
        if (i > 0) ss << ' ';
        ss << std::hex << std::setfill('0') << std::setw(2)
           << static_cast<int>(data[i]);
    }
    if (len > max_bytes) ss << " ...";
    return ss.str();
}

namespace QC {

QcDeviceTransport::QcDeviceTransport(const std::string& device_name,
                                     uint32_t baud_rate)
    : device_name_(device_name)
    , baud_rate_(baud_rate)
{
}

QcDeviceTransport::~QcDeviceTransport() {
    // The owning Protocol stops its I/O threads and calls close() before
    // destroying us; closing the handle here is a safety net.
    close_handle();
}

bool QcDeviceTransport::is_handle_valid() const {
#ifdef TOOLS_TARGET_WINDOWS
    return handle_ != INVALID_HANDLE_VALUE;
#else
    return handle_ != static_cast<HANDLE>(-1);
#endif
}

// Close the device handle. CancelIoEx unblocks any pending overlapped
// read from the Protocol's RX thread before we hand the handle to
// CloseDevice.
void QcDeviceTransport::close_handle() {
    if (!is_handle_valid()) {
        return;
    }

    HANDLE h = handle_;
#ifdef TOOLS_TARGET_WINDOWS
    handle_ = INVALID_HANDLE_VALUE;
    ::CancelIoEx(h, NULL);
#else
    handle_ = static_cast<HANDLE>(-1);
#endif
    QcDevice::CloseDevice(h);
}

bool QcDeviceTransport::open() {
    if (is_handle_valid()) {
        return true;  // Already open
    }

    auto& mgr = TransportManager::instance();

#ifdef TOOLS_TARGET_WINDOWS
    // Resolve the device name: OpenDevice matches against the full DevDesc
    // (e.g. "Qualcomm HS-USB Android DIAG 9091 (COM3)"), not bare COM port
    // names.  If the user passed a short name like "COM3", find the matching
    // DevDesc from GetDeviceList output.
    std::string resolved = mgr.resolve_device_name(device_name_);
    if (resolved != device_name_) {
        device_name_ = resolved;
    }
    std::wstring wide_name = TransportManager::to_wide(device_name_);

    // Check if the COM port is already in use by another process BEFORE
    // we open it.  COM ports opened with FILE_SHARE_READ|FILE_SHARE_WRITE
    // (as QcDevice::OpenDevice does) allow multiple handles, but only one
    // reader receives the data.
    {
        std::string com_port = TransportManager::extract_com_port(device_name_);
        if (!com_port.empty()) {
            if (TransportManager::is_com_port_in_use(com_port)) {
                CFLOG_WARNING("\n*** WARNING: " + com_port
                          + " is already open by another process! ***\n"
                          "*** Close QUTS, Device Manager, or any other tool"
                          " using this port. ***\n"
                          "*** While qualcomm-log-collector can open a shared handle,"
                          " responses will go to\n"
                          "*** the OTHER reader, causing loopback to fail."
                          " ***\n", true);
            } else {
                FLOG_DEBUG("QcDevice: " + com_port
                          + " is not in use by other processes (good)");
            }
        }
    }

    // Attempt to open with retries (matching TUF Usb::open pattern).
    handle_ = QcDevice::OpenDevice(
        const_cast<wchar_t*>(wide_name.c_str()),
        static_cast<DWORD>(baud_rate_));
#else
    handle_ = QcDevice::OpenDevice(
        const_cast<char*>(device_name_.c_str()));
#endif

    for (int retry = 0; !is_handle_valid() && retry < OPEN_RETRY_COUNT; ++retry) {
        CFLOG_WARNING("QcDevice: Open failed, retrying (" + std::to_string(retry + 1)
                  + "/" + std::to_string(OPEN_RETRY_COUNT) + ")...", true);
        std::this_thread::sleep_for(
            std::chrono::milliseconds(OPEN_RETRY_DELAY_MS));

#ifdef TOOLS_TARGET_WINDOWS
        handle_ = QcDevice::OpenDevice(
            const_cast<wchar_t*>(wide_name.c_str()),
            static_cast<DWORD>(baud_rate_));
#else
        handle_ = QcDevice::OpenDevice(
            const_cast<char*>(device_name_.c_str()));
#endif
    }

    if (!is_handle_valid()) {
        CFLOG_ERROR("QcDevice: Failed to open device: " + device_name_, true);
        return false;
    }

    {
        std::ostringstream ss;
        ss << "QcDevice: Opened device: " << device_name_
           << " (handle=0x" << std::hex << (uintptr_t)(handle_)
           << std::dec << ")";
        CFLOG_INFO(ss.str(), true);
    }

    state_.store(TransportState::CONNECTED);
    return true;
}

void QcDeviceTransport::close() {
    auto prev = state_.exchange(TransportState::CLOSED);
    if (prev == TransportState::CLOSED) {
        close_handle();
        return;
    }

    // Closing the handle unblocks any pending ReadFromDevice in the
    // Protocol's RX thread; the Protocol owns thread join.
    close_handle();
}

// The port exists in both CONNECTED and DISCONNECTED: an unexpected device
// loss leaves the port open-but-unavailable. Only an explicit close()
// clears this. (is_open/is_connected/is_disconnected are implemented by
// Transport based on state_ transitions this class performs.)

size_t QcDeviceTransport::write(const uint8_t* data, size_t len) {
    if (!is_handle_valid() || len == 0) {
        return 0;
    }

    if (verbose_) {
        FLOG_DEBUG("QcDevice: write " + std::to_string(len) + " bytes: ["
                  + hex_dump(data, len) + "]");
    }

    // Matches TUF Usb::sendSync() — retry loop with QcDevice::SendToDevice
    DWORD total_sent = 0;
    int attempts = 0;
    bool send_ok = true;

    while (send_ok && total_sent < static_cast<DWORD>(len)) {
        DWORD bytes_sent = 0;

        send_ok = !!QcDevice::SendToDevice(
            handle_,
            const_cast<uint8_t*>(data + total_sent),
            static_cast<DWORD>(len - total_sent),
            &bytes_sent);

        if (send_ok && bytes_sent > 0) {
            total_sent += bytes_sent;
            attempts = 0;
            if (verbose_) {
                FLOG_DEBUG("QcDevice: SendToDevice OK (sent=" + std::to_string(bytes_sent)
                          + " total=" + std::to_string(total_sent) + "/" + std::to_string(len) + ")");
            }
        } else {
            ++attempts;
            CFLOG_ERROR("QcDevice: SendToDevice failed (status=" + std::to_string(send_ok)
                      + " bytes=" + std::to_string(bytes_sent)
                      + " attempt=" + std::to_string(attempts)
                      + " total=" + std::to_string(total_sent) + "/" + std::to_string(len) + ")", true);

            if ((bytes_sent == 0 && total_sent == 0) ||
                attempts > MAX_SEND_RETRY) {
                break;
            }
        }
    }

    return static_cast<size_t>(total_sent);
}

// Single-shot device-level read. Blocks in QcDevice::ReadFromDevice until
// the device returns data or the handle is closed (CancelIoEx unblocks
// the pending read on shutdown). The timeout_ms parameter is ignored —
// QcDevice::ReadFromDevice has no native timeout; the Protocol's RX thread
// just re-enters this call in a loop and shutdown is signalled by
// close_handle().
size_t QcDeviceTransport::read(uint8_t* buf,
                               size_t max_len,
                               uint32_t /*timeout_ms*/) {
    if (!is_handle_valid() || max_len == 0) {
        return 0;
    }

    DWORD bytes_read = 0;
    bool ok = false;
    try {
        ok = !!QcDevice::ReadFromDevice(
            handle_,
            buf,
            static_cast<DWORD>(max_len),
            &bytes_read);
    } catch (...) {
        ok = false;
    }

    if (!ok) {
        if (state_.load() == TransportState::CONNECTED) {
            CFLOG_ERROR("QcDevice: ReadFromDevice FAILED, device disconnected", true);
            state_.store(TransportState::DISCONNECTED);
        }
        close_handle();
        return 0;
    }

    if (bytes_read > 0 && verbose_) {
        FLOG_DEBUG("QcDevice: RX " + std::to_string(bytes_read) + " bytes: ["
                  + hex_dump(buf, bytes_read, 48) + "]");
    }
    return static_cast<size_t>(bytes_read);
}

std::string QcDeviceTransport::description() const {
    return "QcDevice:" + device_name_;
}

} // namespace QC
