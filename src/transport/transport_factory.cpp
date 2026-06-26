// SPDX-License-Identifier: Apache-2.0
// Copyright 2024 Qualcomm Innovation Center, Inc.

#include "transport_factory.h"
#include "transport_manager.h"
#include "qcdevice_transport.h"

#include <iostream>
#include <KL/Macros.h>

namespace diaglogger {

std::unique_ptr<Transport> create_transport(const std::string& device_name,
                                            uint32_t baud_rate) {
    FLOG_DEBUG("Creating transport for device: " + device_name + " baud=" + std::to_string(baud_rate));
    return std::make_unique<QcDeviceTransport>(device_name, baud_rate);
}

std::vector<QcDeviceInfo> enumerate_qcdevices() {
    auto& mgr = TransportManager::instance();
    std::vector<QcDeviceInfo> result;

    auto devices = mgr.enumerate_interfaces();
    for (auto& dev : devices) {
        QcDeviceInfo info;
        info.device_name = std::move(dev.device_name);
        info.description = info.device_name;
        result.push_back(std::move(info));
    }

    return result;
}

} // namespace diaglogger
