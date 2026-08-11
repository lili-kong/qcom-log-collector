// SPDX-License-Identifier: Apache-2.0
// Copyright 2024 Qualcomm Innovation Center, Inc.
//
// qlc (qualcomm-log-collector) CLI entry point.
//
// Usage:
//   qlc --collect-log --cfg=<file.cfg> --device=<device_name>
//       --duration=60 [--out=<dir>] [--baud=38400] [--verbose]
//   qlc --devices

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

#include "qlc/cli_parser.h"
#include "qlc/cli_help.h"
#include "logger/diag_logger.h"
#include "transport/transport_factory.h"
#include "transport/transport_manager.h"
#include "diaglogger/version.h"

static std::atomic<bool> g_should_stop{false};
static QC::DiagLogger* g_logger = nullptr;

static void signal_handler(int /*sig*/) {
    // Only set the flag here; the main loop performs the graceful stop().
    // (stop() sends diag commands and joins threads — not safe from signal context.)
    g_should_stop.store(true);
}

static QC::DiagLoggerConfig to_logger_config(const QC::CliOptions& opts) {
    QC::DiagLoggerConfig config;
    config.cfg_path = opts.cfg_path;
    config.port_name = opts.port_name;
    config.output_path = opts.has_path ? opts.output_path : std::string(PTRACE_DIR);
    config.baud_rate = opts.baud_rate;
    config.duration_seconds = opts.duration_seconds;
    config.verbose = opts.verbose;
    config.hdlc_only = opts.hdlc_only;
    config.enable_recovery = opts.enable_recovery;
    config.recovery_timeout_seconds = opts.recovery_timeout_seconds;
    config.enable_qdss = opts.enable_qdss;
    config.qdss_port_name = opts.qdss_port_name;
    config.enable_adpl = opts.enable_adpl;
    config.adpl_port_name = opts.adpl_port_name;
    return config;
}

