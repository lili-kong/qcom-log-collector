// Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
// SPDX-License-Identifier: BSD 3-Clause Clear License
//
// Local hardware diagnostic for the exported QLC C ABI.
#include "exports/qlc_diag_logger.h"

#include <cstdio>
#include <cstring>
#include <vector>

static const char* status_name(QlcStatus status) {
    switch (status) {
    case QLC_STATUS_OK: return "OK";
    case QLC_STATUS_INVALID_ARGUMENT: return "INVALID_ARGUMENT";
    case QLC_STATUS_INVALID_HANDLE: return "INVALID_HANDLE";
    case QLC_STATUS_BUFFER_TOO_SMALL: return "BUFFER_TOO_SMALL";
    case QLC_STATUS_TIMEOUT: return "TIMEOUT";
    case QLC_STATUS_NOT_READY: return "NOT_READY";
    case QLC_STATUS_INTERNAL_ERROR: return "INTERNAL_ERROR";
    default: return "UNKNOWN";
    }
}

static void print_error() {
    char buffer[2048]{};
    size_t required = 0;
    const QlcStatus status = qlc_last_error(buffer, sizeof(buffer), &required);
    std::printf("last_error status=%s required=%zu text=%s\n",
                status_name(status), required, buffer);
}

int main() {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    std::printf("abi=%u\n", qlc_abi_version());

    const QlcStatus start = qlc_device_discovery_start_monitoring(5000);
    std::printf("start_monitoring=%s\n", status_name(start));
    if (start != QLC_STATUS_OK) {
        print_error();
        return 1;
    }

    size_t required = 0;
    const QlcStatus query = qlc_device_discovery_get_devices(
        nullptr, 0, nullptr, &required);
    std::printf("query_devices=%s required=%zu\n", status_name(query), required);
    if (query != QLC_STATUS_OK) {
        print_error();
        qlc_device_discovery_stop_monitoring();
        return 1;
    }

    std::vector<QlcDeviceInfo> records(required);
    std::vector<std::vector<char>> buffers(required * 10, std::vector<char>(1024));
    for (size_t i = 0; i < required; ++i) {
        QlcDeviceInfo& record = records[i];
        auto set = [&](char*& pointer, size_t& capacity, size_t field) {
            pointer = buffers[i * 10 + field].data();
            capacity = buffers[i * 10 + field].size();
        };
        set(record.description, record.description_capacity, 0);
        set(record.serial_number, record.serial_number_capacity, 1);
        set(record.adb_serial_number, record.adb_serial_number_capacity, 2);
        set(record.location, record.location_capacity, 3);
        set(record.vid, record.vid_capacity, 4);
        set(record.pid, record.pid_capacity, 5);
        set(record.edl_chip_id, record.edl_chip_id_capacity, 6);
        set(record.device_physical_location, record.device_physical_location_capacity, 7);
        set(record.soc_chip_name, record.soc_chip_name_capacity, 8);
        set(record.soc_version, record.soc_version_capacity, 9);
    }

    size_t written = 0;
    const QlcStatus fetch = qlc_device_discovery_get_devices(
        records.data(), records.size(), &written, &required);
    std::printf("fetch_devices=%s written=%zu required=%zu\n",
                status_name(fetch), written, required);
    if (fetch != QLC_STATUS_OK) print_error();
    for (size_t i = 0; i < written; ++i) {
        std::printf("device[%zu] serial=%s description=%s location=%s\n",
                    i, records[i].serial_number, records[i].description,
                    records[i].location);
    }

    const QlcStatus stop = qlc_device_discovery_stop_monitoring();
    std::printf("stop_monitoring=%s\n", status_name(stop));
    return fetch == QLC_STATUS_OK && stop == QLC_STATUS_OK ? 0 : 1;
}
