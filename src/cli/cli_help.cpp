// Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
// SPDX-License-Identifier: BSD 3-Clause Clear License

#include <algorithm>
#include <sstream>
#include <vector>

#include "cli_help.h"
#include "cli_argument_definitions.h"

namespace QC {

namespace {

constexpr size_t MIN_PAD = 2;
constexpr const char* VALUE_SUFFIX = "=<value>";
constexpr size_t VALUE_SUFFIX_LEN = 8;

size_t compute_column_width(const std::vector<std::string>& names,
                            const std::map<std::string, ArgumentDefinition>& defs) {
    size_t max_width = 0;
    for (const auto& name : names) {
        auto it = defs.find(name);
        size_t width = name.size();
        if (it != defs.end() && it->second.type != ArgumentType::FLAG) {
            width += VALUE_SUFFIX_LEN;
        }
        max_width = std::max(max_width, width);
    }
    return max_width + MIN_PAD;
}

void emit_option(std::ostringstream& out, const std::string& name,
                 const ArgumentDefinition& def, size_t column_width,
                 const std::string& indent) {
    out << indent << "--" << name;
    size_t total = name.size();
    if (def.type != ArgumentType::FLAG) {
        out << VALUE_SUFFIX;
        total += VALUE_SUFFIX_LEN;
    }
    size_t pad_len = (total < column_width) ? (column_width - total) : MIN_PAD;
    for (size_t i = 0; i < pad_len; ++i) {
        out << ' ';
    }
    out << def.description;
    if (!def.default_value.empty()) {
        out << " (default: " << def.default_value << ")";
    }
    out << "\n";
}

} // namespace

std::string CliHelp::generate_help(const std::string& prog_name) {
    std::ostringstream out;
    const auto& commands = CliArgumentDefinitions::get_command_definitions();
    const auto& args = CliArgumentDefinitions::get_argument_definitions();

    out << "Usage: " << prog_name << " <command> [options]\n\n";
    out << "Commands:\n";

    size_t cmd_col = 0;
    for (const auto& cmd : commands) {
        cmd_col = std::max(cmd_col, cmd.name.size());
    }
    cmd_col += MIN_PAD;

    for (const auto& cmd : commands) {
        out << "  --" << cmd.name;
        size_t pad = (cmd.name.size() < cmd_col) ? (cmd_col - cmd.name.size()) : MIN_PAD;
        for (size_t i = 0; i < pad; ++i) {
            out << ' ';
        }
        out << cmd.description << "\n";
    }

    out << "  --help";
    size_t help_pad = (4 < cmd_col) ? (cmd_col - 4) : MIN_PAD;
    for (size_t i = 0; i < help_pad; ++i) {
        out << ' ';
    }
    out << "Show this help message\n";

    out << "  --version";
    size_t ver_pad = (7 < cmd_col) ? (cmd_col - 7) : MIN_PAD;
    for (size_t i = 0; i < ver_pad; ++i) {
        out << ' ';
    }
    out << "Show tool version and exit\n";
    out << "\n";

    // collect-log options
    out << "Options for --collect-log:\n";

    // Collect names per section to compute column widths
    std::vector<std::string> required_names;
    std::vector<std::string> duration_names;
    std::vector<std::string> optional_names;

    for (const auto& [name, def] : args) {
        if (def.category != ArgumentCategory::COLLECT_LOG_OPTIONS) {
            continue;
        }
        if (name.rfind("duration", 0) == 0) {
            duration_names.push_back(name);
        } else if (def.required) {
            required_names.push_back(name);
        } else {
            optional_names.push_back(name);
        }
    }

    // Compute a single column width across all collect-log options
    std::vector<std::string> all_names;
    all_names.insert(all_names.end(), required_names.begin(), required_names.end());
    all_names.insert(all_names.end(), duration_names.begin(), duration_names.end());
    all_names.insert(all_names.end(), optional_names.begin(), optional_names.end());
    size_t col = compute_column_width(all_names, args);

    out << "  Required:\n";
    for (const auto& name : required_names) {
        emit_option(out, name, args.at(name), col, "    ");
    }

    out << "\n  Duration (one required):\n";
    for (const auto& name : duration_names) {
        emit_option(out, name, args.at(name), col, "    ");
    }

    out << "\n  Optional:\n";
    for (const auto& name : optional_names) {
        emit_option(out, name, args.at(name), col, "    ");
    }

    out << "\nExamples:\n";
    for (const auto& cmd : commands) {
        out << "  " << cmd.example << "\n";
    }

    return out.str();
}

} // namespace QC
