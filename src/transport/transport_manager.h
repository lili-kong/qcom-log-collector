// SPDX-License-Identifier: Apache-2.0
// Copyright 2024 Qualcomm Innovation Center, Inc.
//
// TransportManager: centralizes QcDevice device detection, monitoring,
// and enumeration. Provides a single point of control for the QDS
// device monitor lifecycle and device discovery operations.

#ifndef DIAGLOGGER_TRANSPORT_MANAGER_H
#define DIAGLOGGER_TRANSPORT_MANAGER_H

#include <mutex>
#include <string>
#include <vector>

#ifdef TOOLS_TARGET_WINDOWS
#include <windows.h>
#endif

namespace diaglogger {

/// Information about a discovered QcDevice.
struct DeviceInfo {
    std::string device_name;     // Full DevDesc string for OpenDevice
    std::string com_port;        // Extracted COM port (e.g. "COM3"), empty if N/A
};

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

    /// Enumerate all currently discovered devices.
    /// Requires monitor to already be running (via start_monitor).
    std::vector<DeviceInfo> enumerate_devices();

    /// Enumerate libusb sub-interfaces (populated by device callbacks
    /// after enumerate_devices() triggers libusb enumeration).
    std::vector<DeviceInfo> enumerate_interfaces();

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

} // namespace diaglogger

#endif // DIAGLOGGER_TRANSPORT_MANAGER_H
