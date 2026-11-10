// Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
// SPDX-License-Identifier: BSD 3-Clause Clear License

#include "exports/qlc_diag_logger.h"

#include <cassert>
#include <cstring>

int main() {
    assert(qlc_abi_version() == QLC_C_ABI_VERSION);

    QlcDiagLoggerConfig config;
    qlc_diag_logger_config_init(&config);
    assert(config.baud_rate == 115200);
    assert(config.command_timeout_ms == 2000);
    assert(config.enable_recovery == 1);

    size_t required = 0;
    assert(qlc_last_error(nullptr, 0, &required) == QLC_STATUS_OK);
    assert(required == 1);

    assert(qlc_device_discovery_is_monitoring(nullptr) == QLC_STATUS_INVALID_ARGUMENT);
    assert(qlc_diag_logger_create(nullptr) == nullptr);

    char error[64] = {};
    assert(qlc_last_error(error, sizeof(error), nullptr) == QLC_STATUS_OK);
    assert(std::strcmp(error, "config must not be null") == 0);

    assert(qlc_diag_logger_start(nullptr) == QLC_STATUS_INVALID_HANDLE);
    assert(qlc_diag_logger_stats(nullptr, reinterpret_cast<QlcDiagStats*>(1)) == QLC_STATUS_INVALID_HANDLE);
    return 0;
}
