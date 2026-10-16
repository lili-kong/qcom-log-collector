// Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
// SPDX-License-Identifier: BSD 3-Clause Clear License
//
// CLI parsed options data structure.

#ifndef QC_CLI_OPTIONS_H
#define QC_CLI_OPTIONS_H

#include <cstdint>
#include <string>

namespace QC {

enum class CommandType {
    NONE,
    LIST_DEVICES,
    COLLECT_LOG,
    HELP,
    DISPLAY_VERSION
};

struct CliOptions {
    CommandType command = CommandType::NONE;

    // collect-log options
    std::string cfg_path;
    std::string device_name;
    std::string port_name;
    std::string qdss_port_name;
    std::string adpl_port_name;
    std::string output_path;
    uint32_t baud_rate = 115200;
    int duration_seconds = 0;
    bool verbose = false;
    bool hdlc_only = false;
    bool enable_recovery = true;
    int recovery_timeout_seconds = 120;
    bool enable_qdss = false;
    bool enable_adpl = false;

    bool has_cfg = false;
    bool has_port = false;
    bool has_path = false;
    bool has_duration = false;
};

} // namespace QC

#endif // QC_CLI_OPTIONS_H
