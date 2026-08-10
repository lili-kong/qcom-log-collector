// Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
// SPDX-License-Identifier: BSD 3-Clause Clear License
//
// QcDevice USB transport implementation.
//
// Uses QcDevice API for all device operations:
//   - TransportManager for discovery and monitor lifecycle
//   - OpenDevice for opening
//   - ReadFromDevice for reading (in RX worker thread)
//   - SendToDevice for writing (with retry loop)
//   - CancelIoEx + CloseDevice for closing
//
// Matches TUF's Usb.cpp pattern (Device/Communication/Usb.cpp).

#include <algorithm>
#include <chrono>
#include <cstring>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <thread>
#include <KL/Macros.h>

#include "qcdevice_transport.h"
#include "transport_manager.h"
#include "qdpublic.h"

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

// Convert a device handle to an integer for diagnostic logging. HANDLE is a
// pointer type on Windows but an integral fd-like type on Linux, so the cast
// required differs per platform.
static uintptr_t handle_to_uint(HANDLE handle) {
#ifdef TOOLS_TARGET_WINDOWS
    return reinterpret_cast<uintptr_t>(handle);
#else
    return static_cast<uintptr_t>(handle);
#endif
}

namespace QC {

QcDeviceTransport::QcDeviceTransport(const std::string& device_name,
                                     uint32_t baud_rate,
                                     const std::string& usb_stream)
    : device_name_(device_name)
    , usb_stream_(usb_stream)
    , baud_rate_(baud_rate)
    , ring_(RING_BUFFER_SIZE)
{
}

QcDeviceTransport::~QcDeviceTransport() {
    close();
}

bool QcDeviceTransport::is_handle_valid() const {
#ifdef TOOLS_TARGET_WINDOWS
    return handle_ != INVALID_HANDLE_VALUE;
#else
    return handle_ != static_cast<HANDLE>(-1);
#endif
}

// Close the device handle — matches TUF Usb::closeHandle() pattern:
// 1. Set handle to INVALID (so RX worker breaks out of its loop)
// 2. CancelIoEx to unblock any pending overlapped I/O (Windows only)
// 3. QcDevice::CloseDevice to close the handle
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
    // Append the stream suffix (if any) only for the open path.  device_name_
    // stays the bare DevDesc so resolve_device_name / extract_com_port and the
    // reopen-time matching above continue to work.  Mirrors TUF's
    // Usb::open(), which opens m_description + m_stream (Usb.cpp:326).
    std::wstring wide_name = TransportManager::to_wide(device_name_ + usb_stream_);

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
                          "*** Close any other tool using this port. ***"
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
    std::string open_name = device_name_ + usb_stream_;
    handle_ = QcDevice::OpenDevice(
        const_cast<char*>(open_name.c_str()));
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
            const_cast<char*>(open_name.c_str()));
#endif
    }

    if (!is_handle_valid()) {
        CFLOG_ERROR("QcDevice: Failed to open device: "
                    + device_name_ + usb_stream_, true);
        return false;
    }

    {
        std::ostringstream ss;
        ss << "QcDevice: Opened device: " << device_name_ << usb_stream_
           << " (handle=0x" << std::hex << handle_to_uint(handle_)
           << std::dec << ")";
        CFLOG_INFO(ss.str(), true);
    }


    // Reset ring buffer state
    {
        std::lock_guard<std::mutex> lock(ring_mutex_);
        ring_head_ = 0;
        ring_tail_ = 0;
    }

    // Capture device match pattern for recovery (strip COM port number,
    // keep the base description so we can re-find the device if the COM
    // port number changes after reboot).
    if (device_match_pattern_.empty()) {
        auto paren = device_name_.rfind('(');
        if (paren != std::string::npos && paren > 0) {
            device_match_pattern_ = device_name_.substr(0, paren);
        } else {
            device_match_pattern_ = device_name_;
        }
        FLOG_DEBUG("QcDevice: Match pattern for recovery: \""
                  + device_match_pattern_ + "\"");
    }

    // Launch RX worker thread (matches TUF starting RxWorker after open)
    state_.store(TransportState::CONNECTED);
    rx_worker_ = std::thread(&QcDeviceTransport::rx_worker_func, this);

    return true;
}

