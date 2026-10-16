// Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
// SPDX-License-Identifier: BSD 3-Clause Clear License
//
// DeviceDiscovery: high-level, CLI-facing wrapper around the QDS device
// monitor. Owns none of the discovery state itself — everything is
// forwarded to TransportManager — but keeps that implementation detail out
// of this public header.

#ifndef QC_EXPORTS_DEVICE_DISCOVERY_H
#define QC_EXPORTS_DEVICE_DISCOVERY_H

#include "exports/Definitions.h"

#include <cstdint>
#include <string>
#include <vector>

namespace QC {

/// High-level device discovery API for CLI (and other) consumers.
/// Thin wrapper over TransportManager's monitor lifecycle, enumeration, and
/// device/protocol change callbacks.
class QLC_API DeviceDiscovery {
public:
    DeviceDiscovery() = default;
    ~DeviceDiscovery() = default;

    // Non-copyable
    DeviceDiscovery(const DeviceDiscovery&) = delete;
    DeviceDiscovery& operator=(const DeviceDiscovery&) = delete;

    /// Start the QDS device monitor. No-op if already started.
    /// @param wait_ms  Time to wait for the initial device scan to complete.
    void start_monitor(uint32_t wait_ms = 5000);

    /// Stop the QDS device monitor. Safe to call even if not running.
    void stop_monitor();

    /// Whether the device monitor is currently running.
    bool is_monitor_running() const;

    /// Enumerate devices grouped by composite parent.
    /// @param filter  Protocol types to include. Empty = no filtering.
    std::vector<DeviceInfo> enumerate_composite_devices(
        const std::vector<ProtocolType>& filter = {PROT_DIAG, PROT_QDSS, PROT_ADPL});

    /// Opaque subscription token; pass to unsubscribe() to detach.
    using ListenerToken = uint64_t;

    /// Register a callback invoked after a protocol/interface has been added
    /// to the device list. Safe to call before or after start_monitor().
    ListenerToken set_protocol_added_listener(ProtocolConnectedCallback callback);

    /// Register a callback invoked after a protocol/interface has been
    /// removed from the device list.
    ListenerToken set_protocol_removed_listener(ProtocolConnectedCallback callback);

    /// Register a callback invoked when a device becomes available (its
    /// first protocol arrives).
    ListenerToken set_device_connected_listener(DeviceConnectedCallback callback);

    /// Register a callback invoked when a device goes away (its last
    /// protocol departs).
    ListenerToken set_device_disconnected_listener(DeviceConnectedCallback callback);

    /// Register a callback invoked whenever any protocol's connection state
    /// changes (see ProtocolState). Fires for every Protocol instance the
    /// process drives (e.g. the Diag protocol backing a DiagLoggerService),
    /// not just ones discovered via enumeration.
    ListenerToken set_protocol_state_listener(ProtocolStateCallback callback);

    /// Remove a previously registered listener. Safe to call with an unknown
    /// or already-removed token (no-op).
    void unsubscribe(ListenerToken token);
};

} // namespace QC

#endif // QC_EXPORTS_DEVICE_DISCOVERY_H
