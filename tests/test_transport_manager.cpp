// Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
// SPDX-License-Identifier: BSD 3-Clause Clear License

#include <iostream>
#include <stdexcept>
#include <string>

#include "qdpublic.h"
#include "transport/transport_manager.h"

namespace {

DEVICECHANGE_CALLBACK device_callback = nullptr;

// TransportManager decodes callback states from bits 4..7 of Flag.
constexpr ULONG ARRIVAL_FLAG = 0x10;
constexpr ULONG DEPARTURE_FLAG = 0x20;

struct DiscoveryEvent {
    std::wstring description;
    std::wstring parent;
    std::wstring location;
    std::wstring serial;
    std::wstring adb_serial;
    std::wstring hardware_id;
    std::wstring soc_version;
    ULONG protocol = DEV_PROTOCOL_UNKNOWN;
    ULONG state = ARRIVAL_FLAG;
};

#define CHECK(condition)                                                                           \
    do {                                                                                           \
        if (!(condition)) {                                                                        \
            throw std::runtime_error("Check failed: " #condition);                                 \
        }                                                                                          \
    } while (false)

void emit_event(DiscoveryEvent event) {
    CHECK(device_callback != nullptr);
    CB_PARAMS params{};
    params.DevDesc = event.description.data();
    params.ParentDev = event.parent.data();
    params.ParentLocationInfomation = event.location.data();
    params.SerNumMsm = event.serial.data();
    params.SerNum = event.adb_serial.data();
    params.HwId = event.hardware_id.data();
    params.SocVer = event.soc_version.data();
    params.Protocol = event.protocol;
    params.Flag = event.state;
    device_callback(&params, nullptr);
}

QC::DeviceInfo find_device(const std::string& description, const std::string& location) {
    for (const auto& device : QC::TransportManager::instance().enumerate_composite_devices({})) {
        if (device.description == description && device.location == location) {
            return device;
        }
    }
    throw std::runtime_error("Expected device was not discovered");
}

void test_sibling_metadata_and_protocol_resolution() {
    emit_event({L"Diagnostics (COM41)", L"Composite", L"port-1"});
    CHECK(find_device("Composite", "port-1").protocols.front().protocol_type == QC::PROT_UNKNOWN);
    emit_event({L"QDSS Data", L"Composite", L"port-1", L"serial-1", L"adb-1",
                L"USB\\VID_05C6&PID_90DB&CID_042A", L"soc-1", DEV_PROTOCOL_QDSS});
    const auto device = find_device("Composite", "port-1");
    CHECK(device.protocols.size() == 2);
    CHECK(device.protocols.front().protocol_type == QC::PROT_DIAG);
    CHECK(device.protocols.back().protocol_type == QC::PROT_QDSS);
    CHECK(device.serial_number == "serial-1");
    CHECK(device.adb_serial_number == "adb-1");
    CHECK(device.device_physical_location == "port-1");
    CHECK(device.vid == "05C6" && device.pid == "90DB");
    CHECK(device.edl_chip_id == "042A");
    CHECK(device.soc_info.chip_name == "soc-1" && device.soc_info.version == "soc-1");
    CHECK(device.device_handle != 0);
    CHECK(device.protocols.front().device_handle == device.device_handle);
    CHECK(device.protocols.back().device_handle == device.device_handle);
    CHECK(device.protocols.front().protocol_handle != device.protocols.back().protocol_handle);
}

void test_locations_and_standalone_interfaces() {
    emit_event({L"Diagnostics (COM42)",
                L"Composite",
                L"port-2",
                L"serial-2",
                {},
                L"PCI\\VEN_17CB&DEV_0306",
                {},
                DEV_PROTOCOL_DIAG});
    const auto separate = find_device("Composite", "port-2");
    CHECK(separate.protocols.size() == 1);
    CHECK(separate.serial_number == "serial-2");
    CHECK(separate.vid == "17CB" && separate.pid == "0306");
    CHECK(separate.device_handle != find_device("Composite", "port-1").device_handle);
    emit_event({L"Standalone Diagnostics (COM43)"});
    const auto standalone = find_device("Standalone Diagnostics (COM43)", "");
    CHECK(standalone.protocols.front().protocol_type == QC::PROT_UNKNOWN);
    CHECK(standalone.serial_number.empty());
    CHECK(standalone.vid.empty() && standalone.pid.empty());
}

void test_reconnect_preserves_metadata_and_handles() {
    const auto before = find_device("Composite", "port-1");
    emit_event({L"Diagnostics (COM41)",
                L"Composite",
                L"port-1",
                {},
                {},
                {},
                {},
                DEV_PROTOCOL_UNKNOWN,
                DEPARTURE_FLAG});
    emit_event(
        {L"QDSS Data", L"Composite", L"port-1", {}, {}, {}, {}, DEV_PROTOCOL_QDSS, DEPARTURE_FLAG});
    for (const auto& device : QC::TransportManager::instance().enumerate_composite_devices({})) {
        CHECK(device.device_handle != before.device_handle);
    }
    emit_event({L"Diagnostics (COM41)", L"Composite", L"port-1"});
    const auto restored = find_device("Composite", "port-1");
    CHECK(restored.device_handle == before.device_handle);
    CHECK(restored.protocols.front().protocol_handle == before.protocols.front().protocol_handle);
    CHECK(restored.protocols.front().protocol_type == QC::PROT_DIAG);
    CHECK(restored.serial_number == before.serial_number);
    CHECK(restored.adb_serial_number == before.adb_serial_number);
    CHECK(restored.vid == before.vid && restored.pid == before.pid);
    CHECK(restored.edl_chip_id == before.edl_chip_id);
    CHECK(restored.soc_info.version == before.soc_info.version);
}

} // namespace

// Replace QDS device I/O so discovery is exercised through its real callback.
namespace QcDevice {

VOID SetLoggingCallback(Utils::QCD_LOGGING_CALLBACK) {}
VOID SetFeature(PVOID) {}
VOID StartDeviceMonitor() {}
VOID StopDeviceMonitor() {}

VOID SetDeviceChangeCallback(DEVICECHANGE_CALLBACK callback, PVOID) { device_callback = callback; }

ULONG GetDeviceList(PVOID, ULONG, PULONG actual_size) {
    *actual_size = 0;
    return 0;
}

} // namespace QcDevice

int main() {
    auto& manager = QC::TransportManager::instance();
    try {
        manager.start_monitor(0);
        test_sibling_metadata_and_protocol_resolution();
        test_locations_and_standalone_interfaces();
        test_reconnect_preserves_metadata_and_handles();
        manager.stop_monitor();
        std::cout << "Transport manager discovery tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        manager.stop_monitor();
        std::cerr << error.what() << '\n';
        return 1;
    }
}
