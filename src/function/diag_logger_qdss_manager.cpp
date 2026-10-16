// Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
// SPDX-License-Identifier: BSD 3-Clause Clear License
//
// QDSS configuration over the diag port. These are DiagLoggerService members
// (declared in function/diag_logger_service.h) kept in their own translation
// unit so the QDSS control-plane logic stays separate from the orchestrator.

#include "function/diag_logger_service.h"
#include "diag/response_pairing.h"
#include "diag/subsystem_commands.h"

#include <KL/Macros.h>
#include <cstdio>

namespace QC {

namespace {
// Format a 32-bit value as hex (no 0x prefix) for concise diag-id mask logging.
std::string hex(uint32_t v) {
    char b[16];
    std::snprintf(b, sizeof(b), "%x", v);
    return std::string(b);
}

// Hex-dump a response buffer (first 64 bytes) for error logging.
std::string hex_dump(const ByteBuffer& buf) {
    std::string hex;
    for (size_t i = 0; i < buf.size() && i < 64; ++i) {
        char byte[4];
        snprintf(byte, sizeof(byte), "%02x ", buf[i]);
        hex += byte;
    }
    return hex;
}

// QDSS trace logging. Deliberately NOT DiagLoggerService::verbose_log — these traces
// were unconditional in QdssDiagManager, and routing them through the gated
// logger would silence them unless --verbose is passed.
void qdss_debug_log(const std::string& msg) {
    FLOG_DEBUG(msg);
}
} // namespace

// ----------------------------------------------------------------------------
// configure_diag_over_qdss
//
// Try HW acceleration first, fall back to legacy Diag-over-STM.
// ----------------------------------------------------------------------------
bool DiagLoggerService::configure_diag_over_qdss() {
    hw_accel_enabled_ = query_hw_acceleration();

    if (!hw_accel_enabled_) {
        CFLOG_INFO("QDSS: HW acceleration not used - falling back to legacy Diag-over-STM path", true);
        if (!query_diag_over_stm()) {
            return false;
        }
    } else {
        CFLOG_INFO("QDSS: configured using HW acceleration", true);
    }

    return true;
}

// ----------------------------------------------------------------------------
// disable_diag_over_qdss
// ----------------------------------------------------------------------------
void DiagLoggerService::disable_diag_over_qdss() {
    if (hw_accel_enabled_) {
        stop_hw_acceleration();
    } else {
        stop_qdss_commands();
    }
}

// ----------------------------------------------------------------------------
// query_hw_acceleration
// ----------------------------------------------------------------------------
bool DiagLoggerService::query_hw_acceleration() {
    qdss_debug_log("queryHwAcceleration: Configuring Diag over QDSS/ATB HW Acceleration STARTING");

    // 1. Query HW acceleration capabilities. Try cmd_version 2 (dynamic AtID) first;
    // many modern devices reject version 1 with BAD_PARM. Fall back to version 1.
    uint8_t query_version = 2;
    ByteBuffer cmd = build_qdss_hw_accel_query(query_version);
    ByteBuffer resp = send_command(cmd, 0);
    if (resp.empty() || is_error_response(resp)) {
        query_version = 1;
        cmd = build_qdss_hw_accel_query(query_version);
        resp = send_command(cmd, 0);
    }
    if (resp.empty() || is_error_response(resp)) {
        qdss_debug_log("Error: HW Acceleration commands not supported by device");
        return false;
    }
    CFLOG_INFO("QDSS: HW acceleration supported (cmd_version=" + std::to_string(query_version) + ")", true);

    auto payload = extract_subsystem_payload(resp);
    hw_accel_ = parse_qdss_hw_accel_response(payload);
    qdss_trace_sink_ = hw_accel_.diag_transport;
    CFLOG_INFO("QDSS: capabilities - ATB(mask=0x" + hex(hw_accel_.atb_diagid_mask) +
               ") STM(mask=0x" + hex(hw_accel_.stm_diagid_mask) +
               ") LPASS_STM(mask=0x" + hex(hw_accel_.lpass_stm_diagid_mask) + ")", true);

    // Validate response version matches what we queried with.
    if (query_version != hw_accel_.response_version) {
        CFLOG_ERROR("Error: Mismatch in CMD_VER from device: expected " +
                    std::to_string(query_version) + ", got " +
                    std::to_string(hw_accel_.response_version), true);
        return false;
    }

    // 2. Set trace sink to USB
    cmd = build_qdss_set_trace_sink(QdssSinkType::USB);
    resp = send_command(cmd, 0);
    if (resp.empty() || is_error_response(resp)) {
        CFLOG_ERROR("Error: QDSS set trace sink failed.", true);
        return false;
    }
    CFLOG_INFO("QDSS: trace sink set to USB", true);

    // 3-5. Enable HW acceleration (ATB → LPASS STM → STM)
    qdss_debug_log("Supported atbDiagIdMask = " + std::to_string(hw_accel_.atb_diagid_mask) +
                   ", Supported stmDiagIdMask = " + std::to_string(hw_accel_.stm_diagid_mask) +
                   ", Supported lpassStmDiagIdMask = " + std::to_string(hw_accel_.lpass_stm_diagid_mask));

    // 3. Enable ATB (highest priority)
    if (hw_accel_.atb_supported && hw_accel_.atb_diagid_mask != 0) {
        cmd = build_qdss_hw_accel_enable(QdssHwAccelType::ATB, hw_accel_.atb_diagid_mask, query_version);
        resp = send_command(cmd, 0);
        if (resp.empty() || is_error_response(resp)) {
            CFLOG_ERROR("Error: ATB Command not supported by device. However the ATB was enabled.", true);
            return false;
        }
        auto enable_result = parse_qdss_hw_accel_enable_response(extract_subsystem_payload(resp));
        if (!enable_result.valid) {
            CFLOG_ERROR("Error: QDSS ATB enable response invalid, "
                        "Response packet = " + hex_dump(resp), true);
            return false;
        }
        if (query_version != enable_result.cmd_version) {
            CFLOG_ERROR("Error: QDSS ATB enable mismatch in CMD_VER from device: " +
                        std::to_string(enable_result.cmd_version) +
                        ", Response packet = " + hex_dump(resp), true);
            return false;
        }
        if (0 != enable_result.status) {
            CFLOG_ERROR("Error: QDSS ATB enable status returned error: " +
                        std::to_string(enable_result.status) +
                        ", Response packet = " + hex_dump(resp), true);
            return false;
        }
        atb_enabled_mask_ = enable_result.diagid_mask;
        CFLOG_INFO("QDSS: ATB enabled (mask=0x" + hex(enable_result.diagid_mask) + ")", true);
    }

    // 4. Enable LPASS STM
    if (hw_accel_.lpass_stm_supported && hw_accel_.lpass_stm_diagid_mask != 0) {
        // Send QDSS_ADSP_Filter_STM command to configure LPASS STM buffering
        ByteBuffer adsp_filter_payload = { 1 };
        cmd = build_subsystem_command(SubsysId::QDSS, QdssCmd::ADSP_FILTER_STM, adsp_filter_payload);
        resp = send_command(cmd, 0);
        if (resp.empty() || is_error_response(resp)) {
            CFLOG_ERROR("Error: LPASS STM Buffering mode command 90/1283 not supported by device, "
                        "Response packet = " + hex_dump(resp), true);
            return false;
        }
        auto adsp_payload = extract_subsystem_payload(resp);
        if (adsp_payload.empty()) {
            CFLOG_ERROR("Error: QDSS ADSP Filter STM response invalid, "
                        "Response packet = " + hex_dump(resp), true);
            return false;
        }
        if (0 != adsp_payload[0]) {
            CFLOG_ERROR("Error: Command 90/1283 - Status returned error: " +
                        std::to_string(adsp_payload[0]) +
                        ", Response packet = " + hex_dump(resp), true);
            return false;
        }
        qdss_debug_log("QDSS: ADSP Filter STM configured");

        cmd = build_qdss_hw_accel_enable(QdssHwAccelType::LPASS_STM, hw_accel_.lpass_stm_diagid_mask, query_version);
        resp = send_command(cmd, 0);
        if (resp.empty() || is_error_response(resp)) {
            CFLOG_ERROR("Error: LPASS STM Commands 548 not supported by device. However the LPASS STM was enabled.", true);
            return false;
        }
        auto enable_result = parse_qdss_hw_accel_enable_response(extract_subsystem_payload(resp));
        if (!enable_result.valid) {
            CFLOG_ERROR("Error: QDSS LPASS STM enable response invalid, "
                        "Response packet = " + hex_dump(resp), true);
            return false;
        }
        if (query_version != enable_result.cmd_version) {
            CFLOG_ERROR("Error: QDSS LPASS STM enable mismatch in CMD_VER from device: " +
                        std::to_string(enable_result.cmd_version) +
                        ", Response packet = " + hex_dump(resp), true);
            return false;
        }
        if (0 != enable_result.status) {
            CFLOG_ERROR("Error: QDSS LPASS STM enable status returned error: " +
                        std::to_string(enable_result.status) +
                        ", Response packet = " + hex_dump(resp), true);
            return false;
        }
        lpass_stm_enabled_mask_ = enable_result.diagid_mask;
        CFLOG_INFO("QDSS: LPASS_STM enabled (mask=0x" + hex(enable_result.diagid_mask) + ")", true);
    }

    // 5. Enable STM
    if (hw_accel_.stm_supported && hw_accel_.stm_diagid_mask != 0) {
        cmd = build_qdss_hw_accel_enable(QdssHwAccelType::STM, hw_accel_.stm_diagid_mask, query_version);
        resp = send_command(cmd, 0);
        if (resp.empty() || is_error_response(resp)) {
            CFLOG_ERROR("Error: QDSS HW accel STM enable failed.", true);
            return false;
        }
        auto enable_result = parse_qdss_hw_accel_enable_response(extract_subsystem_payload(resp));
        if (!enable_result.valid) {
            CFLOG_ERROR("Error: QDSS STM enable response invalid, "
                        "Response packet = " + hex_dump(resp), true);
            return false;
        }
        if (query_version != enable_result.cmd_version) {
            CFLOG_ERROR("Error: QDSS STM enable mismatch in CMD_VER from device: " +
                        std::to_string(enable_result.cmd_version) +
                        ", Response packet = " + hex_dump(resp), true);
            return false;
        }
        if (0 != enable_result.status) {
            CFLOG_ERROR("Error: QDSS STM enable status returned error: " +
                        std::to_string(enable_result.status) +
                        ", Response packet = " + hex_dump(resp), true);
            return false;
        }
        stm_enabled_mask_ = enable_result.diagid_mask;
        CFLOG_INFO("QDSS: STM enabled (mask=0x" + hex(enable_result.diagid_mask) + ")", true);
    }

    qdss_debug_log("queryHwAcceleration: Configuring Diag over QDSS/ATB HW Acceleration FINISHED");
    return true;
}

// ----------------------------------------------------------------------------
// query_diag_over_stm
// ----------------------------------------------------------------------------
bool DiagLoggerService::query_diag_over_stm() {
    qdss_debug_log("queryDiagOverStm: Configuring Diag over QDSS/STM STARTING");

    // Step 1: version=2, ALL mask, AUTO_QUERY_PROCESSORS(3)
    bool stm_query_ok = false;
    ByteBuffer auto_query_payload = { 2, QdssProcessorMask::ALL, QdssStmCommand::AUTO_QUERY_PROCESSORS };
    ByteBuffer cmd = build_subsystem_command(SubsysId::DIAG_SERV, QdssCmd::DIAG_OVER_STM, auto_query_payload);
    ByteBuffer resp = send_command(cmd, 0);
    if (!resp.empty() && !is_error_response(resp)) {
        stm_query_ok = true;
    } else {
        // Step 2: version=2, ALL mask, QUERY_PROCESSORS(2)
        ByteBuffer query_payload = { 2, QdssProcessorMask::ALL, QdssStmCommand::QUERY_PROCESSORS };
        cmd = build_subsystem_command(SubsysId::DIAG_SERV, QdssCmd::DIAG_OVER_STM, query_payload);
        resp = send_command(cmd, 0);
        if (!resp.empty() && !is_error_response(resp)) {
            stm_query_ok = true;
        } else {
            // Step 3: version=1, OLD_MASK, QUERY_PROCESSORS(2)
            uint8_t old_mask = QdssProcessorMask::MODEM | QdssProcessorMask::ADSP |
                               QdssProcessorMask::WCNSS | QdssProcessorMask::APSS;
            ByteBuffer old_query_payload = { 1, old_mask, QdssStmCommand::QUERY_PROCESSORS };
            cmd = build_subsystem_command(SubsysId::DIAG_SERV, QdssCmd::DIAG_OVER_STM, old_query_payload);
            resp = send_command(cmd, 0);
            if (!resp.empty() && !is_error_response(resp)) {
                stm_query_ok = true;
            }
        }
    }

    if (!stm_query_ok) {
        CFLOG_ERROR("Error: Diag over QDSS configuration, Commands not supported by device.", true);
        return false;
    }
    // TEMP DIAGNOSTIC: raw QUERY_PROCESSORS response. TUF's updateRecords() parses
    // [Version][StatusMask][StmCommand][SupportedMask] from this and uses the
    // supported mask as the processor mask + the version for all per-proc commands.
    CFLOG_INFO("QDSS: legacy Diag-over-STM processor query OK", true);

    query_diag_transport();

    if (!enable_stm(true)) {
        return false;
    }
    CFLOG_INFO("QDSS: legacy STM filter enabled, starting per-processor commands", true);

    start_qdss_commands();

    qdss_debug_log("queryDiagOverStm: Configuring Diag over QDSS/STM FINISHED");
    return true;
}

// ----------------------------------------------------------------------------
// query_diag_transport
// ----------------------------------------------------------------------------
void DiagLoggerService::query_diag_transport() {
    ByteBuffer cmd = build_qdss_query_transport();
    ByteBuffer resp = send_command(cmd, 0);
    if (!resp.empty() && !is_error_response(resp)) {
        qdss_debug_log("QDSS: Diag transport query successful");
    } else {
        qdss_debug_log("QDSS: Diag transport query failed, defaulting to USB");
    }

    // Set trace sink to USB (always USB for QLC)
    cmd = build_qdss_set_trace_sink(QdssSinkType::USB);
    resp = send_command(cmd, 0);
    if (resp.empty() || is_error_response(resp)) {
        qdss_debug_log("QDSS: Set trace sink failed, defaulting to USB");
    } else {
        CFLOG_INFO("QDSS: trace sink set to USB", true);
    }
}

// ----------------------------------------------------------------------------
// enable_stm
// ----------------------------------------------------------------------------
bool DiagLoggerService::enable_stm(bool enable) {
    qdss_debug_log("Calling enableStm(" + std::string(enable ? "true" : "false") + ")");

    ByteBuffer payload = { static_cast<uint8_t>(enable ? 1 : 0) };
    ByteBuffer cmd = build_subsystem_command(SubsysId::QDSS, QdssCmd::FILTER_STM, payload);
    ByteBuffer resp = send_command(cmd, 0);
    if (resp.empty() || is_error_response(resp)) {
        CFLOG_ERROR("Error: QDSS enable STM (command 90/259) failed.", true);
        return false;
    }
    qdss_debug_log("QDSS: STM " + std::string(enable ? "enabled" : "disabled"));
    return true;
}

// ----------------------------------------------------------------------------
// start_qdss_commands
// ----------------------------------------------------------------------------
void DiagLoggerService::start_qdss_commands() {
    const uint8_t processor_mask = config_.qdss_processor_mask;
    qdss_debug_log("startQdssCommands: processorMask = " + std::to_string(processor_mask));

    uint8_t processors[] = {
        QdssProcessorMask::MODEM, QdssProcessorMask::ADSP,
        QdssProcessorMask::WCNSS, QdssProcessorMask::APSS,
        QdssProcessorMask::SLPI,  QdssProcessorMask::CDSP
    };

    // Report any per-processor command the device rejects (a subsystem may be absent).
    auto check = [&](uint8_t proc, const char* step, const ByteBuffer& r) {
        if (r.empty() || is_error_response(r)) {
            qdss_debug_log("QDSS: legacy proc=0x" + hex(proc) + " " + step + " rejected (skipped)");
        }
    };

    // 1. enableDiagOnQdss: entity filter per processor (entity_id=0x0D for Diag)
    for (uint8_t proc : processors) {
        if (processor_mask & proc) {
            ByteBuffer entity_cmd = build_qdss_filter_entity(proc, true, 0x0D);
            if (!entity_cmd.empty()) {
                check(proc, "ENTITY", send_command(entity_cmd, 0));
            }
        }
    }

    // 2. setSwTrace: SW trace filter per processor
    for (uint8_t proc : processors) {
        if (processor_mask & proc) {
            ByteBuffer sw_cmd = build_qdss_filter_sw_trace(proc, true);
            if (!sw_cmd.empty()) {
                check(proc, "SW_TRACE", send_command(sw_cmd, 0));
            }
        }
    }

    // 3. enableDiagOverStm: enable Diag-over-STM per processor
    for (uint8_t proc : processors) {
        if (processor_mask & proc) {
            ByteBuffer stm_payload = { 2, proc, QdssStmCommand::ENABLE_DIAG_OVER_STM };
            ByteBuffer stm_cmd = build_subsystem_command(SubsysId::DIAG_SERV, QdssCmd::DIAG_OVER_STM, stm_payload);
            check(proc, "DIAG_OVER_STM", send_command(stm_cmd, 0));
        }
    }

    // 4. enableDiagStmRequest: send STM request to start data flow
    ByteBuffer stm_request_payload = { 2, 0x01, 1 };
    ByteBuffer stm_request_cmd = build_subsystem_command(SubsysId::DIAG_SERV, QdssCmd::DIAG_OVER_STM, stm_request_payload);
    send_command(stm_request_cmd, 0);
    CFLOG_INFO("QDSS: legacy Diag-over-STM data flow started", true);

    qdss_debug_log("startQdssCommands: FINISHED");
}

// ----------------------------------------------------------------------------
// stop_qdss_commands
// ----------------------------------------------------------------------------
void DiagLoggerService::stop_qdss_commands() {
    const uint8_t processor_mask = config_.qdss_processor_mask;

    uint8_t processors[] = {
        QdssProcessorMask::MODEM, QdssProcessorMask::ADSP,
        QdssProcessorMask::WCNSS, QdssProcessorMask::APSS,
        QdssProcessorMask::SLPI,  QdssProcessorMask::CDSP
    };

    for (uint8_t proc : processors) {
        if (processor_mask & proc) {
            ByteBuffer entity_cmd = build_qdss_filter_entity(proc, false, 0x0D);
            if (!entity_cmd.empty()) {
                send_command(entity_cmd, 0);
            }
        }
    }

    for (uint8_t proc : processors) {
        if (processor_mask & proc) {
            ByteBuffer sw_cmd = build_qdss_filter_sw_trace(proc, false);
            if (!sw_cmd.empty()) {
                send_command(sw_cmd, 0);
            }
        }
    }

    enable_stm(false);
}

// ----------------------------------------------------------------------------
// stop_hw_acceleration
// ----------------------------------------------------------------------------
void DiagLoggerService::stop_hw_acceleration() {
    qdss_debug_log("stopHwAcceleration started: Configuring Diag over QDSS/ATB HW Acceleration DISABLE STARTING"
                   ", m_atbDiagIdEnabled = " + std::to_string(atb_enabled_mask_) +
                   ", m_lpassStmDiagIdEnabled = " + std::to_string(lpass_stm_enabled_mask_) +
                   ", m_stmDiagIdEnabled = " + std::to_string(stm_enabled_mask_));

    // Disable ATB
    if (0 != atb_enabled_mask_) {
        ByteBuffer cmd = build_qdss_hw_accel_disable(QdssHwAccelType::ATB, atb_enabled_mask_);
        ByteBuffer resp = send_command(cmd, 0);
        if (resp.empty() || is_error_response(resp)) {
            CFLOG_ERROR("Error: HW Acceleration ATB disable commands not supported by device.", true);
        } else {
            auto result = parse_qdss_hw_accel_enable_response(extract_subsystem_payload(resp));
            if (!result.valid || 2 != result.cmd_version) {
                CFLOG_ERROR("Error: Diag over QDSS ATB disable mismatch in CMD_VER from device: " +
                            std::to_string(result.cmd_version), true);
            } else if (0 != result.status) {
                CFLOG_ERROR("Error: Diag over QDSS ATB disable status returned error: " +
                            std::to_string(result.status), true);
            } else {
                atb_enabled_mask_ = result.diagid_mask;
                CFLOG_INFO("QDSS: ATB disabled", true);
            }
        }
    } else {
        qdss_debug_log("stopHwAcceleration: skip atb disable commands"
                       ", m_atbDiagIdEnabled = " + std::to_string(atb_enabled_mask_));
    }

    // Disable LPASS STM
    if (0 != lpass_stm_enabled_mask_) {
        ByteBuffer cmd = build_qdss_hw_accel_disable(QdssHwAccelType::LPASS_STM, lpass_stm_enabled_mask_);
        ByteBuffer resp = send_command(cmd, 0);
        if (resp.empty() || is_error_response(resp)) {
            CFLOG_ERROR("Error: HW Acceleration LPASS STM disable commands not supported by device.", true);
        } else {
            auto result = parse_qdss_hw_accel_enable_response(extract_subsystem_payload(resp));
            if (!result.valid || 2 != result.cmd_version) {
                CFLOG_ERROR("Error: Diag over QDSS LPASS STM disable mismatch in CMD_VER from device: " +
                            std::to_string(result.cmd_version), true);
            } else if (0 != result.status) {
                CFLOG_ERROR("Error: Diag over QDSS LPASS STM disable status returned error: " +
                            std::to_string(result.status), true);
            } else {
                lpass_stm_enabled_mask_ = result.diagid_mask;
                CFLOG_INFO("QDSS: LPASS_STM disabled", true);
            }
        }
    } else {
        qdss_debug_log("stopHwAcceleration: skip Lpass STM disable commands"
                       ", m_lpassStmDiagIdEnabled = " + std::to_string(lpass_stm_enabled_mask_));
    }

    // Disable STM
    if (0 != stm_enabled_mask_) {
        ByteBuffer cmd = build_qdss_hw_accel_disable(QdssHwAccelType::STM, stm_enabled_mask_);
        ByteBuffer resp = send_command(cmd, 0);
        if (resp.empty() || is_error_response(resp)) {
            CFLOG_ERROR("Error: HW Acceleration STM disable commands not supported by device.", true);
        } else {
            auto result = parse_qdss_hw_accel_enable_response(extract_subsystem_payload(resp));
            if (!result.valid || 2 != result.cmd_version) {
                CFLOG_ERROR("Error: Diag over QDSS STM disable mismatch in CMD_VER from device: " +
                            std::to_string(result.cmd_version), true);
            } else if (0 != result.status) {
                CFLOG_ERROR("Error: Diag over QDSS STM disable status returned error: " +
                            std::to_string(result.status), true);
            } else {
                stm_enabled_mask_ = result.diagid_mask;
                CFLOG_INFO("QDSS: STM disabled", true);
            }
        }
    } else {
        qdss_debug_log("stopHwAcceleration: skip STM disable commands"
                       ", m_stmDiagIdEnabled = " + std::to_string(stm_enabled_mask_));
    }

    qdss_debug_log("stopHwAcceleration: Configuring Diag over QDSS/ATB HW Acceleration DISABLE FINISHED");

    qdss_debug_log("After stopHwAcceleration - "
                   "Currently atbDiagIdEnabled = " + std::to_string(atb_enabled_mask_) +
                   ", Currently stmDiagIdEnabled = " + std::to_string(stm_enabled_mask_) +
                   ", Currently lpassStmDiagIdEnabled = " + std::to_string(lpass_stm_enabled_mask_));

    // Reset trace sink (matches TUF: setTraceSink at end of stopHwAcceleration).
    // Use the QdssSinkType value (USB=3) — qdss_trace_sink_ holds the query-response
    // transport enum value (QDSS_TRACE_USB=2), which is a different enum and would
    // wrongly select TPIU_SET_A(2) here.
    ByteBuffer sink_cmd = build_qdss_set_trace_sink(QdssSinkType::USB);
    send_command(sink_cmd, 0);
    CFLOG_INFO("QDSS: trace sink reset to USB", true);
}

} // namespace QC
