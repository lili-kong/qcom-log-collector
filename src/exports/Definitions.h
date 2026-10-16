// Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
// SPDX-License-Identifier: BSD 3-Clause Clear License
//
// Public device/protocol data structures shared across QLC's transport
// layer, protocol layer and CLI.

#ifndef QC_EXPORTS_DEFINITIONS_H
#define QC_EXPORTS_DEFINITIONS_H

#include <cstdint>
#include <list>
#include <string>
#include <vector>

// DLL export/import macro for the qlc public API (src/exports/). Building
// the shared DiagLoggerLib target defines QLC_DLL_EXPORTS; consumers
// linking against the import library get dllimport. QLC_STATIC opts out
// entirely for a statically-linked build of the same sources.
#if defined(_WIN32) && !defined(QLC_STATIC)
#ifdef QLC_DLL_EXPORTS
#define QLC_API __declspec(dllexport)
#else
#define QLC_API __declspec(dllimport)
#endif
#else
#define QLC_API
#endif

namespace QC {

/// Protocol type classification.
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

/// Transport-level connection classification.
enum ConnectionType {
    CONNECT_UNKNOWN = -1,
    CONNECT_USB = 0,
    CONNECT_MAX
};

/// Open/share access flags.
enum OpenProp {
    OPEN_NONE = 0x0000,
    OPEN_READ = 0x0001,
    OPEN_WRITE = 0x0002,
    OPEN_READ_WRITE = 0x0003
};

/// Protocol connection state. Protocol-level and distinct from
/// TransportState (see types.h): the port can be open (CONNECTED)
/// while the protocol is still handshaking (STATE_INITIALIZING) or the
/// peer has stopped answering (STATE_UNRESPONSIVE).
enum ProtocolState {
    STATE_AVAILABLE = 0,
    STATE_DISCONNECTED = 1,
    STATE_UNRESPONSIVE = 2,
    STATE_INITIALIZING = 3
};

/// SoC identification for a device.
struct SocInfo {
    std::string chip_name;
    std::string version;
};

/// Information about a discovered QcDevice protocol/interface.
struct ProtocolInfo {
    uint64_t       protocol_handle;     // Synthetic per-protocol handle; see TransportManager
    uint64_t       device_handle;       // Handle of the owning device; see TransportManager
    std::string    description;         // Full DevDesc string for OpenDevice
    ProtocolType   protocol_type;
    ConnectionType connection_type;     // QLC only discovers USB-attached devices
    OpenProp       connection_status;   // Not surfaced by QDS; reserved
    OpenProp       share_status;        // Not surfaced by QDS; reserved
    ProtocolState  protocol_state;      // Present in the monitor list => available
    uint32_t       local_port;          // Not surfaced by QDS; reserved
    uint32_t       remote_port;         // Not surfaced by QDS; reserved
    std::string    alternate_description; // Not surfaced by QDS; reserved

    // QLC-internal fields sourced from QDS; used for device grouping and CLI display.
    std::string  com_port;          // Extracted COM port (e.g. "COM3"), empty if N/A
    std::string  parent_device;     // Parent composite device description
    std::string  parent_location;   // Parent physical location on bus
    std::string  adb_serial_number; // ADB serial number from QcDevice
    std::string  msm_serial_number; // MSM serial number from QcDevice
    std::string  vid;               // USB vendor ID, parsed from HwId (e.g. "05C6")
    std::string  pid;               // USB product ID, parsed from HwId
    std::string  edl_chip_id;       // EDL chip ID, parsed from HwId's CID_ segment
    std::string  soc_version;       // SoC version string from QcDevice
    uint32_t     flags;             // Device flags (type, state, bus, QC driver)
};

/// A composite device with its child protocols.
/// Protocols sharing the same parent_device + parent_location are grouped.
struct DeviceInfo {
    uint64_t    device_handle;             // Synthetic device handle; see TransportManager
    std::string description;               // Parent device description (friendly name)
    std::list<ProtocolInfo> protocols;     // Child protocols (DIAG, QDSS, ADPL)

    std::string serial_number;             // MSM serial number
    std::string adb_serial_number;         // ADB serial number
    std::string location;                  // Physical bus location (== device_physical_location)
    std::string vid;                       // USB vendor ID
    std::string pid;                       // USB product ID
    std::string edl_chip_id;               // EDL chip ID
    std::string device_physical_location;  // Physical bus location
    SocInfo     soc_info;                  // SoC chip name/version
};

/// Calling convention for callbacks handed to QLC. __stdcall on Windows to
/// match standard host calling conventions; no-op on other platforms.
#if defined(_WIN32)
#define QC_CALLBACK __stdcall
#else
#define QC_CALLBACK
#endif

/// Invoked when a protocol/interface is added or removed. The same signature
/// serves both edges; which edge fired is implied by the listener the callback
/// was registered with (see TransportManager::set_protocol_added_listener and
/// set_protocol_removed_listener).
///
/// @param device    Owning device. \c device.protocols lists the protocols
///                  available on it — including \p protocol on the added edge,
///                  and excluding it on the removed edge.
/// @param protocol  The protocol that changed.
///
/// Called from the device monitor thread — keep it fast and non-blocking, do
/// not throw, and do not register listeners from inside it.
typedef void(QC_CALLBACK* ProtocolConnectedCallback)(const DeviceInfo& device,
                                                     const ProtocolInfo& protocol);

/// Invoked when a device as a whole becomes available or goes away — that is,
/// when its first protocol appears or its last one departs. Which edge fired is
/// implied by the listener the callback was registered with (see
/// TransportManager::set_device_connected_listener and
/// set_device_disconnected_listener).
///
/// @param device  The device that changed. \c device.protocols lists the
///                protocols present on it: at least one on the connected edge,
///                and empty on the disconnected edge.
///
/// Same threading constraints as ProtocolConnectedCallback.
typedef void(QC_CALLBACK* DeviceConnectedCallback)(const DeviceInfo& device);

/// Invoked whenever a protocol's connection state changes (see ProtocolState).
///
/// @param handle  Opaque handle identifying the protocol instance that
///                changed; stable for the lifetime of that instance.
/// @param state   The protocol's new state.
///
/// Called from whichever thread drove the transition (the lifecycle worker
/// or a command thread) — keep it fast and non-blocking, do not throw, and
/// do not register/unregister listeners from inside it.
typedef void(QC_CALLBACK* ProtocolStateCallback)(int64_t handle, ProtocolState state);

} // namespace QC

#endif // QC_EXPORTS_DEFINITIONS_H
