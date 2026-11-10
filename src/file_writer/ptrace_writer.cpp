// Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
// SPDX-License-Identifier: BSD 3-Clause Clear License

#include <KL/Logger.h>

#include <chrono>
#include <cstdio>
#include <filesystem>
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

    const std::string requested_directory =
        path.empty() ? std::string(PTRACE_DIR) : path;
    if (output_directory_ != requested_directory) {
        output_directory_ = requested_directory;
        path_.clear();
        file_sequence_ = 0;
        file_bytes_written_ = 0;
    }

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

    header_lines_ = {
        "pid,appName,appVersion,hostname,osFamily",  // Line 1: metadata header
        meta_ss.str(),                                // Line 2: metadata values
        "",                                           // Line 3: blank separator
        "datetime,message,datatype,datalen,data"     // Line 4: column header
    };

    std::error_code ec;
    std::filesystem::create_directories(output_directory_, ec);
    if (ec) {
        return false;
    }

    if (path_.empty()) {
        if (!open_new_file()) {
            return false;
        }
    } else {
        const bool file_exists = std::filesystem::exists(path_, ec) && !ec;
        stream_.open(path_, std::ios::out | (file_exists ? std::ios::app : std::ios::trunc));
        if (!stream_.is_open()) {
            return false;
        }
        if (file_exists) {
            file_bytes_written_ = std::filesystem::file_size(path_, ec);
            if (ec) {
                file_bytes_written_ = 0;
            }
        } else {
            file_bytes_written_ = 0;
            for (const auto& header : header_lines_) {
                stream_ << header << '\n';
                file_bytes_written_ += header.size() + 1;
            }
        }
    }

    is_open_ = true;
    stopping_ = false;
    try {
        writer_thread_ = std::thread(&PtraceWriter::writer_loop, this);
    } catch (...) {
        is_open_ = false;
        stream_.close();
        return false;
    }
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

    pending_lines_.push(line);

    bytes_written_ += line.size();
    ++record_count_;
    cv_.notify_one();

    return true;
}

bool PtraceWriter::write_record(Direction dir, const ByteBuffer& data) {
    return write_record(dir, data.data(), data.size());
}

void PtraceWriter::close() {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!writer_thread_.joinable()) {
            is_open_ = false;
            if (stream_.is_open()) {
                stream_.flush();
                stream_.close();
            }
            return;
        }
        is_open_ = false;
        stopping_ = true;
    }
    cv_.notify_all();
    writer_thread_.join();

    std::lock_guard<std::mutex> lock(mutex_);
    if (stream_.is_open()) {
        stream_.flush();
        stream_.close();
    }
    stopping_ = false;
}

bool PtraceWriter::is_open() {
    std::lock_guard<std::mutex> lock(mutex_);
    return is_open_;
}

void PtraceWriter::writer_loop() {
    for (;;) {
        std::string line;
        {
            std::unique_lock<std::mutex> lock(mutex_);
            cv_.wait(lock, [this] { return stopping_ || !pending_lines_.empty(); });
            if (pending_lines_.empty() && stopping_) {
                break;
            }
            line = std::move(pending_lines_.front());
            pending_lines_.pop();
        }
        write_line(line);
    }
}

bool PtraceWriter::open_new_file() {
    if (stream_.is_open()) {
        stream_.flush();
        stream_.close();
    }

    ++file_sequence_;
    const auto now = std::chrono::system_clock::now();
    const auto time_t_value = std::chrono::system_clock::to_time_t(now);
    const auto milliseconds =
        std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()) % 1000;
    std::tm local_time{};
#ifdef TOOLS_TARGET_WINDOWS
    localtime_s(&local_time, &time_t_value);
#else
    localtime_r(&time_t_value, &local_time);
#endif

    char filename[160]{};
    if (file_sequence_ == 1) {
        std::snprintf(filename, sizeof(filename),
                      "qlc_%02d_%02d_%04d_%02d_%02d_%02d_%03d__PID_%u_START_1.diag",
                      local_time.tm_mday, local_time.tm_mon + 1, local_time.tm_year + 1900,
                      local_time.tm_hour, local_time.tm_min, local_time.tm_sec,
                      static_cast<int>(milliseconds.count()), get_pid());
    } else {
        std::snprintf(filename, sizeof(filename),
                      "qlc_%02d_%02d_%04d_%02d_%02d_%02d_%03d__PID_%u_%zu.diag",
                      local_time.tm_mday, local_time.tm_mon + 1, local_time.tm_year + 1900,
                      local_time.tm_hour, local_time.tm_min, local_time.tm_sec,
                      static_cast<int>(milliseconds.count()), get_pid(), file_sequence_);
    }

    path_ = (std::filesystem::path(output_directory_) / filename).string();
    stream_.open(path_, std::ios::out | std::ios::trunc);
    if (!stream_.is_open()) {
        return false;
    }

    file_bytes_written_ = 0;
    for (const auto& header : header_lines_) {
        stream_ << header << '\n';
        file_bytes_written_ += header.size() + 1;
    }
    return stream_.good();
}

void PtraceWriter::write_line(const std::string& line) {
    if (!stream_.is_open()) {
        return;
    }
    if (file_bytes_written_ > 0 &&
        file_bytes_written_ + line.size() + 1 > KL::MAX_LOG_FILE_SIZE) {
        if (!open_new_file()) {
            return;
        }
    }
    stream_ << line << '\n';
    if (stream_.good()) {
        file_bytes_written_ += line.size() + 1;
    }
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
