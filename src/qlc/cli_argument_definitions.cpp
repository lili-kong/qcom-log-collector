// SPDX-License-Identifier: Apache-2.0
// Copyright 2024 Qualcomm Innovation Center, Inc.

#include "cli_argument_definitions.h"

namespace diaglogger {

std::map<std::string, ArgumentDefinition> CliArgumentDefinitions::argument_definitions_;
std::vector<CommandDefinition> CliArgumentDefinitions::command_definitions_;
bool CliArgumentDefinitions::initialized_ = false;

void CliArgumentDefinitions::initialize() {
    if (initialized_) return;

    // --- Command definitions ---
    command_definitions_ = {
        {
            "devices",
            "List connected QcDevice diag devices and exit",
            {},  // no required args
            {},  // no optional args
            "qlc --devices",
            CommandType::LIST_DEVICES
        },
        {
            "collect-log",
            "Collect diagnostic logs from a device",
            {"cfg", "device", "duration|duration-minutes|duration-hours"},
            {"out", "baud", "verbose", "hdlc", "no-recovery", "recovery-timeout"},
            "qlc --collect-log --cfg=mask.cfg --device=\"Qualcomm HS-USB\" --duration=60",
            CommandType::COLLECT_LOG
        }
    };

    // --- Argument definitions (options for commands) ---
    argument_definitions_ = {
        {"cfg", {
            "cfg",
            "Path to CFG mask configuration file",
            ArgumentType::PATH,
            ArgumentCategory::COLLECT_LOG_OPTIONS,
            "",
            true,
            "--cfg=mask.cfg",
            [](CliOptions& opts, const std::string& val) {
                opts.cfg_path = val;
                opts.has_cfg = true;
            }
        }},
        {"device", {
            "device",
            "QcDevice name (use --devices to find)",
            ArgumentType::STRING,
            ArgumentCategory::COLLECT_LOG_OPTIONS,
            "",
            true,
            "--device=\"Qualcomm HS-USB Android DIAG 9091 (COM3)\"",
            [](CliOptions& opts, const std::string& val) {
                opts.port_name = val;
                opts.has_port = true;
            }
        }},
        {"duration", {
            "duration",
            "Auto-stop after N seconds",
            ArgumentType::INTEGER,
            ArgumentCategory::COLLECT_LOG_OPTIONS,
            "",
            false,
            "--duration=60",
            [](CliOptions& opts, const std::string& val) {
                opts.duration_seconds = std::stoi(val);
                opts.has_duration = true;
            }
        }},
        {"duration-minutes", {
            "duration-minutes",
            "Auto-stop after N minutes",
            ArgumentType::INTEGER,
            ArgumentCategory::COLLECT_LOG_OPTIONS,
            "",
            false,
            "--duration-minutes=5",
            [](CliOptions& opts, const std::string& val) {
                opts.duration_seconds = std::stoi(val) * 60;
                opts.has_duration = true;
            }
        }},
        {"duration-hours", {
            "duration-hours",
            "Auto-stop after N hours",
            ArgumentType::INTEGER,
            ArgumentCategory::COLLECT_LOG_OPTIONS,
            "",
            false,
            "--duration-hours=1",
            [](CliOptions& opts, const std::string& val) {
                opts.duration_seconds = std::stoi(val) * 3600;
                opts.has_duration = true;
            }
        }},
        {"out", {
            "out",
            "Path to store ptrace logs",
            ArgumentType::PATH,
            ArgumentCategory::COLLECT_LOG_OPTIONS,
            "",
            false,
            "--out=C:\\Logs\\PTrace",
            [](CliOptions& opts, const std::string& val) {
                opts.output_path = val;
                opts.has_path = true;
            }
        }},
        {"baud", {
            "baud",
            "Baud rate",
            ArgumentType::INTEGER,
            ArgumentCategory::COLLECT_LOG_OPTIONS,
            "115200",
            false,
            "--baud=115200",
            [](CliOptions& opts, const std::string& val) {
                opts.baud_rate = static_cast<uint32_t>(std::stoul(val));
            }
        }},
        {"verbose", {
            "verbose",
            "Print decoded command IDs to stderr",
            ArgumentType::FLAG,
            ArgumentCategory::COLLECT_LOG_OPTIONS,
            "",
            false,
            "--verbose",
            [](CliOptions& opts, const std::string&) {
                opts.verbose = true;
            }
        }},
        {"hdlc", {
            "hdlc",
            "Force HDLC-only mode (skip Non-HDLC framing switch)",
            ArgumentType::FLAG,
            ArgumentCategory::COLLECT_LOG_OPTIONS,
            "",
            false,
            "--hdlc",
            [](CliOptions& opts, const std::string&) {
                opts.hdlc_only = true;
            }
        }},
        {"no-recovery", {
            "no-recovery",
            "Disable automatic reboot recovery (enabled by default)",
            ArgumentType::FLAG,
            ArgumentCategory::COLLECT_LOG_OPTIONS,
            "",
            false,
            "--no-recovery",
            [](CliOptions& opts, const std::string&) {
                opts.enable_recovery = false;
            }
        }},
        {"recovery-timeout", {
            "recovery-timeout",
            "Max seconds to wait for device recovery",
            ArgumentType::INTEGER,
            ArgumentCategory::COLLECT_LOG_OPTIONS,
            "120",
            false,
            "--recovery-timeout=120",
            [](CliOptions& opts, const std::string& val) {
                opts.recovery_timeout_seconds = std::stoi(val);
            }
        }}
    };

    initialized_ = true;
}

const std::map<std::string, ArgumentDefinition>& CliArgumentDefinitions::get_argument_definitions() {
    initialize();
    return argument_definitions_;
}

const std::vector<CommandDefinition>& CliArgumentDefinitions::get_command_definitions() {
    initialize();
    return command_definitions_;
}

const ArgumentDefinition* CliArgumentDefinitions::get_argument_definition(const std::string& name) {
    initialize();
    auto it = argument_definitions_.find(name);
    if (it != argument_definitions_.end()) {
        return &it->second;
    }
    return nullptr;
}

const CommandDefinition* CliArgumentDefinitions::get_command_definition(const std::string& name) {
    initialize();
    for (const auto& cmd : command_definitions_) {
        if (cmd.name == name) {
            return &cmd;
        }
    }
    return nullptr;
}

} // namespace diaglogger
