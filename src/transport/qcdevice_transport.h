// Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
// SPDX-License-Identifier: BSD 3-Clause Clear License
//
// QcDevice USB transport wrapping the QDS QcDevice API. Implements only
// the device-level single read/write; the base Transport class owns the
// RX/TX threads and byte queues.

#ifndef QC_QCDEVICE_TRANSPORT_H
#define QC_QCDEVICE_TRANSPORT_H

#include "transport.h"

#include <string>

#ifdef TOOLS_TARGET_WINDOWS
#include <windows.h>
#else
#include <cstdint>
typedef long int HANDLE;
#endif

namespace QC {

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

    // Concrete-transport interface, driven by the owning Protocol. The
    // async lifecycle helpers (request_open/close, wait_ready) live on
    // Protocol. is_open/is_connected/is_disconnected are provided by
    // Transport based on state_ transitions this class performs.
    bool open() override;
    void close() override;
    size_t write(const uint8_t* data, size_t len) override;
    size_t read(uint8_t* buf, size_t max_len, uint32_t timeout_ms) override;
    std::string description() const override;

private:
    static constexpr int OPEN_RETRY_COUNT    = 8;
    static constexpr int OPEN_RETRY_DELAY_MS = 250;
    static constexpr int MAX_SEND_RETRY      = 5;

    std::string device_name_;
    uint32_t baud_rate_;

#ifdef TOOLS_TARGET_WINDOWS
    HANDLE handle_ = INVALID_HANDLE_VALUE;
#else
    HANDLE handle_ = -1;
#endif

    bool is_handle_valid() const;
    void close_handle();
};

} // namespace QC

#endif // QC_QCDEVICE_TRANSPORT_H
