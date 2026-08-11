// Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
// SPDX-License-Identifier: BSD 3-Clause Clear License
//
// Transport factory: creates the QcDeviceTransport and provides device
// enumeration.

#ifndef QC_TRANSPORT_FACTORY_H
#define QC_TRANSPORT_FACTORY_H

#include <memory>
#include <string>
#include <vector>

#include "transport.h"
#include "transport_manager.h"

namespace QC {

/// Information about a discovered QcDevice protocol.
struct QcDeviceInfo {
    std::string  device_name;     // Name for opening (pass to --port)
    std::string  description;     // Human-readable description
    ProtocolType protocol = PROT_UNKNOWN;
};

/// A composite device with grouped child protocols (2-level hierarchy).
struct QcCompositeDevice {
    std::string description;                 // Parent device friendly name
    std::string location;                    // Physical bus location
    std::string adb_serial_number;           // ADB serial number
    std::string msm_serial_number;           // MSM serial number
    std::vector<QcDeviceInfo> protocols;     // Child protocols (DIAG, QDSS, ADPL)
};

/// USB stream suffixes appended to a device name when opening interfaces whose
/// driver requires selecting a stream.  Windows-only; empty elsewhere.
/// Matches TUF's QDSS_TRACE_STREAM / DPL_STREAM (Device/Manager.cpp:94-100).
#ifdef TOOLS_TARGET_WINDOWS
constexpr const char* QDSS_TRACE_STREAM = "\\TRACE";
constexpr const char* ADPL_DPL_STREAM   = "\\DPL";
#else
constexpr const char* QDSS_TRACE_STREAM = "";
constexpr const char* ADPL_DPL_STREAM   = "";
#endif

/// Create a QcDeviceTransport for the given device name.
/// usb_stream is an optional stream suffix used only at open time (e.g.
/// QDSS_TRACE_STREAM); device discovery still matches the bare device name.
std::unique_ptr<Transport> create_transport(const std::string& device_name,
                                           uint32_t baud_rate,
                                           const std::string& usb_stream = std::string());

/// Enumerate connected QcDevices grouped by composite parent.
/// Returns a 2-level hierarchy: Device → Protocols (DIAG, QDSS, ADPL only).
std::vector<QcCompositeDevice> enumerate_composite_qcdevices();

} // namespace QC

#endif // QC_TRANSPORT_FACTORY_H
