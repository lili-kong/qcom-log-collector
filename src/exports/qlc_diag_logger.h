// Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
// SPDX-License-Identifier: BSD 3-Clause Clear License
#pragma once

#include "qlc_export.h"

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define QLC_C_ABI_VERSION 1u

typedef struct QlcDiagLoggerHandleOpaque* QlcDiagLoggerHandle;

typedef enum QlcStatus {
    QLC_STATUS_OK = 0,
    QLC_STATUS_INVALID_ARGUMENT = 1,
    QLC_STATUS_INVALID_HANDLE = 2,
    QLC_STATUS_BUFFER_TOO_SMALL = 3,
    QLC_STATUS_TIMEOUT = 4,
    QLC_STATUS_NOT_READY = 5,
    QLC_STATUS_INTERNAL_ERROR = 6
} QlcStatus;

/* Values match QC::ProtocolType in exports/Definitions.h. */
enum {
    QLC_PROT_UNKNOWN = -1,
    QLC_PROT_SAHARA = 0,
    QLC_PROT_FIREHOSE,
    QLC_PROT_DIAG,
    QLC_PROT_ADPL,
    QLC_PROT_QDSS,
    QLC_PROT_QMI,
    QLC_PROT_ADB,
    QLC_PROT_FASTBOOT,
    QLC_PROT_NMEA,
    QLC_PROT_DUN,
    QLC_PROT_RMNET,
    QLC_PROT_RNDIS,
    QLC_PROT_MBIM,
    QLC_PROT_PROTOBUF,
    QLC_PROT_TAC,
    QLC_PROT_EPM,
    QLC_PROT_TELNET,
    QLC_PROT_FTP,
    QLC_PROT_EUD,
    QLC_PROT_QSPS,
    QLC_PROT_SERIAL,
    QLC_PROT_PROMIRA,
    QLC_PROT_MAX
};

enum {
    QLC_STATE_AVAILABLE = 0,
    QLC_STATE_DISCONNECTED = 1,
    QLC_STATE_UNRESPONSIVE = 2,
    QLC_STATE_INITIALIZING = 3
};

typedef struct QlcDeviceInfo {
    uint64_t device_handle;
    char* description; size_t description_capacity;
    char* serial_number; size_t serial_number_capacity;
    char* adb_serial_number; size_t adb_serial_number_capacity;
    char* location; size_t location_capacity;
    char* vid; size_t vid_capacity;
    char* pid; size_t pid_capacity;
    char* edl_chip_id; size_t edl_chip_id_capacity;
    char* device_physical_location; size_t device_physical_location_capacity;
    char* soc_chip_name; size_t soc_chip_name_capacity;
    char* soc_version; size_t soc_version_capacity;
} QlcDeviceInfo;

typedef struct QlcProtocolInfo {
    uint64_t protocol_handle;
    uint64_t device_handle;
    int32_t protocol_type;
    int32_t connection_type;
    int32_t connection_status;
    int32_t share_status;
    int32_t protocol_state;
    uint32_t local_port;
    uint32_t remote_port;
    char* description; size_t description_capacity;
    char* alternate_description; size_t alternate_description_capacity;
    char* com_port; size_t com_port_capacity;
    char* parent_device; size_t parent_device_capacity;
    char* parent_location; size_t parent_location_capacity;
    char* adb_serial_number; size_t adb_serial_number_capacity;
    char* msm_serial_number; size_t msm_serial_number_capacity;
    char* vid; size_t vid_capacity;
    char* pid; size_t pid_capacity;
    char* edl_chip_id; size_t edl_chip_id_capacity;
    char* soc_version; size_t soc_version_capacity;
    uint32_t flags;
} QlcProtocolInfo;

typedef struct QlcDiagLoggerConfig {
    const char* cfg_path;
    const char* port_name;
    const char* qdss_port_name;
    const char* adpl_port_name;
    const char* output_path;
    uint32_t baud_rate;
    int32_t duration_seconds;
    uint32_t command_timeout_ms;
    uint8_t verbose;
    uint8_t hdlc_only;
    uint8_t enable_recovery;
    int32_t recovery_timeout_seconds;
    uint8_t enable_qdss;
    uint8_t enable_adpl;
    uint8_t qdss_processor_mask;
    uint8_t adpl_version;
} QlcDiagLoggerConfig;

typedef struct QlcDiagStats {
    uint64_t rx_packets;
    uint64_t rx_bytes;
    uint64_t tx_packets;
    uint64_t tx_bytes;
    uint32_t frame_errors;
} QlcDiagStats;

QLC_C_API uint32_t qlc_abi_version(void);
QLC_C_API void qlc_diag_logger_config_init(QlcDiagLoggerConfig* config);
QLC_C_API QlcStatus qlc_last_error(char* buffer, size_t capacity, size_t* required_size);

QLC_C_API QlcStatus qlc_device_discovery_start_monitoring(uint32_t wait_ms);
QLC_C_API QlcStatus qlc_device_discovery_stop_monitoring(void);
QLC_C_API QlcStatus qlc_device_discovery_is_monitoring(uint8_t* running);
QLC_C_API QlcStatus qlc_device_discovery_get_devices(
    QlcDeviceInfo* records, size_t capacity, size_t* written, size_t* required);
QLC_C_API QlcStatus qlc_device_discovery_get_protocols(
    QlcProtocolInfo* records, size_t capacity, size_t* written, size_t* required);

QLC_C_API QlcDiagLoggerHandle qlc_diag_logger_create(const QlcDiagLoggerConfig* config);
QLC_C_API void qlc_diag_logger_destroy(QlcDiagLoggerHandle handle);
QLC_C_API QlcStatus qlc_diag_logger_last_error(
    QlcDiagLoggerHandle handle, char* buffer, size_t capacity, size_t* required_size);
QLC_C_API QlcStatus qlc_diag_logger_request_open(QlcDiagLoggerHandle handle, int32_t protocol_type);
QLC_C_API QlcStatus qlc_diag_logger_request_close(QlcDiagLoggerHandle handle, int32_t protocol_type);
QLC_C_API QlcStatus qlc_diag_logger_wait_ready(
    QlcDiagLoggerHandle handle, uint32_t timeout_ms, int32_t protocol_type);
QLC_C_API QlcStatus qlc_diag_logger_wait_closed(
    QlcDiagLoggerHandle handle, uint32_t timeout_ms, int32_t protocol_type);
QLC_C_API QlcStatus qlc_diag_logger_protocol_state(
    QlcDiagLoggerHandle handle, int32_t protocol_type, int32_t* state);
QLC_C_API QlcStatus qlc_diag_logger_validate_ready(
    QlcDiagLoggerHandle handle, uint32_t timeout_ms);
QLC_C_API QlcStatus qlc_diag_logger_start(QlcDiagLoggerHandle handle);
QLC_C_API QlcStatus qlc_diag_logger_stop(QlcDiagLoggerHandle handle);
QLC_C_API QlcStatus qlc_diag_logger_is_running(QlcDiagLoggerHandle handle, uint8_t* running);
QLC_C_API QlcStatus qlc_diag_logger_stats(QlcDiagLoggerHandle handle, QlcDiagStats* stats);

#ifdef __cplusplus
}
#endif
