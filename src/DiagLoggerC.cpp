// Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
// SPDX-License-Identifier: BSD 3-Clause Clear License

#include "exports/qlc_diag_logger.h"

#include "exports/device_discovery.h"
#include "exports/diag_logger.h"

#include <KL/kLogger.h>

#include <algorithm>
#include <chrono>
#include <cstring>
#include <exception>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

struct QlcDiagLoggerHandleOpaque {
    std::unique_ptr<QC::DiagLogger> implementation;
    std::string last_error;
};

namespace {
std::mutex g_error_mutex;
std::string g_last_error;
QC::DeviceDiscovery g_discovery;

void configure_console_for_host() {
#ifdef QLC_DLL_EXPORTS
    static std::once_flag configured;
    std::call_once(configured, [] {
        auto& logger = KL::Logger::get_instance();
        logger.setOptions(logger.getOptions() | KL::LogOption::ConsoleToStderr);
    });
#endif
}

void set_error(const std::string& error) {
    std::lock_guard<std::mutex> lock(g_error_mutex);
    g_last_error = error;
}

QlcStatus copy_text(const std::string& value, char* buffer, size_t capacity, size_t* required) {
    const size_t needed = value.size() + 1;
    if (required != nullptr) *required = needed;
    if (buffer == nullptr || capacity == 0) {
        return needed == 1 ? QLC_STATUS_OK : QLC_STATUS_BUFFER_TOO_SMALL;
    }
    const size_t count = (std::min)(value.size(), capacity - 1);
    std::memcpy(buffer, value.data(), count);
    buffer[count] = '\0';
    return count == value.size() ? QLC_STATUS_OK : QLC_STATUS_BUFFER_TOO_SMALL;
}

QlcStatus copy_field(const std::string& value, char* buffer, size_t capacity, QlcStatus current) {
    const QlcStatus status = copy_text(value, buffer, capacity, nullptr);
    return status == QLC_STATUS_OK ? current : QLC_STATUS_BUFFER_TOO_SMALL;
}

QC::ProtocolType protocol_type(int32_t value) {
    return static_cast<QC::ProtocolType>(value);
}

QC::DiagLoggerConfig make_config(const QlcDiagLoggerConfig& source) {
    QC::DiagLoggerConfig result;
    if (source.cfg_path) result.cfg_path = source.cfg_path;
    if (source.port_name) result.port_name = source.port_name;
    if (source.qdss_port_name) result.qdss_port_name = source.qdss_port_name;
    if (source.adpl_port_name) result.adpl_port_name = source.adpl_port_name;
    if (source.output_path) result.output_path = source.output_path;
    result.baud_rate = source.baud_rate;
    result.duration_seconds = source.duration_seconds;
    result.command_timeout_ms = source.command_timeout_ms;
    result.verbose = source.verbose != 0;
    result.hdlc_only = source.hdlc_only != 0;
    result.enable_recovery = source.enable_recovery != 0;
    result.recovery_timeout_seconds = source.recovery_timeout_seconds;
    result.enable_qdss = source.enable_qdss != 0;
    result.enable_adpl = source.enable_adpl != 0;
    result.qdss_processor_mask = source.qdss_processor_mask;
    result.adpl_version = source.adpl_version;
    return result;
}

QlcStatus copy_device(const QC::DeviceInfo& source, QlcDeviceInfo& target) {
    QlcStatus status = QLC_STATUS_OK;
    target.device_handle = source.device_handle;
    status = copy_field(source.description, target.description, target.description_capacity, status);
    status = copy_field(source.serial_number, target.serial_number, target.serial_number_capacity, status);
    status = copy_field(source.adb_serial_number, target.adb_serial_number, target.adb_serial_number_capacity, status);
    status = copy_field(source.location, target.location, target.location_capacity, status);
    status = copy_field(source.vid, target.vid, target.vid_capacity, status);
    status = copy_field(source.pid, target.pid, target.pid_capacity, status);
    status = copy_field(source.edl_chip_id, target.edl_chip_id, target.edl_chip_id_capacity, status);
    status = copy_field(source.device_physical_location, target.device_physical_location,
                        target.device_physical_location_capacity, status);
    status = copy_field(source.soc_info.chip_name, target.soc_chip_name,
                        target.soc_chip_name_capacity, status);
    status = copy_field(source.soc_info.version, target.soc_version, target.soc_version_capacity, status);
    return status;
}

QlcStatus copy_protocol(const QC::ProtocolInfo& source, QlcProtocolInfo& target) {
    QlcStatus status = QLC_STATUS_OK;
    target.protocol_handle = source.protocol_handle;
    target.device_handle = source.device_handle;
    target.protocol_type = static_cast<int32_t>(source.protocol_type);
    target.connection_type = static_cast<int32_t>(source.connection_type);
    target.connection_status = static_cast<int32_t>(source.connection_status);
    target.share_status = static_cast<int32_t>(source.share_status);
    target.protocol_state = static_cast<int32_t>(source.protocol_state);
    target.local_port = source.local_port;
    target.remote_port = source.remote_port;
    status = copy_field(source.description, target.description, target.description_capacity, status);
    status = copy_field(source.alternate_description, target.alternate_description,
                        target.alternate_description_capacity, status);
    // com_port, parent_device, parent_location, adb_serial_number,
    // msm_serial_number, vid, pid, edl_chip_id, soc_version, and flags moved
    // to QC::DeviceInfo upstream (they were device-level facts duplicated
    // onto every protocol). None of these are read by any caller of this C
    // ABI today, so they are left at their zero-initialized defaults here
    // rather than threading the parent DeviceInfo through for values nothing
    // consumes.
    status = copy_field(std::string(), target.com_port, target.com_port_capacity, status);
    status = copy_field(std::string(), target.parent_device, target.parent_device_capacity, status);
    status = copy_field(std::string(), target.parent_location, target.parent_location_capacity, status);
    status = copy_field(std::string(), target.adb_serial_number,
                        target.adb_serial_number_capacity, status);
    status = copy_field(std::string(), target.msm_serial_number,
                        target.msm_serial_number_capacity, status);
    status = copy_field(std::string(), target.vid, target.vid_capacity, status);
    status = copy_field(std::string(), target.pid, target.pid_capacity, status);
    status = copy_field(std::string(), target.edl_chip_id,
                        target.edl_chip_id_capacity, status);
    status = copy_field(std::string(), target.soc_version,
                        target.soc_version_capacity, status);
    target.flags = 0;
    return status;
}

template <typename Source, typename Target, typename Copier>
QlcStatus copy_records(const std::vector<Source>& source, Target* records, size_t capacity,
                       size_t* written, size_t* required, Copier copier) {
    if (written) *written = (std::min)(source.size(), capacity);
    if (required) *required = source.size();
    if (capacity == 0 && records == nullptr) return QLC_STATUS_OK;
    if (source.size() > 0 && records == nullptr && capacity != 0) return QLC_STATUS_INVALID_ARGUMENT;
    QlcStatus status = QLC_STATUS_OK;
    const size_t count = (std::min)(source.size(), capacity);
    for (size_t index = 0; index < count; ++index) {
        if (copier(source[index], records[index]) != QLC_STATUS_OK) {
            status = QLC_STATUS_BUFFER_TOO_SMALL;
        }
    }
    if (capacity < source.size()) return QLC_STATUS_BUFFER_TOO_SMALL;
    return status;
}

QlcStatus validate_handle(QlcDiagLoggerHandle handle) {
    return handle && handle->implementation ? QLC_STATUS_OK : QLC_STATUS_INVALID_HANDLE;
}

template <typename Fn>
QlcStatus logger_call(QlcDiagLoggerHandle handle, Fn&& fn) {
    const QlcStatus valid = validate_handle(handle);
    if (valid != QLC_STATUS_OK) return valid;
    try {
        return fn(*handle->implementation);
    } catch (const std::exception& error) {
        handle->last_error = error.what();
        return QLC_STATUS_INTERNAL_ERROR;
    } catch (...) {
        handle->last_error = "Unknown C++ exception";
        return QLC_STATUS_INTERNAL_ERROR;
    }
}
} // namespace