int main(int argc, char* argv[]) {
    QC::CliOptions opts;

    try {
        opts = QC::CliParser::parse(argc, argv);
    } catch (const std::invalid_argument& e) {
        CLOG_ERROR(std::string("Error: ") + e.what());
        CLOG_INFO("\nUse --help for usage information.");
        return 1;
    }

    if (opts.command == QC::CommandType::HELP) {
        CLOG_INFO(QC::CliHelp::generate_help(argv[0]));
        return 0;
    }

    if (opts.command == QC::CommandType::DISPLAY_VERSION) {
        CLOG_INFO(std::string("qlc ") + QC::kToolVersion);
        return 0;
    }

    if(!opts.has_path) {
        opts.output_path = std::string(PTRACE_DIR);
    }

    // Initialize kLogger
    try {
        KL::Logger::get_instance().init(LOG_DIR, opts.output_path, 0, KL::MAX_LOG_FILE_SIZE);
        std::string log_level = "INFO";
        if(opts.verbose) {
            KL::Logger::get_instance().setLevel(KL::Level::Debug);
            KL::Logger::get_instance().setOptions(KL::LogOption::DebugToFile);
            log_level = "DEBUG";
        }
        CFLOG_INFO("kLogger initialized successfully. PTRACE_DIR:" + opts.output_path + ", Log Level:" + log_level, true);
    } catch (const std::exception& e) {
        CFLOG_ERROR(std::string("Exception initializing kLogger: ") + e.what(), true);
    }

    QC::TransportManager::instance().start_monitor();

    if (opts.command == QC::CommandType::LIST_DEVICES) {
        CFLOG_INFO("Scanning for QcDevice devices...", true);
        auto composites = QC::enumerate_composite_qcdevices();
        if (composites.empty()) {
            CFLOG_WARNING("No QcDevice devices found.", true);
            QC::TransportManager::instance().stop_monitor();
            KL::Logger::get_instance().flush_and_shutdown();
            return 0;
        }
        CFLOG_INFO("Found " + std::to_string(composites.size()) + " device(s):\n", true);
        for (size_t i = 0; i < composites.size(); ++i) {
            const auto& dev = composites[i];
            CFLOG_INFO("  Device [" + std::to_string(i) + "] " + dev.description, true);
            if (!dev.adb_serial_number.empty()) {
                CFLOG_INFO("    ADB Serial Number: " + dev.adb_serial_number, true);
            }
            if (!dev.msm_serial_number.empty()) {
                CFLOG_INFO("    MSM Serial Number: " + dev.msm_serial_number, true);
            }
            CFLOG_INFO("    Available Protocols:", true);
            for (size_t j = 0; j < dev.protocols.size(); ++j) {
                const auto& proto = dev.protocols[j];
                std::string proto_str;
                switch (proto.protocol) {
                    case QC::PROT_DIAG: proto_str = " [DIAG]"; break;
                    case QC::PROT_QDSS: proto_str = " [QDSS]"; break;
                    case QC::PROT_ADPL: proto_str = " [ADPL]"; break;
                    default: break;
                }
                CFLOG_INFO("      [" + std::to_string(i) + "." + std::to_string(j) + "] " +
                           proto.description + proto_str, true);
            }
        }
        CLOG_INFO("\nUse --collect-log --device=\"<device description>\" to connect.");
        QC::TransportManager::instance().stop_monitor();
        KL::Logger::get_instance().flush_and_shutdown();
        return 0;
    }

    // --- collect-log command ---
    // Resolve --device: match the given value against composite device descriptions,
    // ADB serial numbers, or MSM serial numbers, then auto-select the DIAG protocol.
    if (opts.has_port && !opts.device_name.empty()) {
        auto composites = QC::enumerate_composite_qcdevices();
        const QC::QcCompositeDevice* matched_dev = nullptr;
        for (const auto& dev : composites) {
            if (dev.description == opts.device_name
                || dev.adb_serial_number == opts.device_name
                || dev.msm_serial_number == opts.device_name) {
                matched_dev = &dev;
                break;
            }
        }
        if (!matched_dev) {
            CFLOG_ERROR("Device not found: \"" + opts.device_name + "\". "
                        "Use --devices to list available devices.", true);
            QC::TransportManager::instance().stop_monitor();
            KL::Logger::get_instance().flush_and_shutdown();
            return 1;
        }
        std::string diag_name;
        std::string qdss_name;
        std::string adpl_name;
        for (const auto& proto : matched_dev->protocols) {
            if (diag_name.empty() && proto.protocol == QC::PROT_DIAG) {
                diag_name = proto.device_name;
            } else if (qdss_name.empty() && proto.protocol == QC::PROT_QDSS) {
                qdss_name = proto.device_name;
            } else if (adpl_name.empty() && proto.protocol == QC::PROT_ADPL) {
                adpl_name = proto.device_name;
            }
        }
        if (diag_name.empty()) {
            CFLOG_ERROR("No DIAG protocol found on device: " + matched_dev->description, true);
            QC::TransportManager::instance().stop_monitor();
            KL::Logger::get_instance().flush_and_shutdown();
            return 1;
        }
        CFLOG_INFO("Resolved device \"" + opts.device_name + "\" to DIAG protocol: " + diag_name, true);
        opts.port_name = diag_name;

        // Use the QDSS port from the same composite device if --enable-qdss set
        if (opts.enable_qdss) {
            if (qdss_name.empty()) {
                CFLOG_ERROR("No QDSS protocol found on device: " + matched_dev->description +
                            "\nUse --devices to verify QDSS is available.", true);
                QC::TransportManager::instance().stop_monitor();
                KL::Logger::get_instance().flush_and_shutdown();
                return 1;
            }
            CFLOG_INFO("Resolved QDSS protocol: " + qdss_name, true);
            opts.qdss_port_name = qdss_name;
        }

        // Use the ADPL port from the same composite device if --enable-adpl set
        if (opts.enable_adpl) {
            if (adpl_name.empty()) {
                CFLOG_ERROR("No ADPL protocol found on device: " + matched_dev->description +
                            "\nUse --devices to verify ADPL is available.", true);
                QC::TransportManager::instance().stop_monitor();
                KL::Logger::get_instance().flush_and_shutdown();
                return 1;
            }
            CFLOG_INFO("Resolved ADPL protocol: " + adpl_name, true);
            opts.adpl_port_name = adpl_name;
        }
    }

    auto config = to_logger_config(opts);

    std::signal(SIGINT, signal_handler);
#ifndef TOOLS_TARGET_WINDOWS
    std::signal(SIGTERM, signal_handler);
#endif

    QC::DiagLogger logger(config);
    g_logger = &logger;

    if (!logger.open_transport()) {
        g_logger = nullptr;
        QC::TransportManager::instance().stop_monitor();
        KL::Logger::get_instance().flush_and_shutdown();
        return 1;
    }

    if (!logger.start()) {
        logger.close_transport();
        g_logger = nullptr;
        QC::TransportManager::instance().stop_monitor();
        KL::Logger::get_instance().flush_and_shutdown();
        return 1;
    }

    // Duration limit is enforced here, not in the RX loop: the RX loop must keep
    // running so stop()'s graceful teardown (QDSS/ADPL disable, mask clearing) can
    // still exchange commands and receive responses. stop() then clears running_.
    const auto start_time = std::chrono::steady_clock::now();
    const auto duration = std::chrono::seconds(config.duration_seconds);

    while (!g_should_stop.load() && logger.is_running()) {
        if (config.duration_seconds > 0 &&
            std::chrono::steady_clock::now() - start_time >= duration) {
            CFLOG_INFO("\nDuration limit reached (" +
                       std::to_string(config.duration_seconds) + "s)", true);
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }

    // Always stop() — covers Ctrl+C, the --duration limit, and a self-terminated
    // session. stop() is idempotent and runs the graceful teardown once.
    logger.stop();

    logger.close_transport();
    g_logger = nullptr;

    QC::TransportManager::instance().stop_monitor();
    KL::Logger::get_instance().flush_and_shutdown();
    return 0;
}
