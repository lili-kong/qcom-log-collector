// SPDX-License-Identifier: Apache-2.0
// Copyright 2024 Qualcomm Innovation Center, Inc.

#include "cli_parser.h"
#include "cli_argument_definitions.h"

#include <stdexcept>

namespace diaglogger {

CliParser::ParsedArg CliParser::extract_option(const std::string& arg) {
    if (arg.rfind("--", 0) != 0) {
        throw std::invalid_argument("Invalid argument format: " + arg + " (must start with --)");
    }

    std::string stripped = arg.substr(2);
    size_t eq_pos = stripped.find('=');

    if (eq_pos != std::string::npos) {
        return {stripped.substr(0, eq_pos), stripped.substr(eq_pos + 1)};
    }
    return {stripped, ""};
}

bool CliParser::is_command(const std::string& name) {
    const auto& commands = CliArgumentDefinitions::get_command_definitions();
    for (const auto& cmd : commands) {
        if (cmd.name == name) return true;
    }
    return (name == "help" || name == "version");
}

void CliParser::validate_collect_log_options(const CliOptions& opts) {
    if (!opts.has_cfg) {
        throw std::invalid_argument("Missing required option: --cfg=<file>");
    }
    if (!opts.has_port) {
        throw std::invalid_argument("Missing required option: --device=<device>");
    }
    if (!opts.has_duration) {
        throw std::invalid_argument(
            "Missing required option: --duration=<sec>, --duration-minutes=<N>, or --duration-hours=<N>");
    }
}

CliOptions CliParser::parse(int argc, char* argv[]) {
    CliOptions opts;

    if (argc < 2) {
        throw std::invalid_argument("No command specified. Use --help for usage information.");
    }

    // First pass: identify the command
    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];

        if (arg == "-h" || arg == "--help") {
            opts.command = CommandType::HELP;
            return opts;
        }
        if (arg == "-V" || arg == "--version") {
            opts.command = CommandType::DISPLAY_VERSION;
            return opts;
        }

        auto parsed = extract_option(arg);

        if (is_command(parsed.key)) {
            if (opts.command != CommandType::NONE) {
                throw std::invalid_argument("Multiple commands specified. Only one command is allowed.");
            }

            if (parsed.key == "devices") {
                opts.command = CommandType::LIST_DEVICES;
            } else if (parsed.key == "collect-log") {
                opts.command = CommandType::COLLECT_LOG;
            }
        }
    }

    if (opts.command == CommandType::NONE) {
        throw std::invalid_argument("No command specified. Use --devices or --collect-log.");
    }

    // Second pass: parse options for the identified command
    if (opts.command == CommandType::LIST_DEVICES) {
        for (int i = 1; i < argc; ++i) {
            std::string arg = argv[i];
            auto parsed = extract_option(arg);
            if (parsed.key != "devices") {
                throw std::invalid_argument("--devices command takes no options. Got: --" + parsed.key);
            }
        }
        return opts;
    }

    if (opts.command == CommandType::COLLECT_LOG) {
        for (int i = 1; i < argc; ++i) {
            std::string arg = argv[i];
            auto parsed = extract_option(arg);

            if (parsed.key == "collect-log") {
                continue;
            }

            const auto* arg_def = CliArgumentDefinitions::get_argument_definition(parsed.key);
            if (!arg_def) {
                throw std::invalid_argument("Unknown option: --" + parsed.key);
            }

            if (arg_def->type != ArgumentType::FLAG && parsed.value.empty()) {
                throw std::invalid_argument("Option --" + parsed.key + " requires a value (use --" + parsed.key + "=<value>)");
            }

            if (arg_def->type == ArgumentType::FLAG && !parsed.value.empty()) {
                throw std::invalid_argument("Option --" + parsed.key + " is a flag and does not accept a value");
            }

            arg_def->setter(opts, parsed.value);
        }

        validate_collect_log_options(opts);
    }

    return opts;
}

} // namespace diaglogger