QLC_C_API uint32_t qlc_abi_version(void) {
    configure_console_for_host();
    return QLC_C_ABI_VERSION;
}

QLC_C_API void qlc_diag_logger_config_init(QlcDiagLoggerConfig* config) {
    configure_console_for_host();
    if (!config) return;
    std::memset(config, 0, sizeof(*config));
    config->baud_rate = 115200;
    config->command_timeout_ms = 2000;
    config->enable_recovery = 1;
    config->recovery_timeout_seconds = 120;
    config->qdss_processor_mask = 0x3F;
    config->adpl_version = 7;
}

QLC_C_API QlcStatus qlc_last_error(char* buffer, size_t capacity, size_t* required_size) {
    configure_console_for_host();
    std::lock_guard<std::mutex> lock(g_error_mutex);
    return copy_text(g_last_error, buffer, capacity, required_size);
}

QLC_C_API QlcStatus qlc_device_discovery_start_monitoring(uint32_t wait_ms) {
    configure_console_for_host();
    try { g_discovery.start_monitor(wait_ms); return QLC_STATUS_OK; }
    catch (const std::exception& error) { set_error(error.what()); return QLC_STATUS_INTERNAL_ERROR; }
    catch (...) { set_error("Unknown C++ exception"); return QLC_STATUS_INTERNAL_ERROR; }
}