void QcDeviceTransport::close() {
    auto prev = state_.exchange(TransportState::CLOSED);
    if (prev == TransportState::CLOSED) {
        close_handle();
        return;
    }

    // Close handle first — this causes the RX worker loop to break
    close_handle();

    // Wake any waiting reader
    ring_cv_.notify_all();

    // Join the worker thread
    if (rx_worker_.joinable()) {
        rx_worker_.join();
    }
}

bool QcDeviceTransport::is_open() const {
    return state_.load() == TransportState::CONNECTED;
}

bool QcDeviceTransport::is_disconnected() const {
    return state_.load() == TransportState::DISCONNECTED;
}

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

size_t QcDeviceTransport::read(uint8_t* buf, size_t max_len, uint32_t timeout_ms) {
    std::unique_lock<std::mutex> lock(ring_mutex_);

    // When disconnected and ring is empty, return immediately (don't block)
    if (state_.load() == TransportState::DISCONNECTED && ring_available() == 0) {
        return 0;
    }

    // Wait for data or timeout
    if (ring_available() == 0) {
        ring_cv_.wait_for(lock, std::chrono::milliseconds(timeout_ms),
                          [this] {
                              return ring_available() > 0 ||
                                     state_.load() != TransportState::CONNECTED;
                          });
    }

    return ring_pop(buf, max_len);
}

std::string QcDeviceTransport::description() const {
    return "QcDevice:" + device_name_;
}

// --- Ring buffer helpers ---

size_t QcDeviceTransport::ring_available() const {
    if (ring_head_ >= ring_tail_) {
        return ring_head_ - ring_tail_;
    }
    return RING_BUFFER_SIZE - ring_tail_ + ring_head_;
}

void QcDeviceTransport::ring_push(const uint8_t* data, size_t len) {
    size_t free = RING_BUFFER_SIZE - ring_available() - 1;
    if (len > free) {
        size_t drop = len - free;
        ring_tail_ = (ring_tail_ + drop) % RING_BUFFER_SIZE;
    }

    size_t first_chunk = (std::min)(len, RING_BUFFER_SIZE - ring_head_);
    std::memcpy(ring_.data() + ring_head_, data, first_chunk);
    if (len > first_chunk) {
        std::memcpy(ring_.data(), data + first_chunk, len - first_chunk);
    }
    ring_head_ = (ring_head_ + len) % RING_BUFFER_SIZE;
}

size_t QcDeviceTransport::ring_pop(uint8_t* buf, size_t max_len) {
    size_t avail = ring_available();
    if (avail == 0) return 0;

    size_t to_copy = (std::min)(avail, max_len);
    size_t first_chunk = (std::min)(to_copy, RING_BUFFER_SIZE - ring_tail_);
    std::memcpy(buf, ring_.data() + ring_tail_, first_chunk);
    if (to_copy > first_chunk) {
        std::memcpy(buf + first_chunk, ring_.data(), to_copy - first_chunk);
    }
    ring_tail_ = (ring_tail_ + to_copy) % RING_BUFFER_SIZE;
    return to_copy;
}

void QcDeviceTransport::drain_ring() {
    std::lock_guard<std::mutex> lock(ring_mutex_);
    ring_head_ = 0;
    ring_tail_ = 0;
    ring_cv_.notify_all();
}

