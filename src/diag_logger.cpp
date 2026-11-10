// Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
// SPDX-License-Identifier: BSD 3-Clause Clear License
//
// DiagLogger: public API implementation. A pimpl forwarding layer over the
// internal DiagLoggerService engine, plus the library's own kLogger setup.

#include <KL/kLogger.h>

#include "exports/diag_logger.h"
#include "function/diag_logger_service.h"

namespace QC {

namespace {

// Configure this module's kLogger instance for internal diagnostics. The DIAG
// port trace is written by PtraceWriter directly to the caller's output path.
//
// kLogger is header-only and its singleton lives in a function-local static, so
// each module owns one: the consumer (e.g. qlc.exe) configures its own for its
// user-facing messages, and this one backs everything the engine writes —
// The first init wins, so keep this process-global logger independent of the
// per-capture output path.
//
// init() is std::call_once-guarded internally, so constructing several
// DiagLoggers is harmless: the first one's paths win. Shutdown is left to
// ~Logger() at module unload — kLogger exposes no flush that keeps the worker
// thread alive, so flushing here would break any later DiagLogger.
void configure_library_logging(const DiagLoggerConfig& config) {
    auto& logger = KL::Logger::get_instance();

#ifdef QLC_DLL_EXPORTS
    // MCP uses stdout for its JSON-RPC stdio transport. Keep diagnostics
    // visible to the host on stderr, but never let the DLL write protocol noise
    // to stdout. The static CLI retains its normal console behavior.
    auto options = KL::LogOption::ConsoleToStderr;
    if (config.verbose) {
        logger.setLevel(KL::Level::Debug);
        options |= KL::LogOption::DebugToFile;
    }
    logger.setOptions(options);
#else
    if (config.verbose) {
        logger.setLevel(KL::Level::Debug);
        logger.setOptions(KL::LogOption::DebugToFile);
    }
#endif

    logger.init(LOG_DIR, 0, KL::MAX_LOG_FILE_SIZE);
}

} // namespace

DiagLogger::DiagLogger(const DiagLoggerConfig& config) {
    // Before the engine exists: DiagLoggerService's constructor creates the
    // transport, which already logs.
    configure_library_logging(config);
    impl_ = std::make_unique<DiagLoggerService>(config);
}

// Out-of-line so DiagLoggerService is complete where unique_ptr's deleter is
// instantiated; the header only forward-declares it.
DiagLogger::~DiagLogger() = default;

// The port lifecycle is driven straight through to the protocol the service
// owns, so a caller can react to device arrival/departure callbacks without
// this wrapper holding any state of its own.
//
// Each call tolerates an absent protocol: get_protocol() hands back a null
// handle for a type this service does not drive, and for QDSS/ADPL before
// start() creates them. Requests against one are dropped rather than
// crashing, which keeps a monitor callback naming an unexpected type safe.
void DiagLogger::request_open(ProtocolType type) {
    const auto& protocol = impl_->get_protocol(type);
    if (protocol) {
        protocol->request_open();
    }
}

void DiagLogger::request_close(ProtocolType type) {
    const auto& protocol = impl_->get_protocol(type);
    if (protocol) {
        protocol->request_close();
    }
}

bool DiagLogger::wait_ready(std::chrono::milliseconds timeout, ProtocolType type) {
    const auto& protocol = impl_->get_protocol(type);
    // An absent protocol is never going to become ready.
    return protocol ? protocol->wait_ready(timeout) : false;
}

bool DiagLogger::wait_closed(std::chrono::milliseconds timeout, ProtocolType type) {
    const auto& protocol = impl_->get_protocol(type);
    // An absent protocol is already as closed as it will ever be.
    return protocol ? protocol->wait_closed(timeout) : true;
}

ProtocolState DiagLogger::protocol_state(ProtocolType type) const {
    return impl_->get_protocol_state(type);
}

void DiagLogger::validate_diag_ready(std::chrono::milliseconds timeout) const {
    impl_->validate_diag_state(timeout);
}

bool DiagLogger::start() {
    return impl_->start();
}

void DiagLogger::stop() {
    impl_->stop();
}

bool DiagLogger::is_running() const {
    return impl_->is_running();
}

DiagStats DiagLogger::stats() const {
    return impl_->stats();
}

} // namespace QC
