// Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
// SPDX-License-Identifier: BSD 3-Clause Clear License
//
// Command-line argument parser with two-level command + option support.

#ifndef DIAGLOGGER_CLI_PARSER_H
#define DIAGLOGGER_CLI_PARSER_H

#include "cli_options.h"

#include <string>
#include <vector>

namespace diaglogger {

class CliParser {
public:
    static CliOptions parse(int argc, char* argv[]);

private:
    struct ParsedArg {
        std::string key;
        std::string value;
    };

    static ParsedArg extract_option(const std::string& arg);
    static bool is_command(const std::string& name);
    static void validate_collect_log_options(const CliOptions& opts);
};

} // namespace diaglogger

#endif // DIAGLOGGER_CLI_PARSER_H
