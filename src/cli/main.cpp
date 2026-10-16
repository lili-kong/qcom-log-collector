// Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
// SPDX-License-Identifier: BSD 3-Clause Clear License
//
// qlc (qualcomm-log-collector) CLI entry point.
//
// Usage:
//   qlc --collect-log --cfg=<file.cfg> --device=<device_name>
//       --duration=60 [--out=<dir>] [--baud=38400] [--verbose]
//   qlc --devices

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <csignal>
#include <cstdlib>
#include <ctime>
#include <exception>
#include <iomanip>
#include <iostream>
#include <KL/kLogger.h>
#include <KL/Macros.h>
#include <memory>
#include <mutex>
#include <string>
#include <sstream>
#include <thread>
#include <vector>

#include "cli/cli_parser.h"
#include "cli/cli_help.h"
#include "exports/device_discovery.h"
#include "exports/diag_logger.h"
#include "version.h"

static std::atomic<bool> g_should_stop{false};
static std::shared_ptr<QC::DiagLogger> g_logger;
static std::mutex g_logger_mutex;
static std::mutex g_start_state_mutex;
static std::condition_variable g_start_state_cv;
static std::atomic<bool> g_start_in_progress{false};
static std::atomic<bool> g_start_succeeded{false};
static std::atomic<bool> g_diag_start_armed{true};
static std::atomic<bool> g_start_dispatch_enabled{false};
static QC::DeviceDiscovery* g_discovery = nullptr;

static bool wait_protocols_ready(std::chrono::milliseconds wait_time);

static void start_diaglogger() {
    if (!wait_protocols_ready(std::chrono::seconds(30))) {
        if (!g_should_stop.load()) {
            CFLOG_ERROR("Timed out waiting for required DIAG, QDSS, and ADPL protocols.", true);
        }
        std::lock_guard<std::mutex> lock(g_start_state_mutex);
        g_start_succeeded.store(false);
        g_start_in_progress.store(false);
        g_start_state_cv.notify_all();
        return;
    }

    std::shared_ptr<QC::DiagLogger> logger;
    {
        std::lock_guard<std::mutex> lock(g_logger_mutex);
        logger = g_logger;
    }

    bool started = false;
    if (logger) {
        try {
            // request_open() is non-blocking; wait until the DIAG handshake
            // has completed before starting the logging service.
            logger->request_open();
            if (!logger->wait_ready(std::chrono::seconds(30))) {
                CFLOG_ERROR("Error: Timed out opening diag port.", true);
                logger->request_close();
                logger->wait_closed(std::chrono::seconds(5));
            } else if (logger->is_running()) {
                // A reconnect keeps the existing service workers alive. Its
                // recovery worker reapplies device setup after the handshake.
                started = true;
            } else {
                started = logger->start();
                if (!started) {
                    logger->request_close();
                    logger->wait_closed(std::chrono::seconds(5));
                }
            }
        } catch (const std::exception& e) {
            CFLOG_ERROR(std::string("Exception starting DiagLogger: ") + e.what(), true);
        } catch (...) {
            CFLOG_ERROR("Unknown exception starting DiagLogger.", true);
        }
    }

    {
        std::lock_guard<std::mutex> lock(g_start_state_mutex);
        g_start_succeeded.store(started);
        g_start_in_progress.store(false);
    }
    g_start_state_cv.notify_all();
}

// Start the DIAG-open sequence without blocking a device-monitor callback.
// Initial startup and recovery both consume the same DIAG-start permit; a
// DIAG departure is the only event that arms another startup attempt.
static bool launch_diaglogger_start() {
    if (!g_start_dispatch_enabled.load()) {
        return false;
    }

    bool armed = true;
    if (!g_diag_start_armed.compare_exchange_strong(armed, false)) {
        return false;
    }

    bool expected = false;
    if (!g_start_in_progress.compare_exchange_strong(expected, true)) {
        g_diag_start_armed.store(true);
        return false;
    }

    g_start_succeeded.store(false);
    try {
        std::thread(start_diaglogger).detach();
    } catch (const std::exception& e) {
        g_start_in_progress.store(false);
        g_diag_start_armed.store(true);
        CFLOG_ERROR(std::string("Failed to create DiagLogger startup thread: ") + e.what(), true);
        g_start_state_cv.notify_all();
        return false;
    }
    return true;
}

