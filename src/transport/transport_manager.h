// Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
// SPDX-License-Identifier: BSD 3-Clause Clear License
//
// TransportManager: centralizes QcDevice device detection, monitoring,
// and enumeration. Provides a single point of control for the QDS
// device monitor lifecycle and device discovery operations.

#ifndef QC_TRANSPORT_MANAGER_H
#define QC_TRANSPORT_MANAGER_H

#include "exports/Definitions.h"

#include <mutex>
#include <string>
#include <vector>

#ifdef TOOLS_TARGET_WINDOWS
#include <windows.h>
#endif

namespace QC {

/// Manages QcDevice device detection and monitor lifecycle.
/// Thread-safe singleton — call instance() to access.
class TransportManager {
public:
    static TransportManager& instance();

    // Non-copyable, non-movable
    TransportManager(const TransportManager&) = delete;
    TransportManager& operator=(const TransportManager&) = delete;

    /// Start the QDS device monitor (SetFeature + SetDeviceChangeCallback +
    /// StartDeviceMonitor). No-op if already started.
    /// @param wait_ms  Time to wait for device scan to complete.
    void start_monitor(uint32_t wait_ms = 5000);

    /// Stop the QDS device monitor. Resets _IsQcomLibusbEnable = FALSE.
    /// Safe to call even if monitor is not running.
    void stop_monitor();

    /// Whether the device monitor is currently running.
    bool is_monitor_running() const;

    /// Enumerate all currently discovered protocols (flat list).
    /// Requires monitor to already be running (via start_monitor).
    std::vector<ProtocolInfo> enumerate_devices();

    /// Enumerate devices grouped by composite parent.
    /// Protocols sharing the same parent are grouped under one DeviceInfo.
    /// Standalone (non-composite) protocols get their own DeviceInfo entry.
    /// @param filter  Protocol types to include. Empty = no filtering.
    std::vector<DeviceInfo> enumerate_composite_devices(
        const std::vector<ProtocolType>& filter = {PROT_DIAG, PROT_QDSS, PROT_ADPL});

    /// Resolve a short device name (e.g. "COM3") to the full DevDesc string
    /// by searching the current device list. Returns the resolved name,
    /// or the original name if no match is found.
    std::string resolve_device_name(const std::string& short_name);

    /// Extract COM port name (e.g. "COM3") from a full QcDevice description.
    /// Returns empty string if no COM port pattern is found.
    static std::string extract_com_port(const std::string& desc);

    /// Check if a COM port is already open by another process.
    /// Returns true if the port is in use (sharing violation detected).
    static bool is_com_port_in_use(const std::string& com_port);

    /// Opaque subscription token; pass to unsubscribe() to detach.
    using ListenerToken = uint64_t;

    /// Register a callback invoked after a protocol/interface has been added
    /// to the internal device list. Returns a token to pass to unsubscribe().
    /// Safe to call before or after start_monitor(); register first to receive
    /// the initial-enumeration events for already-attached devices.
    /// Passing a null callback is a no-op and returns 0.
    ListenerToken set_protocol_added_listener(ProtocolConnectedCallback callback);

    /// Register a callback invoked after a protocol/interface has been removed
    /// from the internal device list. Returns a token to pass to unsubscribe().
    /// Passing a null callback is a no-op and returns 0.
    ListenerToken set_protocol_removed_listener(ProtocolConnectedCallback callback);

    /// Register a callback invoked when a device becomes available, i.e. when
    /// its first protocol arrives. Fires before the corresponding
    /// protocol-added callback, and again on every reconnect after the device
    /// has been fully disconnected. Returns a token to pass to unsubscribe().
    /// Passing a null callback is a no-op and returns 0.
    ListenerToken set_device_connected_listener(DeviceConnectedCallback callback);

    /// Register a callback invoked when a device goes away, i.e. when its last
    /// protocol departs. Fires after the corresponding protocol-removed
    /// callback. Returns a token to pass to unsubscribe().
    /// Passing a null callback is a no-op and returns 0.
    ListenerToken set_device_disconnected_listener(DeviceConnectedCallback callback);

    /// Register a callback invoked whenever any protocol's connection state
    /// changes (see ProtocolState). Returns a token to pass to unsubscribe().
    /// Passing a null callback is a no-op and returns 0.
    ListenerToken set_protocol_state_listener(ProtocolStateCallback callback);

    /// Remove a previously registered listener. Safe to call with an unknown
    /// or already-removed token (no-op). Accepts tokens from any setter.
    void unsubscribe(ListenerToken token);

    /// Notify registered protocol-state listeners of a state change. Called
    /// by Protocol::set_state() — not part of the discovery API surface, but
    /// public so any Protocol subclass across translation units can reach
    /// the singleton without transport_manager.h depending on protocol.h.
    void notify_protocol_state_changed(int64_t handle, ProtocolState state);

#ifdef TOOLS_TARGET_WINDOWS
    /// Convert narrow string (CP_ACP) to wide string for QcDevice APIs.
    static std::wstring to_wide(const std::string& s);
#endif

private:
    TransportManager() = default;
    ~TransportManager() = default;

    mutable std::mutex mutex_;
    bool monitor_running_ = false;

    /// Configure QDS feature settings (exclude DEV_CLASS_LIBUSB).
    void configure_features();
};

} // namespace QC

#endif // QC_TRANSPORT_MANAGER_H
