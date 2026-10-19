// Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
// SPDX-License-Identifier: BSD 3-Clause Clear License
//

#include "transport_factory.h"
#include "transport_manager.h"
#include "qcdevice_transport.h"

#include <iostream>
#include <KL/Macros.h>

namespace QC {

std::unique_ptr<Transport> create_transport(const std::string& device_name,
                                            uint32_t baud_rate) {
    FLOG_DEBUG("Creating transport for device: " + device_name + " baud=" + std::to_string(baud_rate));
    return std::make_unique<QcDeviceTransport>(device_name, baud_rate);
}

std::vector<DeviceInfo> enumerate_composite_qcdevices() {
    return TransportManager::instance().enumerate_composite_devices();
}

} // namespace QC