QLC_C_API QlcStatus qlc_device_discovery_stop_monitoring(void) {
    configure_console_for_host();
    try { g_discovery.stop_monitor(); return QLC_STATUS_OK; }
    catch (const std::exception& error) { set_error(error.what()); return QLC_STATUS_INTERNAL_ERROR; }
    catch (...) { set_error("Unknown C++ exception"); return QLC_STATUS_INTERNAL_ERROR; }
}

QLC_C_API QlcStatus qlc_device_discovery_is_monitoring(uint8_t* running) {
    configure_console_for_host();
    if (!running) return QLC_STATUS_INVALID_ARGUMENT;
    try { *running = g_discovery.is_monitor_running() ? 1 : 0; return QLC_STATUS_OK; }
    catch (...) { return QLC_STATUS_INTERNAL_ERROR; }
}

QLC_C_API QlcStatus qlc_device_discovery_get_devices(
    QlcDeviceInfo* records, size_t capacity, size_t* written, size_t* required) {
    configure_console_for_host();
    try {
        const auto devices = g_discovery.get_device_list();
        return copy_records(devices, records, capacity, written, required, copy_device);
    } catch (const std::exception& error) { set_error(error.what()); return QLC_STATUS_INTERNAL_ERROR; }
    catch (...) { set_error("Unknown C++ exception"); return QLC_STATUS_INTERNAL_ERROR; }
}

QLC_C_API QlcStatus qlc_device_discovery_get_protocols(
    QlcProtocolInfo* records, size_t capacity, size_t* written, size_t* required) {
    configure_console_for_host();
    try {
        const auto devices = g_discovery.get_device_list();
        std::vector<QC::ProtocolInfo> protocols;
        for (const auto& device : devices) {
            protocols.insert(protocols.end(), device.protocols.begin(), device.protocols.end());
        }
        return copy_records(protocols, records, capacity, written, required, copy_protocol);
    } catch (const std::exception& error) { set_error(error.what()); return QLC_STATUS_INTERNAL_ERROR; }
    catch (...) { set_error("Unknown C++ exception"); return QLC_STATUS_INTERNAL_ERROR; }
}

QLC_C_API QlcDiagLoggerHandle qlc_diag_logger_create(const QlcDiagLoggerConfig* config) {
    configure_console_for_host();
    if (!config) { set_error("config must not be null"); return nullptr; }
    try {
        auto* handle = new QlcDiagLoggerHandleOpaque;
        handle->implementation = std::make_unique<QC::DiagLogger>(make_config(*config));
        return handle;
    } catch (const std::exception& error) { set_error(error.what()); return nullptr; }
    catch (...) { set_error("Unknown C++ exception"); return nullptr; }
}

QLC_C_API void qlc_diag_logger_destroy(QlcDiagLoggerHandle handle) {
    configure_console_for_host();
    delete handle;
}

QLC_C_API QlcStatus qlc_diag_logger_last_error(
    QlcDiagLoggerHandle handle, char* buffer, size_t capacity, size_t* required_size) {
    configure_console_for_host();
    const QlcStatus valid = validate_handle(handle);
    if (valid != QLC_STATUS_OK) return valid;
    return copy_text(handle->last_error, buffer, capacity, required_size);
}