static void wait_for_diaglogger_start() {
    std::unique_lock<std::mutex> lock(g_start_state_mutex);
    g_start_state_cv.wait(lock, [] { return !g_start_in_progress.load(); });
}

// DeviceDiscovery listeners are plain function pointers (see
// exports/Definitions.h), so the state they need lives at file scope. These are
// written before start_monitor() installs the listeners and only read from the
// monitor thread afterwards.
//
// g_logging_active gates recovery: the listeners drive the ports only while a
// logging session is in progress. Before start() the main thread owns the open
// sequence, and after stop() the ports must stay closed.
static std::string g_diag_port_name;
static std::string g_qdss_port_name;
static std::string g_adpl_port_name;
static std::atomic<bool> g_logging_active{false};
static std::atomic<bool> g_qdss_required{false};
static std::atomic<bool> g_adpl_required{false};
static std::atomic<bool> g_diag_port_ready{false};
static std::atomic<bool> g_qdss_port_ready{false};
static std::atomic<bool> g_adpl_port_ready{false};

static void signal_handler(int /*sig*/) {
    // Only set the flag here; the main loop performs the graceful stop().
    // (stop() sends diag commands and joins threads — not safe from signal context.)
    g_should_stop.store(true);
}

// Whether a protocol description refers to one of this session's ports.
// QcDevice reports a full DevDesc (e.g. "Qualcomm ... (COM3)") while the
// resolved port name may be a substring, so match in either direction.
static bool port_matches(const std::string& configured, const std::string& description) {
    if (configured.empty() || description.empty()) {
        return false;
    }
    return description.find(configured) != std::string::npos
           || configured.find(description) != std::string::npos;
}

// Map a protocol description back to the protocol type this session assigned
// it. Returns PROT_UNKNOWN when the description is not one of ours — the
// monitor fires for every protocol on every device, so most events land here.
static QC::ProtocolType session_protocol_type(const std::string& description) {
    if (port_matches(g_diag_port_name, description)) return QC::PROT_DIAG;
    if (port_matches(g_qdss_port_name, description)) return QC::PROT_QDSS;
    if (port_matches(g_adpl_port_name, description)) return QC::PROT_ADPL;
    return QC::PROT_UNKNOWN;
}

static void set_protocol_ready(QC::ProtocolType type, bool ready) {
    switch (type) {
        case QC::PROT_DIAG: g_diag_port_ready.store(ready); break;
        case QC::PROT_QDSS: g_qdss_port_ready.store(ready); break;
        case QC::PROT_ADPL: g_adpl_port_ready.store(ready); break;
        default: break;
    }
}

static bool protocols_ready() {
    return g_diag_port_ready.load()
           && (!g_qdss_required.load() || g_qdss_port_ready.load())
           && (!g_adpl_required.load() || g_adpl_port_ready.load());
}

static void initialize_protocol_readiness(const std::vector<QC::DeviceInfo>& devices) {
    g_diag_port_ready.store(false);
    g_qdss_port_ready.store(false);
    g_adpl_port_ready.store(false);
    for (const auto& device : devices) {
        for (const auto& protocol : device.protocols) {
            const QC::ProtocolType type = session_protocol_type(protocol.description);
            if (type != QC::PROT_UNKNOWN &&
                protocol.protocol_state == QC::STATE_AVAILABLE) {
                set_protocol_ready(type, true);
            }
        }
    }
}

