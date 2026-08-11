// Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
// SPDX-License-Identifier: BSD 3-Clause Clear License
//
// TransportManager: centralizes QcDevice device detection, monitoring,
// and enumeration. Provides a single point of control for the QDS
// device monitor lifecycle and device discovery operations.

#ifndef QC_TRANSPORT_MANAGER_H
#define QC_TRANSPORT_MANAGER_H

#include <mutex>
#include <string>
#include <vector>

#ifdef TOOLS_TARGET_WINDOWS
#include <windows.h>
#endif

namespace QC {

/// Protocol type classification (mirrors QUTS ProtocolType enum).
enum ProtocolType {
    PROT_UNKNOWN = -1,
    PROT_SAHARA,
    PROT_FIREHOSE,
    PROT_DIAG,
    PROT_ADPL,
    PROT_QDSS,
    PROT_QMI,
    PROT_ADB,
    PROT_FASTBOOT,
    PROT_NMEA,
    PROT_DUN,
    PROT_RMNET,
    PROT_RNDIS,
    PROT_MBIM,
    PROT_PROTOBUF,
    PROT_TAC,
    PROT_EPM,
    PROT_TELNET,
    PROT_FTP,
    PROT_EUD,
    PROT_QSPS,
    PROT_SERIAL,
    PROT_PROMIRA,
    PROT_MAX
};

/// Information about a discovered QcDevice protocol.
struct DeviceInfo {
    std::string  device_name;     // Full DevDesc string for OpenDevice
    std::string  com_port;        // Extracted COM port (e.g. "COM3"), empty if N/A
    std::string  parent_device;   // Parent composite device description
    std::string  parent_location; // Parent physical location on bus
    std::string  adb_serial_number; // ADB serial number from QcDevice
    std::string  msm_serial_number; // MSM serial number from QcDevice
    ProtocolType protocol = PROT_UNKNOWN;
    uint32_t     flags = 0;       // Device flags (type, state, bus, QC driver)
};

/// A composite device with its child protocols.
/// Protocols sharing the same parent_device + parent_location are grouped.
struct CompositeDevice {
    std::string description;             // Parent device description (friendly name)
    std::string location;                // Physical bus location
    std::string adb_serial_number;       // ADB serial number
    std::string msm_serial_number;       // MSM serial number
    std::vector<DeviceInfo> protocols;   // Child protocols (DIAG, QDSS, ADPL)
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

    /// Enumerate all currently discovered devices (flat list).
    /// Requires monitor to already be running (via start_monitor).
    std::vector<DeviceInfo> enumerate_devices();

    /// Enumerate devices grouped by composite parent.
    /// Protocols sharing the same parent are grouped under one CompositeDevice.
    /// Standalone (non-composite) protocols get their own CompositeDevice entry.
    /// @param filter  Protocol types to include. Empty = no filtering.
    std::vector<CompositeDevice> enumerate_composite_devices(
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