QLC_C_API QlcStatus qlc_diag_logger_request_open(QlcDiagLoggerHandle handle, int32_t type) {
    configure_console_for_host();
    return logger_call(handle, [type](QC::DiagLogger& logger) {
        logger.request_open(protocol_type(type)); return QLC_STATUS_OK;
    });
}

QLC_C_API QlcStatus qlc_diag_logger_request_close(QlcDiagLoggerHandle handle, int32_t type) {
    configure_console_for_host();
    return logger_call(handle, [type](QC::DiagLogger& logger) {
        logger.request_close(protocol_type(type)); return QLC_STATUS_OK;
    });
}

QLC_C_API QlcStatus qlc_diag_logger_wait_ready(QlcDiagLoggerHandle handle, uint32_t timeout_ms, int32_t type) {
    configure_console_for_host();
    return logger_call(handle, [=](QC::DiagLogger& logger) {
        return logger.wait_ready(std::chrono::milliseconds(timeout_ms), protocol_type(type))
            ? QLC_STATUS_OK : QLC_STATUS_TIMEOUT;
    });
}

QLC_C_API QlcStatus qlc_diag_logger_wait_closed(QlcDiagLoggerHandle handle, uint32_t timeout_ms, int32_t type) {
    configure_console_for_host();
    return logger_call(handle, [=](QC::DiagLogger& logger) {
        return logger.wait_closed(std::chrono::milliseconds(timeout_ms), protocol_type(type))
            ? QLC_STATUS_OK : QLC_STATUS_TIMEOUT;
    });
}

QLC_C_API QlcStatus qlc_diag_logger_protocol_state(
    QlcDiagLoggerHandle handle, int32_t type, int32_t* state) {
    configure_console_for_host();
    if (!state) return QLC_STATUS_INVALID_ARGUMENT;
    return logger_call(handle, [=](QC::DiagLogger& logger) {
        *state = static_cast<int32_t>(logger.protocol_state(protocol_type(type)));
        return QLC_STATUS_OK;
    });
}

QLC_C_API QlcStatus qlc_diag_logger_validate_ready(
    QlcDiagLoggerHandle handle, uint32_t timeout_ms) {
    configure_console_for_host();
    return logger_call(handle, [timeout_ms](QC::DiagLogger& logger) {
        logger.validate_diag_ready(std::chrono::milliseconds(timeout_ms));
        return QLC_STATUS_OK;
    });
}

QLC_C_API QlcStatus qlc_diag_logger_start(QlcDiagLoggerHandle handle) {
    configure_console_for_host();
    return logger_call(handle, [](QC::DiagLogger& logger) {
        return logger.start() ? QLC_STATUS_OK : QLC_STATUS_NOT_READY;
    });
}

QLC_C_API QlcStatus qlc_diag_logger_stop(QlcDiagLoggerHandle handle) {
    configure_console_for_host();
    return logger_call(handle, [](QC::DiagLogger& logger) { logger.stop(); return QLC_STATUS_OK; });
}

QLC_C_API QlcStatus qlc_diag_logger_is_running(QlcDiagLoggerHandle handle, uint8_t* running) {
    configure_console_for_host();
    if (!running) return QLC_STATUS_INVALID_ARGUMENT;
    return logger_call(handle, [running](QC::DiagLogger& logger) {
        *running = logger.is_running() ? 1 : 0; return QLC_STATUS_OK;
    });
}

QLC_C_API QlcStatus qlc_diag_logger_stats(QlcDiagLoggerHandle handle, QlcDiagStats* stats) {
    configure_console_for_host();
    if (!stats) return QLC_STATUS_INVALID_ARGUMENT;
    return logger_call(handle, [stats](QC::DiagLogger& logger) {
        const QC::DiagStats source = logger.stats();
        stats->rx_packets = source.rx_packets;
        stats->rx_bytes = source.rx_bytes;
        stats->tx_packets = source.tx_packets;
        stats->tx_bytes = source.tx_bytes;
        stats->frame_errors = source.frame_errors;
        return QLC_STATUS_OK;
    });
}
