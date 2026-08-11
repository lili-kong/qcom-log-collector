// Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
// SPDX-License-Identifier: BSD 3-Clause Clear License

#include <iostream>

#include <KL/Macros.h>

#include "qcdevice_transport.h"
#include "transport_factory.h"
#include "transport_manager.h"

namespace QC {

std::unique_ptr<Transport> create_transport(const std::string& device_name,
                                            uint32_t baud_rate,
                                            const std::string& usb_stream) {
    FLOG_DEBUG("Creating transport for device: " + device_name + usb_stream
               + " baud=" + std::to_string(baud_rate));
    return std::make_unique<QcDeviceTransport>(device_name, baud_rate, usb_stream);
}

std::vector<QcCompositeDevice> enumerate_composite_qcdevices() {
    auto& mgr = TransportManager::instance();
    std::vector<QcCompositeDevice> result;

    auto composites = mgr.enumerate_composite_devices();
    for (auto& comp : composites) {
        QcCompositeDevice dev;
        dev.description = std::move(comp.description);
        dev.location = std::move(comp.location);
        dev.adb_serial_number = std::move(comp.adb_serial_number);
        dev.msm_serial_number = std::move(comp.msm_serial_number);
        for (auto& proto : comp.protocols) {
            QcDeviceInfo info;
            info.device_name = std::move(proto.device_name);
            info.description = info.device_name;
            info.protocol = proto.protocol;
            dev.protocols.push_back(std::move(info));
        }
        result.push_back(std::move(dev));
    }

    return result;
}

} // namespace QC
