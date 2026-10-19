// Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
// SPDX-License-Identifier: BSD 3-Clause Clear License
//
// CFG file parser for Qualcomm diagnostic logging configuration.
// Supports .cfg (HDLC-framed) and .cfg2 (Non-HDLC-framed) command streams.

#ifndef QC_CFG_PARSER_H
#define QC_CFG_PARSER_H

#include <string>
#include <vector>

#include "types.h"

namespace QC {

/// CFG file format.
enum class CfgVersion {
    Unknown,
    Hdlc,      // HDLC-framed command stream (.cfg)
    NonHdlc    // Non-HDLC-framed command stream (.cfg2)
};

/// Parse a CFG file from disk.
/// Returns decoded command payloads (no framing) ready to send via send_command().
/// Throws std::runtime_error on parse failure.
std::vector<ByteBuffer> parse_cfg_file(const std::string& path);

/// Detect the CFG file format from content and/or file path extension.
CfgVersion detect_cfg_version(const ByteBuffer& data, const std::string& path = "");

} // namespace QC

#endif // QC_CFG_PARSER_H
