// Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
// SPDX-License-Identifier: BSD 3-Clause Clear License
//
// Command-line argument parser with two-level command + option support.

#ifndef QC_CLI_PARSER_H
#define QC_CLI_PARSER_H

#include <string>
#include <vector>

#include "cli_options.h"

namespace QC {

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

} // namespace QC

#endif // QC_CLI_PARSER_H