static bool wait_protocols_ready(std::chrono::milliseconds wait_time) {
    const auto deadline = std::chrono::steady_clock::now() + wait_time;
    while (!g_should_stop.load()) {
        if (protocols_ready()) {
            return true;
        }
        if (std::chrono::steady_clock::now() >= deadline) {
            break;
        }
        if (g_discovery) {
            initialize_protocol_readiness(g_discovery->enumerate_composite_devices());
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    return false;
}

static const char* protocol_type_name(QC::ProtocolType type) {
    switch (type) {
        case QC::PROT_DIAG: return "DIAG";
        case QC::PROT_QDSS: return "QDSS";
        case QC::PROT_ADPL: return "ADPL";
        default:            return "unknown";
    }
}

// --- Device arrival / departure logging -------------------------------------
//
// Whole-device edges: fired when a device's first protocol appears or its last
// one departs. Log-only — the port lifecycle is driven by the per-protocol
// callbacks below, which can tell whether the protocol that moved is one this
// session is logging.

static void QC_CALLBACK on_device_connected(const QC::DeviceInfo& device) {
    FLOG_INFO("Device connected: " + device.description
              + " (" + std::to_string(device.protocols.size()) + " protocol(s))");
}

static void QC_CALLBACK on_device_disconnected(const QC::DeviceInfo& device) {
    FLOG_INFO("Device disconnected: " + device.description);
}

// --- Recovery listeners -----------------------------------------------------
//
// The CLI owns the port lifecycle. On DIAG return, the existing DiagLogger
// instance reopens the port; its recovery worker restores the setup that the
// reboot wiped (CFG masks, QDSS/ADPL enables, and related state).
//
// Only DIAG is reopened by the CLI through the startup thread. QDSS and ADPL
// are managed by DiagLogger's startup and recovery paths.
//
// Both run on the device-monitor thread. request_open()/request_close() are
// non-blocking — they only set the protocol's desired state, and its own
// lifecycle worker does the work — so neither blocks the monitor.

static void QC_CALLBACK on_protocol_removed(const QC::DeviceInfo& device,
                                            const QC::ProtocolInfo& protocol) {
    FLOG_INFO("Protocol departed: " + protocol.description
              + " (device: " + device.description + ")");
    std::shared_ptr<QC::DiagLogger> logger;
    {
        std::lock_guard<std::mutex> lock(g_logger_mutex);
        if (!g_logger || !g_logging_active.load()) {
            return;
        }
        logger = g_logger;
    }
    const QC::ProtocolType type = session_protocol_type(protocol.description);
    if (type == QC::PROT_UNKNOWN) {
        return;
    }
    set_protocol_ready(type, false);
    CFLOG_WARNING(std::string("\nPort departed (") + protocol_type_name(type)
                  + "); closing it.", true);
    if (type == QC::PROT_DIAG) {
        // Consume this only when the matching DIAG arrival appears. Duplicate
        // arrival events must not run the startup sequence again.
        g_diag_start_armed.store(true);
    }
    logger->request_close(type);
}

static void QC_CALLBACK on_protocol_added(const QC::DeviceInfo& device,
                                          const QC::ProtocolInfo& protocol) {
    FLOG_INFO("Protocol arrived: " + protocol.description
              + " (device: " + device.description + ")");
    if (!g_logging_active.load() && !g_start_dispatch_enabled.load()) {
        return;
    }
    const QC::ProtocolType type = session_protocol_type(protocol.description);
    if (type == QC::PROT_UNKNOWN) {
        return;
    }
    set_protocol_ready(type, protocol.protocol_state == QC::STATE_AVAILABLE);

    if (launch_diaglogger_start()) {
        CFLOG_INFO("Waiting for required protocols before starting DiagLogger.", true);
    }
}

// Flush this executable's kLogger instance (backing the CFLOG_*/CLOG_* macros).
// kLogger's singleton is per-module: the DiagLogger DLL owns a separate one,
// configured inside DiagLogger's constructor and flushed by its own module
// teardown, so there is nothing to shut down across the library boundary here.
static void shutdown_logging() {
    KL::Logger::get_instance().flush_and_shutdown();
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
    QC::DeviceDiscovery discovery;

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

    // Initialize this executable's kLogger instance, which backs the
    // CFLOG_*/CLOG_* macros used below. kLogger is header-only with a
    // function-local static, so the DiagLogger DLL owns a separate instance for
    // the engine's own output (including the ptrace capture); DiagLogger
    // configures that one itself from DiagLoggerConfig::output_path.
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

    // Log device arrivals and departures for the whole session, so the user can
    // see from the log when a device appeared or vanished mid-run. Registered
    // before start_monitor() so the initial scan's edges are reported too.
    // These are log-only; the port lifecycle is driven by the per-protocol
    // listeners registered once logging starts.
    const QC::DeviceDiscovery::ListenerToken connected_token =
        discovery.set_device_connected_listener(&on_device_connected);
    const QC::DeviceDiscovery::ListenerToken disconnected_token =
        discovery.set_device_disconnected_listener(&on_device_disconnected);

    // Detach the device listeners. Every exit path below runs this before
    // stopping the monitor, so no callback can fire against a torn-down CLI.
    auto unsubscribe_device_listeners = [&] {
        discovery.unsubscribe(connected_token);
        discovery.unsubscribe(disconnected_token);
    };

    discovery.start_monitor();

    if (opts.command == QC::CommandType::LIST_DEVICES) {
        CFLOG_INFO("Scanning for QcDevice devices...", true);
        auto composites = discovery.enumerate_composite_devices();
        if (composites.empty()) {
            CFLOG_WARNING("No QcDevice devices found.", true);
            unsubscribe_device_listeners();
            discovery.stop_monitor();
            shutdown_logging();
            return 0;
        }
        CFLOG_INFO("Found " + std::to_string(composites.size()) + " device(s):\n", true);
        size_t i = 0;
        for (const auto& dev : composites) {
            CFLOG_INFO("  Device [" + std::to_string(i) + "] " + dev.description, true);
            if (!dev.adb_serial_number.empty()) {
                CFLOG_INFO("    ADB Serial Number: " + dev.adb_serial_number, true);
            }
            if (!dev.serial_number.empty()) {
                CFLOG_INFO("    MSM Serial Number: " + dev.serial_number, true);
            }
            CFLOG_INFO("    Available Protocols:", true);
            size_t j = 0;
            for (const auto& proto : dev.protocols) {
                std::string proto_str;
                switch (proto.protocol_type) {
                    case QC::PROT_DIAG: proto_str = " [DIAG]"; break;
                    case QC::PROT_QDSS: proto_str = " [QDSS]"; break;
                    case QC::PROT_ADPL: proto_str = " [ADPL]"; break;
                    default: break;
                }
                CFLOG_INFO("      [" + std::to_string(i) + "." + std::to_string(j) + "] " +
                           proto.description + proto_str, true);
                ++j;
            }
            ++i;
        }
        CLOG_INFO("\nUse --collect-log --device=\"<device description>\" to connect.");
        unsubscribe_device_listeners();
        discovery.stop_monitor();
        shutdown_logging();
        return 0;
    }

    // --- collect-log command ---
    // Resolve --device: match the given value against composite device descriptions,
    // ADB serial numbers, or MSM serial numbers, then auto-select the DIAG protocol.
    if (opts.has_port && !opts.device_name.empty()) {
        auto composites = discovery.enumerate_composite_devices();
        const QC::DeviceInfo* matched_dev = nullptr;
        for (const auto& dev : composites) {
            if (dev.description == opts.device_name
                || dev.adb_serial_number == opts.device_name
                || dev.serial_number == opts.device_name) {
                matched_dev = &dev;
                break;
            }
        }
        if (!matched_dev) {
            CFLOG_ERROR("Device not found: \"" + opts.device_name + "\". "
                        "Use --devices to list available devices.", true);
            unsubscribe_device_listeners();
            discovery.stop_monitor();
            shutdown_logging();
            return 1;
        }
        std::string diag_name;
        std::string qdss_name;
        std::string adpl_name;
        for (const auto& proto : matched_dev->protocols) {
            if (diag_name.empty() && proto.protocol_type == QC::PROT_DIAG) {
                diag_name = proto.description;
            } else if (qdss_name.empty() && proto.protocol_type == QC::PROT_QDSS) {
                qdss_name = proto.description;
            } else if (adpl_name.empty() && proto.protocol_type == QC::PROT_ADPL) {
                adpl_name = proto.description;
            }
        }
        if (diag_name.empty()) {
            CFLOG_ERROR("No DIAG protocol found on device: " + matched_dev->description, true);
            unsubscribe_device_listeners();
            discovery.stop_monitor();
            shutdown_logging();
            return 1;
        }
        CFLOG_INFO("Resolved device \"" + opts.device_name + "\" to DIAG protocol: " + diag_name, true);
        opts.port_name = diag_name;

        // Use the QDSS port from the same composite device if --enable-qdss set
        if (opts.enable_qdss) {
            if (qdss_name.empty()) {
                CFLOG_ERROR("No QDSS protocol found on device: " + matched_dev->description +
                            "\nUse --devices to verify QDSS is available.", true);
                unsubscribe_device_listeners();
                discovery.stop_monitor();
                shutdown_logging();
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
                unsubscribe_device_listeners();
                discovery.stop_monitor();
                shutdown_logging();
                return 1;
            }
            CFLOG_INFO("Resolved ADPL protocol: " + adpl_name, true);
            opts.adpl_port_name = adpl_name;
        }
    }

    auto config = to_logger_config(opts);

    g_diag_port_name = config.port_name;
    g_qdss_port_name = config.enable_qdss ? config.qdss_port_name : std::string();
    g_adpl_port_name = config.enable_adpl ? config.adpl_port_name : std::string();
    g_qdss_required.store(config.enable_qdss);
    g_adpl_required.store(config.enable_adpl);
    initialize_protocol_readiness(discovery.enumerate_composite_devices());
    g_discovery = &discovery;

    std::signal(SIGINT, signal_handler);
#ifndef TOOLS_TARGET_WINDOWS
    std::signal(SIGTERM, signal_handler);
#endif
    // Register recovery callbacks in main. The startup thread runs once; a
    // reconnect reuses the same logger instance and its recovery worker.
    QC::DeviceDiscovery::ListenerToken added_token = 0;
    QC::DeviceDiscovery::ListenerToken removed_token = 0;
    if (config.enable_recovery) {
        removed_token = discovery.set_protocol_removed_listener(on_protocol_removed);
        added_token = discovery.set_protocol_added_listener(on_protocol_added);
    }

    const auto session_start_time = std::chrono::steady_clock::now();
    const auto duration = std::chrono::seconds(config.duration_seconds);
    int logger_exit_code = 0;
    auto logger = std::make_shared<QC::DiagLogger>(config);
    {
        std::lock_guard<std::mutex> lock(g_logger_mutex);
        g_logger = logger;
    }
    g_start_dispatch_enabled.store(true);

    // The same arm is used for the first startup and every later recovery.
    // on_protocol_removed(PROT_DIAG) is the only path that re-arms it.
    launch_diaglogger_start();
    wait_for_diaglogger_start();
    const bool started = g_start_succeeded.load();

    if (!started) {
        g_start_dispatch_enabled.store(false);
        {
            std::lock_guard<std::mutex> lock(g_logger_mutex);
            g_logger = nullptr;
        }
        wait_for_diaglogger_start();
        logger_exit_code = 1;
    } else {
        g_logging_active.store(true);

        // Keep the session alive in main so the startup thread is limited to
        // opening and starting DiagLogger. stop() remains able to exchange
        // graceful teardown commands through the running RX loop.
        while (!g_should_stop.load() && logger->is_running()) {
            if (config.duration_seconds > 0 &&
                std::chrono::steady_clock::now() - session_start_time >= duration) {
                CFLOG_INFO("\nDuration limit reached (" +
                           std::to_string(config.duration_seconds) + "s)", true);
                break;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }

        g_logging_active.store(false);
        g_start_dispatch_enabled.store(false);
        {
            std::lock_guard<std::mutex> lock(g_logger_mutex);
            g_logger = nullptr;
        }
        wait_for_diaglogger_start();
        logger->stop();
        logger->request_close();
        logger->wait_closed(std::chrono::seconds(5));
    }

    g_logging_active.store(false);
    g_start_dispatch_enabled.store(false);
    g_discovery = nullptr;
    if (config.enable_recovery) {
        discovery.unsubscribe(added_token);
        discovery.unsubscribe(removed_token);
    }
    unsubscribe_device_listeners();
    discovery.stop_monitor();
    shutdown_logging();
    return logger_exit_code;
}
