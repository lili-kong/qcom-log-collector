// Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
// SPDX-License-Identifier: BSD 3-Clause Clear License
//
// Centralized CLI argument and command definitions (single source of truth).

#ifndef DIAGLOGGER_CLI_ARGUMENT_DEFINITIONS_H
#define DIAGLOGGER_CLI_ARGUMENT_DEFINITIONS_H

#include "cli_options.h"

#include <functional>
#include <map>
#include <string>
#include <vector>

namespace diaglogger {

enum class ArgumentType {
    FLAG,
    STRING,
    INTEGER,
    PATH
};

enum class ArgumentCategory {
    COMMANDS,
    COLLECT_LOG_OPTIONS,
    GENERAL
};

struct ArgumentDefinition {
    std::string name;
    std::string description;
    ArgumentType type;
    ArgumentCategory category;
    std::string default_value;
    bool required;
    std::string example;
    std::function<void(CliOptions&, const std::string&)> setter;
};

struct CommandDefinition {
    std::string name;
    std::string description;
    std::vector<std::string> required_args;
    std::vector<std::string> optional_args;
    std::string example;
    CommandType command_type;
};

class CliArgumentDefinitions {
public:
    static const std::map<std::string, ArgumentDefinition>& get_argument_definitions();
    static const std::vector<CommandDefinition>& get_command_definitions();
    static const ArgumentDefinition* get_argument_definition(const std::string& name);
    static const CommandDefinition* get_command_definition(const std::string& name);

private:
    static std::map<std::string, ArgumentDefinition> argument_definitions_;
    static std::vector<CommandDefinition> command_definitions_;
    static bool initialized_;
    static void initialize();
};

} // namespace diaglogger

#endif // DIAGLOGGER_CLI_ARGUMENT_DEFINITIONS_H
