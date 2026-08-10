// Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
// SPDX-License-Identifier: BSD 3-Clause Clear License
//
// QcDevice USB transport wrapping the QDS QcDevice API for device
// discovery, open, read, write, and close.
//
// Follows the same pattern as TUF's Usb.cpp:
//   - QcDevice::OpenDevice for opening (with retries)
//   - QcDevice::ReadFromDevice in an RX worker thread
//   - QcDevice::SendToDevice for writes (with retry loop)
//   - CancelIoEx + QcDevice::CloseDevice for closing

#ifndef QC_QCDEVICE_TRANSPORT_H
#define QC_QCDEVICE_TRANSPORT_H

#include <atomic>
#include <condition_variable>
#include <mutex>
#include <thread>
#include <vector>

#ifdef TOOLS_TARGET_WINDOWS
#include <windows.h>
#else
#include <cstdint>
typedef long int HANDLE;
#endif

#include "transport.h"

namespace QC {

class QcDeviceTransport : public Transport {
public:
    /// Construct with a QcDevice device name (the DevDesc string from
    /// QcDevice::GetDeviceList) and optional baud rate.
    ///
    /// usb_stream is an optional stream suffix appended to the device name at
    /// open time for interfaces whose USB driver requires selecting a stream
    /// (e.g. "\\TRACE" for QDSS, "\\DPL" for ADPL).  It is deliberately kept
    /// separate from device_name_ so device discovery/matching still works
    /// against the bare DevDesc.  Matches TUF's Usb::setUsbStream
    /// (Device/Communication/Usb.cpp), which opens m_description + m_stream.
    explicit QcDeviceTransport(const std::string& device_name,
                               uint32_t baud_rate = 38400,
                               const std::string& usb_stream = std::string());
    ~QcDeviceTransport() override;

    // Non-copyable
    QcDeviceTransport(const QcDeviceTransport&) = delete;
    QcDeviceTransport& operator=(const QcDeviceTransport&) = delete;

    bool open() override;
    void close() override;
    bool is_open() const override;
    size_t write(const uint8_t* data, size_t len) override;
    size_t read(uint8_t* buf, size_t max_len, uint32_t timeout_ms) override;
    std::string description() const override;
    bool is_disconnected() const override;
    bool reopen() override;

private:
    static constexpr size_t RX_BUFFER_SIZE   = 128 * 1024;   // 128KB per read
    static constexpr size_t RING_BUFFER_SIZE = 512 * 1024;   // 512KB ring buffer
    static constexpr int    OPEN_RETRY_COUNT = 8;
    static constexpr int    OPEN_RETRY_DELAY_MS = 250;
    static constexpr int    MAX_SEND_RETRY = 5;

    std::string device_name_;
    std::string usb_stream_;
    uint32_t baud_rate_;

#ifdef TOOLS_TARGET_WINDOWS
    HANDLE handle_ = INVALID_HANDLE_VALUE;
#else
    HANDLE handle_ = -1;
#endif

    // Ring buffer members
    std::vector<uint8_t> ring_;
    size_t ring_head_ = 0;   // write position (producer)
    size_t ring_tail_ = 0;   // read position (consumer)
    std::mutex ring_mutex_;
    std::condition_variable ring_cv_;

    // RX worker thread
    std::atomic<TransportState> state_{TransportState::CLOSED};
    std::thread rx_worker_;
    void rx_worker_func();

    // Device match pattern for recovery (e.g. base COM port or serial)
    std::string device_match_pattern_;

    // Clear ring buffer stale data
    void drain_ring();

    // Ring buffer helpers (caller must hold ring_mutex_)
    size_t ring_available() const;
    void ring_push(const uint8_t* data, size_t len);
    size_t ring_pop(uint8_t* buf, size_t max_len);

    bool is_handle_valid() const;
    void close_handle();
};

} // namespace QC

#endif // QC_QCDEVICE_TRANSPORT_H
