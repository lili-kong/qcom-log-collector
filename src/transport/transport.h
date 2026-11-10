// Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
// SPDX-License-Identifier: BSD 3-Clause Clear License
//
// Abstract transport interface for device communication.
//
// This is the low-level (driver-level) contract only: open/close the port,
// one blocking read, one blocking write, and a human-readable description.
// A transport deals in raw bytes and knows nothing about framing, worker
// threads, or request/response pairing — all of that lives in Protocol,
// which owns a Transport via unique_ptr and drives it.

#ifndef QC_TRANSPORT_H
#define QC_TRANSPORT_H

#include "types.h"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <string>

namespace QC {

/// Abstract transport interface.
class Transport {
public:
    Transport();
    virtual ~Transport();

    // Non-copyable
    Transport(const Transport&) = delete;
    Transport& operator=(const Transport&) = delete;

    // --- Concrete-transport interface (implemented by subclasses) ---
    //
    // Public because the owning Protocol drives these through its
    // unique_ptr<Transport>; they are not meant for business logic, which
    // goes through the Protocol API instead.

    /// Open cooperatively, checking cancellation during retries and waits.
    /// The caller keeps cancel_requested alive until this call returns.
    virtual bool open(const std::atomic<bool>& cancel_requested) = 0;
    virtual void close() = 0;

    /// Write data to the transport. Returns number of bytes written.
    virtual size_t write(const uint8_t* data, size_t len) = 0;

    /// Read data with timeout. Returns bytes read (0 on timeout).
    /// timeout_ms: max time to wait for data.
    virtual size_t read(uint8_t* buf, size_t max_len, uint32_t timeout_ms) = 0;

    /// Human-readable description of this transport.
    virtual std::string description() const = 0;

    // --- State queries (implemented here from state_) ---

    /// Whether the port exists — opened and not yet closed. Stays true after
    /// an unexpected device loss, so a lost link is distinguishable from a
    /// deliberate close.
    bool is_open() const { return state_.load() != TransportState::CLOSED; }

    /// Whether the port is available for I/O. False once the link is lost,
    /// even while is_open() is still true.
    bool is_connected() const { return state_.load() == TransportState::CONNECTED; }

    /// Whether the transport lost connection and is eligible for recovery.
    /// Equivalent to is_open() && !is_connected().
    bool is_disconnected() const { return state_.load() == TransportState::DISCONNECTED; }

    /// Enable verbose per-packet I/O logging (TX/RX byte dumps, zero-reads).
    /// Off by default; routine I/O chatter is suppressed unless enabled.
    void set_verbose(bool v) { verbose_ = v; }

protected:
    bool verbose_ = false;

    /// Transport lifecycle state. Subclasses transition it:
    ///   CLOSED       -> CONNECTED    on successful open
    ///   CONNECTED    -> DISCONNECTED on unexpected device loss
    ///   any          -> CLOSED       on deliberate close
    /// Base uses this to implement is_open/is_connected/is_disconnected
    /// so subclasses don't have to.
    std::atomic<TransportState> state_{TransportState::CLOSED};
};

} // namespace QC

#endif // QC_TRANSPORT_H
