// SPDX-License-Identifier: Apache-2.0
// Copyright 2024 Qualcomm Innovation Center, Inc.
//
// qlc (qualcomm-log-collector) CLI entry point.
//
// Usage:
//   qlc --collect-log --cfg=<file.cfg> --device=<device_name>
//       --duration=60 [--out=<dir>] [--baud=38400] [--verbose]
//   qlc --devices

#include "qlc/cli_parser.h"
#include "qlc/cli_help.h"
#include "logger/diag_logger.h"
#include "transport/transport_factory.h"
#include "transport/transport_manager.h"
#include "diaglogger/version.h"

#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdlib>
#include <ctime>
#include <iomanip>
#include <iostream>
#include <KL/kLogger.h>
#include <KL/Macros.h>
#include <string>
#include <sstream>

static std::atomic<bool> g_should_stop{false};
static diaglogger::DiagLogger* g_logger = nullptr;

static void signal_handler(int /*sig*/) {
    g_should_stop.store(true);
    if (g_logger) {
        g_logger->stop();
    }
}

static diaglogger::DiagLoggerConfig to_logger_config(const diaglogger::CliOptions& opts)
{
    diaglogger::DiagLoggerConfig config;
    config.cfg_path = opts.cfg_path;
    config.port_name = opts.port_name;
    config.output_path = opts.output_path;
    config.baud_rate = opts.baud_rate;
    config.duration_seconds = opts.duration_seconds;
    config.verbose = opts.verbose;
    config.hdlc_only = opts.hdlc_only;
    config.enable_recovery = opts.enable_recovery;
    config.recovery_timeout_seconds = opts.recovery_timeout_seconds;
    return config;
}

int main(int argc, char* argv[]) {
    diaglogger::CliOptions opts;

    try {
        opts = diaglogger::CliParser::parse(argc, argv);
    } catch (const std::invalid_argument& e) {
        CLOG_ERROR(std::string("Error: ") + e.what());
        CLOG_INFO("\nUse --help for usage information.");
        return 1;
    }

    if (opts.command == diaglogger::CommandType::HELP) {
        CLOG_INFO(diaglogger::CliHelp::generate_help(argv[0]));
        return 0;
    }

    if (opts.command == diaglogger::CommandType::DISPLAY_VERSION) {
        CLOG_INFO(std::string("qlc ") + diaglogger::kToolVersion);
        return 0;
    }

    // Initialize kLogger
    try {
        std::string ptrace_dir = opts.has_path ? opts.output_path : std::string(PTRACE_DIR);
        KL::Logger::get_instance().init(LOG_DIR, ptrace_dir, 0, KL::MAX_LOG_FILE_SIZE);
        CFLOG_INFO("kLogger initialized successfully. PTRACE_DIR:" + ptrace_dir, true);
    } catch (const std::exception& e) {
        CFLOG_ERROR(std::string("Exception initializing kLogger: ") + e.what(), true);
    }

    diaglogger::TransportManager::instance().start_monitor();

    if (opts.command == diaglogger::CommandType::LIST_DEVICES) {
        CFLOG_INFO("Scanning for QcDevice devices...", true);
        auto devices = diaglogger::enumerate_qcdevices();
        if (devices.empty()) {
            CFLOG_WARNING("No QcDevice devices found.", true);
            diaglogger::TransportManager::instance().stop_monitor();
            KL::Logger::get_instance().flush_and_shutdown();
            return 0;
        }
        CFLOG_INFO("Found " + std::to_string(devices.size()) + " QcDevice device(s):\n", true);
        for (size_t i = 0; i < devices.size(); ++i) {
            const auto& dev = devices[i];
            CFLOG_INFO("  [" + std::to_string(i) + "] " + dev.description, true);
        }
        CLOG_INFO("\nUse --collect-log --device=\"<description>\" to connect to a device.");
        diaglogger::TransportManager::instance().stop_monitor();
        KL::Logger::get_instance().flush_and_shutdown();
        return 0;
    }

    // --- collect-log command ---
    auto config = to_logger_config(opts);

    std::signal(SIGINT, signal_handler);
#ifndef TOOLS_TARGET_WINDOWS
    std::signal(SIGTERM, signal_handler);
#endif

    diaglogger::DiagLogger logger(config);
    g_logger = &logger;

    if (!logger.open_transport()) {
        g_logger = nullptr;
        diaglogger::TransportManager::instance().stop_monitor();
        KL::Logger::get_instance().flush_and_shutdown();
        return 1;
    }

    if (!logger.start()) {
        logger.close_transport();
        g_logger = nullptr;
        diaglogger::TransportManager::instance().stop_monitor();
        KL::Logger::get_instance().flush_and_shutdown();
        return 1;
    }

    while (!g_should_stop.load() && logger.is_running()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }

    if (logger.is_running()) {
        logger.stop();
    }

    logger.close_transport();
    g_logger = nullptr;

    diaglogger::TransportManager::instance().stop_monitor();
    KL::Logger::get_instance().flush_and_shutdown();
    return 0;
}
