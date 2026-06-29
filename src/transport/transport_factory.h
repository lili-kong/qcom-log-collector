// Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
// SPDX-License-Identifier: BSD 3-Clause Clear License
//
// Transport factory: creates the QcDeviceTransport and provides device
// enumeration.

#ifndef DIAGLOGGER_TRANSPORT_FACTORY_H
#define DIAGLOGGER_TRANSPORT_FACTORY_H

#include "transport.h"
#include <memory>
#include <string>
#include <vector>

namespace diaglogger {

/// Information about a discovered QcDevice.
struct QcDeviceInfo {
    std::string device_name;     // Name for opening (pass to --port)
    std::string description;     // Human-readable description
    uint32_t    protocol = 0;    // USB protocol code if available (0x30=DIAG, 0x70=QDSS, etc.)
};

/// Create a QcDeviceTransport for the given device name.
std::unique_ptr<Transport> create_transport(const std::string& device_name,
                                            uint32_t baud_rate);

/// Enumerate connected QcDevice diag devices.
/// Returns a list of devices with DEV_PROTOCOL_DIAG (0x30).
std::vector<QcDeviceInfo> enumerate_qcdevices();

} // namespace diaglogger

#endif // DIAGLOGGER_TRANSPORT_FACTORY_H
