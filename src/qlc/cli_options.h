// Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
// SPDX-License-Identifier: BSD 3-Clause Clear License
//
// CLI parsed options data structure.

#ifndef DIAGLOGGER_CLI_OPTIONS_H
#define DIAGLOGGER_CLI_OPTIONS_H

#include <string>

namespace diaglogger {

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
    std::string port_name;
    std::string output_path;
    uint32_t baud_rate = 115200;
    int duration_seconds = 0;
    bool verbose = false;
    bool hdlc_only = false;
    bool enable_recovery = true;
    int recovery_timeout_seconds = 120;

    bool has_cfg = false;
    bool has_port = false;
    bool has_path = false;
    bool has_duration = false;
};

} // namespace diaglogger

#endif // DIAGLOGGER_CLI_OPTIONS_H