bool QcDeviceTransport::reopen() {
    // Join the old RX worker if still running
    if (rx_worker_.joinable()) {
        rx_worker_.join();
    }

    // Close the old device handle (may already be closed)
    close_handle();

    // Clear stale data from the ring buffer
    drain_ring();

    auto& mgr = TransportManager::instance();

#ifdef TOOLS_TARGET_WINDOWS
    // Re-enumerate devices and find our device by match pattern
    std::wstring wide_name;
    bool found_match = false;

    {
        auto devices = mgr.enumerate_devices();
        for (const auto& dev : devices) {
            if (dev.device_name.find(device_match_pattern_) != std::string::npos) {
                found_match = true;
                // Match on the bare DevDesc, then re-append the stream suffix
                // for the open call (see open()).
                wide_name = TransportManager::to_wide(dev.device_name + usb_stream_);
                device_name_ = dev.device_name;
                CFLOG_WARNING("QcDevice: reopen: Matched device \"" + dev.device_name + "\"", true);
                break;
            }
        }
    }

    if (!found_match) {
        CFLOG_WARNING("QcDevice: reopen: Device not found (pattern=\""
                  + device_match_pattern_ + "\")", true);
        return false;
    }

    // Open the matched device
    handle_ = QcDevice::OpenDevice(
        const_cast<wchar_t*>(wide_name.c_str()),
        static_cast<DWORD>(baud_rate_));
#else
    std::string open_name = device_name_ + usb_stream_;
    handle_ = QcDevice::OpenDevice(
        const_cast<char*>(open_name.c_str()));
    bool found_match = is_handle_valid();
#endif

    for (int retry = 0; !is_handle_valid() && retry < OPEN_RETRY_COUNT; ++retry) {
        CFLOG_WARNING("QcDevice: reopen: Open failed, retrying (" + std::to_string(retry + 1)
                  + "/" + std::to_string(OPEN_RETRY_COUNT) + ")...", true);
        std::this_thread::sleep_for(
            std::chrono::milliseconds(OPEN_RETRY_DELAY_MS));
#ifdef TOOLS_TARGET_WINDOWS
        handle_ = QcDevice::OpenDevice(
            const_cast<wchar_t*>(wide_name.c_str()),
            static_cast<DWORD>(baud_rate_));
#else
        handle_ = QcDevice::OpenDevice(
            const_cast<char*>(open_name.c_str()));
#endif
    }

    if (!is_handle_valid()) {
        CFLOG_ERROR("QcDevice: reopen: Failed to open device", true);
        return false;
    }

    {
        std::ostringstream ss;
        ss << "QcDevice: reopen: Opened device: " << device_name_
           << " (handle=0x" << std::hex << handle_to_uint(handle_)
           << std::dec << ")";
        CFLOG_INFO(ss.str(), true);
    }

    // Set state to CONNECTED and spawn new RX worker
    state_.store(TransportState::CONNECTED);
    rx_worker_ = std::thread(&QcDeviceTransport::rx_worker_func, this);

    return true;
}

// --- RX worker thread ---

void QcDeviceTransport::rx_worker_func() {
    std::vector<uint8_t> buf(RX_BUFFER_SIZE);
    uint64_t iteration = 0;
    uint64_t zero_count = 0;
    uint64_t total_bytes_received = 0;

    {
        std::ostringstream ss;
        ss << "QcDevice: RX worker started (handle=0x"
           << std::hex << handle_to_uint(handle_)
           << std::dec << ")";
        FLOG_DEBUG(ss.str());
    }

    while (state_.load() == TransportState::CONNECTED && is_handle_valid()) {
        DWORD bytes_read = 0;
        bool ok = false;

        try {
            ok = !!QcDevice::ReadFromDevice(
                handle_,
                buf.data(),
                static_cast<DWORD>(RX_BUFFER_SIZE),
                &bytes_read);
        } catch (...) {
            ok = false;
        }

        ++iteration;

        if (!ok) {
            if (state_.load() == TransportState::CONNECTED) {
                // Only log error if this was an unexpected disconnect (not intentional shutdown)
                CFLOG_ERROR("QcDevice: ReadFromDevice FAILED at iteration "
                          + std::to_string(iteration) + " (total_rx=" + std::to_string(total_bytes_received)
                          + " zero_reads=" + std::to_string(zero_count) + "), device disconnected", true);
                state_.store(TransportState::DISCONNECTED);
            }
            close_handle();
            ring_cv_.notify_all();
            break;
        }

        if (bytes_read > 0 && bytes_read <= RX_BUFFER_SIZE) {
            total_bytes_received += bytes_read;
            if (verbose_) {
                FLOG_DEBUG("QcDevice: RX " + std::to_string(bytes_read) + " bytes at iter "
                          + std::to_string(iteration) + ": ["
                          + hex_dump(buf.data(), bytes_read, 48) + "]");
            }
            std::lock_guard<std::mutex> lock(ring_mutex_);
            ring_push(buf.data(), static_cast<size_t>(bytes_read));
            ring_cv_.notify_one();
        } else {
            ++zero_count;
            if (verbose_ && (zero_count <= 5 || (zero_count % 50 == 0))) {
                FLOG_DEBUG("QcDevice: ReadFromDevice returned 0 bytes"
                          " (ok=" + std::to_string(ok) + " iter=" + std::to_string(iteration)
                          + " zero_count=" + std::to_string(zero_count) + ")");
            }
        }
    }

    FLOG_DEBUG("QcDevice: RX worker exiting (iterations=" + std::to_string(iteration)
              + " total_rx=" + std::to_string(total_bytes_received)
              + " zero_reads=" + std::to_string(zero_count) + ")");
}

} // namespace QC
