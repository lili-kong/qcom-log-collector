// Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
// SPDX-License-Identifier: BSD 3-Clause Clear License

#include <KL/kLogger.h>
#include <sstream>

#ifdef TOOLS_TARGET_WINDOWS
#include <windows.h>
#else
#include <time.h>
#include <unistd.h>
#endif

#include "ptrace_writer.h"

namespace QC {

PtraceWriter::~PtraceWriter() {
    close();
}

bool PtraceWriter::open(const std::string& path, const PtraceMetadata& metadata) {
    std::lock_guard<std::mutex> lock(mutex_);

    // Guard: reject a second open() without an intervening close().
    if (is_open_) {
        return false;
    }

    path_ = path;
    device_description_ = metadata.device_description;
    record_count_ = 0;
    bytes_written_ = 0;

    // Build metadata values string
    std::ostringstream meta_ss;
    meta_ss << get_pid() << ","
            << metadata.app_name << ","
            << metadata.app_version << ","
            << get_hostname() << ","
            << get_os_family();

    // Initialize ptrace headers in kLogger - these will be written at the start
    // of each new ptrace file (including rotated files)
    KL::Logger::get_instance().initialize_ptrace_headers({
        "pid,appName,appVersion,hostname,osFamily",  // Line 1: metadata header
        meta_ss.str(),                                // Line 2: metadata values
        "",                                           // Line 3: blank separator
        "datetime,message,datatype,datalen,data"     // Line 4: column header
    });

    is_open_ = true;
    return true;
}

bool PtraceWriter::write_record(Direction dir, const uint8_t* data, size_t len) {
    std::lock_guard<std::mutex> lock(mutex_);

    if (!is_open_) { return false; }

    std::string ts = now_as_timestamp();
    std::string hex = hex_encode(data, len);

    std::string dir_str;
    std::string message;
    if (dir == Direction::TX) {
        dir_str = "Tx";
        message = "Send to " + device_description_ + " size: " + std::to_string(len);
    } else {
        dir_str = "Rx";
        message = "# Receive from " + device_description_ + " Size: " + std::to_string(len);
    }

    // CSV line: "datetime","message","datatype","datalen","data"
    std::string line = "\"" + ts + "\",\"" + message + "\",\"" +
                       dir_str + "\",\"" + std::to_string(len) + "\",\"" +
                       hex + "\"";

    PTRACE_LOG(line);

    bytes_written_ += line.size();
    ++record_count_;

    return true;
}

bool PtraceWriter::write_record(Direction dir, const ByteBuffer& data) {
    return write_record(dir, data.data(), data.size());
}

void PtraceWriter::close() {
    std::lock_guard<std::mutex> lock(mutex_);
    is_open_ = false;
    // kLogger handles file flushing/closing via its shutdown mechanism
}

bool PtraceWriter::is_open() {
    std::lock_guard<std::mutex> lock(mutex_);
    return is_open_;
}

std::string PtraceWriter::now_as_timestamp() {
    // Format: "YYYY/MM/DD HH:MM:SS.mmm" (23 chars)
#ifdef TOOLS_TARGET_WINDOWS
    SYSTEMTIME st;
    GetLocalTime(&st);
    char buf[32];
    snprintf(buf, sizeof(buf), "%04u/%02u/%02u %02u:%02u:%02u.%03u",
             st.wYear, st.wMonth, st.wDay,
             st.wHour, st.wMinute, st.wSecond, st.wMilliseconds);
    return buf;
#else
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    struct tm tm_local;
    localtime_r(&ts.tv_sec, &tm_local);
    int ms = static_cast<int>(ts.tv_nsec / 1000000);
    char buf[32];
    snprintf(buf, sizeof(buf), "%04d/%02d/%02d %02d:%02d:%02d.%03d",
             tm_local.tm_year + 1900, tm_local.tm_mon + 1, tm_local.tm_mday,
             tm_local.tm_hour, tm_local.tm_min, tm_local.tm_sec, ms);
    return buf;
#endif
}

std::string PtraceWriter::hex_encode(const uint8_t* data, size_t len) {
    // "7B 01 02 03 " — space-separated hex bytes with trailing space
    std::string result;
    result.reserve(len * 3);
    static const char hex_chars[] = "0123456789ABCDEF";
    for (size_t i = 0; i < len; ++i) {
        result += hex_chars[(data[i] >> 4) & 0x0F];
        result += hex_chars[data[i] & 0x0F];
        result += ' ';
    }
    return result;
}

uint32_t PtraceWriter::get_pid() {
#ifdef TOOLS_TARGET_WINDOWS
    return static_cast<uint32_t>(GetCurrentProcessId());
#else
    return static_cast<uint32_t>(getpid());
#endif
}

std::string PtraceWriter::get_hostname() {
#ifdef TOOLS_TARGET_WINDOWS
    char buf[256] = {};
    DWORD size = sizeof(buf);
    if (GetComputerNameA(buf, &size)) {
        return std::string(buf, size);
    }
    return "UNKNOWN";
#else
    char buf[256] = {};
    if (gethostname(buf, sizeof(buf)) == 0) {
        return buf;
    }
    return "UNKNOWN";
#endif
}

std::string PtraceWriter::get_os_family() {
#ifdef TOOLS_TARGET_WINDOWS
    return "Windows";
#elif defined(TOOLS_TARGET_LINUX)
    return "Linux";
#elif defined(TOOLS_TARGET_OSX)
    return "macOS";
#else
    return "Unknown";
#endif
}

} // namespace QC
