// SPDX-License-Identifier: Apache-2.0
// Copyright 2024 Qualcomm Innovation Center, Inc.

#include "cli_help.h"
#include "cli_argument_definitions.h"

#include <sstream>

namespace diaglogger {

std::string CliHelp::generate_help(const std::string& prog_name) {
    std::ostringstream out;

    out << "Usage: " << prog_name << " <command> [options]\n\n";
    out << "Commands:\n";

    const auto& commands = CliArgumentDefinitions::get_command_definitions();
    for (const auto& cmd : commands) {
        out << "  --" << cmd.name;
        size_t pad = 20 - cmd.name.size();
        for (size_t i = 0; i < pad; ++i) out << ' ';
        out << cmd.description << "\n";
    }

    out << "  --help                Show this help message\n";
    out << "  --version             Show tool version and exit\n";
    out << "\n";

    // collect-log options
    out << "Options for --collect-log:\n";
    out << "  Required:\n";

    const auto& args = CliArgumentDefinitions::get_argument_definitions();
    for (const auto& [name, def] : args) {
        if (def.category != ArgumentCategory::COLLECT_LOG_OPTIONS) continue;
        if (!def.required) continue;

        out << "    --" << name;
        if (def.type != ArgumentType::FLAG) {
            out << "=<value>";
        }
        size_t total = name.size() + (def.type != ArgumentType::FLAG ? 8 : 0);
        size_t pad_len = (total < 26) ? (26 - total) : 2;
        for (size_t i = 0; i < pad_len; ++i) out << ' ';
        out << def.description;
        if (!def.default_value.empty()) {
            out << " (default: " << def.default_value << ")";
        }
        out << "\n";
    }

    out << "\n  Duration (one required):\n";
    for (const auto& [name, def] : args) {
        if (def.category != ArgumentCategory::COLLECT_LOG_OPTIONS) continue;
        if (name.rfind("duration", 0) != 0) continue;

        out << "    --" << name;
        if (def.type != ArgumentType::FLAG) {
            out << "=<value>";
        }
        size_t total = name.size() + (def.type != ArgumentType::FLAG ? 8 : 0);
        size_t pad_len = (total < 26) ? (26 - total) : 2;
        for (size_t i = 0; i < pad_len; ++i) out << ' ';
        out << def.description;
        if (!def.default_value.empty()) {
            out << " (default: " << def.default_value << ")";
        }
        out << "\n";
    }

    out << "\n  Optional:\n";
    for (const auto& [name, def] : args) {
        if (def.category != ArgumentCategory::COLLECT_LOG_OPTIONS) continue;
        if (def.required) continue;
        if (name.rfind("duration", 0) == 0) continue;

        out << "    --" << name;
        if (def.type != ArgumentType::FLAG) {
            out << "=<value>";
        }
        size_t total = name.size() + (def.type != ArgumentType::FLAG ? 8 : 0);
        size_t pad_len = (total < 26) ? (26 - total) : 2;
        for (size_t i = 0; i < pad_len; ++i) out << ' ';
        out << def.description;
        if (!def.default_value.empty()) {
            out << " (default: " << def.default_value << ")";
        }
        out << "\n";
    }

    out << "\nExamples:\n";
    for (const auto& cmd : commands) {
        out << "  " << cmd.example << "\n";
    }

    return out.str();
}

} // namespace diaglogger
