// Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
// SPDX-License-Identifier: BSD 3-Clause Clear License
//
// Transport factory: creates the QcDeviceTransport and provides device
// enumeration.

#ifndef QC_TRANSPORT_FACTORY_H
#define QC_TRANSPORT_FACTORY_H

#include "transport.h"
#include "transport_manager.h"
#include <memory>
#include <string>
#include <vector>

namespace QC {

/// Create a QcDeviceTransport for the given device name.
std::unique_ptr<Transport> create_transport(const std::string& device_name,
                                            uint32_t baud_rate);

/// Enumerate connected QcDevices grouped by composite parent.
/// Returns a 2-level hierarchy: Device → Protocols (DIAG, QDSS, ADPL only).
std::vector<DeviceInfo> enumerate_composite_qcdevices();

} // namespace QC

#endif // QC_TRANSPORT_FACTORY_H
