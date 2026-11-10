// Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
// SPDX-License-Identifier: BSD 3-Clause Clear License

#include "exports/device_discovery.h"
#include "transport/transport_manager.h"

namespace QC {

void DeviceDiscovery::start_monitor(uint32_t wait_ms) {
    TransportManager::instance().start_monitor(wait_ms);
}

void DeviceDiscovery::stop_monitor() {
    TransportManager::instance().stop_monitor();
}

bool DeviceDiscovery::is_monitor_running() const {
    return TransportManager::instance().is_monitor_running();
}

std::vector<DeviceInfo> DeviceDiscovery::get_device_list() {
    return TransportManager::instance().enumerate_composite_devices();
}

DeviceDiscovery::ListenerToken DeviceDiscovery::set_protocol_added_listener(
    ProtocolConnectedCallback callback) {
    return TransportManager::instance().set_protocol_added_listener(callback);
}

DeviceDiscovery::ListenerToken DeviceDiscovery::set_protocol_removed_listener(
    ProtocolConnectedCallback callback) {
    return TransportManager::instance().set_protocol_removed_listener(callback);
}

DeviceDiscovery::ListenerToken DeviceDiscovery::set_device_connected_listener(
    DeviceConnectedCallback callback) {
    return TransportManager::instance().set_device_connected_listener(callback);
}

DeviceDiscovery::ListenerToken DeviceDiscovery::set_device_disconnected_listener(
    DeviceConnectedCallback callback) {
    return TransportManager::instance().set_device_disconnected_listener(callback);
}

DeviceDiscovery::ListenerToken DeviceDiscovery::set_protocol_state_listener(
    ProtocolStateCallback callback) {
    return TransportManager::instance().set_protocol_state_listener(callback);
}

void DeviceDiscovery::unsubscribe(ListenerToken token) {
    TransportManager::instance().unsubscribe(token);
}

} // namespace QC
