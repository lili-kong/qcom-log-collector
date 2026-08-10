// Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
// SPDX-License-Identifier: BSD 3-Clause Clear License
//
// CFG file parser for Qualcomm diagnostic logging configuration.
// Parses .cfg (HDLC-framed) and .cfg2 (Non-HDLC-framed) command streams,
// returning decoded command payloads ready to send to the device.

#include <fstream>
#include <stdexcept>
#include <KL/Macros.h>
#include "cfg_parser.h"
#include "hdlc/hdlc.h"
#include "hdlc/non_hdlc.h"

namespace QC {

// Check if path has a given extension (case-insensitive).
static bool has_extension(const std::string& path, const std::string& ext) {
    if (path.size() < ext.size()) {
        return false;
    }
    std::string suffix = path.substr(path.size() - ext.size());
    for (auto& c : suffix) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return suffix == ext;
}

// Generic stream parser — works with any decoder that has feed() and next_frame().
// Returns decoded commands. Throws if no valid frames are found.
template<typename Decoder>
static std::vector<ByteBuffer> parse_cfg_stream(const ByteBuffer& data, const std::string& path) {
    std::vector<ByteBuffer> commands;
    Decoder decoder;
    decoder.feed(data.data(), data.size());

    while (true) {
        auto frame = decoder.next_frame();
        if (!frame || frame->empty()) break;
        commands.push_back(std::move(*frame));
    }

    if (commands.empty()) {
        throw std::runtime_error("Failed to parse CFG file: " + path +
            "\n  No valid frames found. Check that the file is in the correct format.");
    }

    return commands;
}

CfgVersion detect_cfg_version(const ByteBuffer& /*data*/, const std::string& path) {
    if (!path.empty()) {
        if (has_extension(path, ".cfg2")) {
            return CfgVersion::NonHdlc;
        }
        if (has_extension(path, ".cfg")) {
            return CfgVersion::Hdlc;
        }
    }
    return CfgVersion::Unknown;
}

std::vector<ByteBuffer> parse_cfg_file(const std::string& path) {
    FLOG_DEBUG("Opening CFG file: " + path);
    std::ifstream file(path, std::ios::binary | std::ios::ate);
    if (!file.is_open()) {
        throw std::runtime_error("Cannot open CFG file: " + path);
    }

    auto size = file.tellg();
    if (size <= 0) {
        throw std::runtime_error("CFG file is empty: " + path);
    }

    FLOG_DEBUG("CFG file size: " + std::to_string(size) + " bytes");
    ByteBuffer data(static_cast<size_t>(size));
    file.seekg(0, std::ios::beg);
    file.read(reinterpret_cast<char*>(data.data()), size);

    CfgVersion version = detect_cfg_version(data, path);
    FLOG_DEBUG("Detected CFG version: " + std::string(version == CfgVersion::Hdlc ? "HDLC" :
               (version == CfgVersion::NonHdlc ? "Non-HDLC" : "Unknown")));
    switch (version) {
    case CfgVersion::Hdlc:
        return parse_cfg_stream<HdlcDecoder>(data, path);
    case CfgVersion::NonHdlc:
        return parse_cfg_stream<NonHdlcDecoder>(data, path);
    default:
        throw std::runtime_error("Unknown CFG file format: " + path +
            "\n  Expected .cfg (HDLC) or .cfg2 (Non-HDLC) file extension.");
    }
}

} // namespace QC
