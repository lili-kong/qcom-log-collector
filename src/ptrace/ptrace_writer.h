// Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
// SPDX-License-Identifier: BSD 3-Clause Clear License
//
// Ptrace file writer — CSV format.
//
//   Line 1: metadata header — pid,appName,appVersion,hostname,osFamily
//   Line 2: metadata values — <pid>,<appName>,<version>,<hostname>,<os>
//   Line 3: blank
//   Line 4: column header   — datetime,message,datatype,datalen,data
//   Data:   per-record CSV  — "datetime","message","datatype","datalen","data"
//
// TX message: "Send to <device_description> size: N"
// RX message: "# Receive from <device_description> Size: N"
//
// The "data" field contains raw wire-format bytes (HDLC or NonHDLC encoded),
// hex-encoded with spaces between bytes and a trailing space.
// The PortTraceReader handles HDLC/NonHDLC decoding internally.

#ifndef DIAGLOGGER_PTRACE_WRITER_H
#define DIAGLOGGER_PTRACE_WRITER_H

#include "diaglogger/types.h"
#include <cstdint>
#include <mutex>
#include <string>

namespace diaglogger {

/// Metadata for the ptrace file header.
struct PtraceMetadata {
    std::string app_name = "qualcomm-log-collector";
    std::string app_version = "1.0";
    std::string device_description;  // e.g. "USB handle 2020 : Qualcomm HS-USB Diagnostics 90DB (COM3)"
};

/// Writes raw diag data to a ptrace file in CSV format.
class PtraceWriter {
public:
    PtraceWriter() = default;
    ~PtraceWriter();

    // Non-copyable
    PtraceWriter(const PtraceWriter&) = delete;
    PtraceWriter& operator=(const PtraceWriter&) = delete;

    /// Open ptrace file for writing (writes metadata header + column header).
    bool open(const std::string& path, const PtraceMetadata& metadata);

    /// Write a single record to the ptrace file (thread-safe).
    /// Data should be raw wire-format bytes (HDLC or NonHDLC encoded).
    bool write_record(Direction dir, const uint8_t* data, size_t len);
    bool write_record(Direction dir, const ByteBuffer& data);

    /// Close the ptrace file.
    void close();

    /// Whether the file is open.
    bool is_open();

    /// Number of records written.
    uint64_t record_count() const { return record_count_; }

    /// Total bytes written (including CSV overhead).
    uint64_t bytes_written() const { return bytes_written_; }

    /// Path to the open ptrace file.
    const std::string& path() const { return path_; }

private:
    std::string path_;
    std::string device_description_;
    std::mutex mutex_;
    uint64_t record_count_ = 0;
    uint64_t bytes_written_ = 0;
    bool is_open_{false};

    /// Format current local time as "YYYY/MM/DD HH:MM:SS.mmm".
    static std::string now_as_timestamp();

    /// Hex-encode bytes with spaces and trailing space: "7B 01 02 03 ".
    static std::string hex_encode(const uint8_t* data, size_t len);

    /// Get current process ID.
    static uint32_t get_pid();

    /// Get current hostname.
    static std::string get_hostname();

    /// Get OS family string.
    static std::string get_os_family();
};

} // namespace diaglogger

#endif // DIAGLOGGER_PTRACE_WRITER_H
