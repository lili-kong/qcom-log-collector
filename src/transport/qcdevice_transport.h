// SPDX-License-Identifier: Apache-2.0
// Copyright 2024 Qualcomm Innovation Center, Inc.
//
// QcDevice USB transport wrapping the QDS QcDevice API for device
// discovery, open, read, write, and close.
//
// Follows the same pattern as TUF's Usb.cpp:
//   - QcDevice::OpenDevice for opening (with retries)
//   - QcDevice::ReadFromDevice in an RX worker thread
//   - QcDevice::SendToDevice for writes (with retry loop)
//   - CancelIoEx + QcDevice::CloseDevice for closing

#ifndef DIAGLOGGER_QCDEVICE_TRANSPORT_H
#define DIAGLOGGER_QCDEVICE_TRANSPORT_H

#include "transport.h"

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

namespace diaglogger {

class QcDeviceTransport : public Transport {
public:
    /// Construct with a QcDevice device name (the DevDesc string from
    /// QcDevice::GetDeviceList) and optional baud rate.
    explicit QcDeviceTransport(const std::string& device_name,
                               uint32_t baud_rate = 38400);
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

} // namespace diaglogger

#endif // DIAGLOGGER_QCDEVICE_TRANSPORT_H
