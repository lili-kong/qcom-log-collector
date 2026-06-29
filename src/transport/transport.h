// Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
// SPDX-License-Identifier: BSD 3-Clause Clear License
//
// Abstract transport interface for device communication.

#ifndef DIAGLOGGER_TRANSPORT_H
#define DIAGLOGGER_TRANSPORT_H

#include "diaglogger/types.h"

namespace diaglogger {

/// Abstract transport interface.
class Transport {
public:
    virtual ~Transport() = default;

    virtual bool open() = 0;
    virtual void close() = 0;
    virtual bool is_open() const = 0;

    /// Write data to the transport. Returns number of bytes written.
    virtual size_t write(const uint8_t* data, size_t len) = 0;

    /// Read data with timeout. Returns bytes read (0 on timeout).
    /// timeout_ms: max time to wait for data.
    virtual size_t read(uint8_t* buf, size_t max_len, uint32_t timeout_ms) = 0;

    /// Human-readable description of this transport.
    virtual std::string description() const = 0;

    /// Whether the transport lost connection and is eligible for recovery.
    virtual bool is_disconnected() const { return false; }

    /// Attempt to re-open a disconnected transport. Returns true on success.
    virtual bool reopen() { return false; }

    /// Enable verbose per-packet I/O logging (TX/RX byte dumps, zero-reads).
    /// Off by default; routine I/O chatter is suppressed unless enabled.
    void set_verbose(bool v) { verbose_ = v; }

protected:
    bool verbose_ = false;
};

} // namespace diaglogger

#endif // DIAGLOGGER_TRANSPORT_H
