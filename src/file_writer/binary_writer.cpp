// Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
// SPDX-License-Identifier: BSD 3-Clause Clear License

#include <KL/Macros.h>

#include "binary_writer.h"

namespace QC {

BinaryWriter::~BinaryWriter() {
    close();
}

BinaryWriter::BinaryWriter(BinaryWriter&& other) noexcept
    : out_(std::move(other.out_))
    , path_(std::move(other.path_))
    , bytes_written_(other.bytes_written_) {
    other.bytes_written_ = 0;
}

BinaryWriter& BinaryWriter::operator=(BinaryWriter&& other) noexcept {
    if (this != &other) {
        close();
        out_ = std::move(other.out_);
        path_ = std::move(other.path_);
        bytes_written_ = other.bytes_written_;
        other.bytes_written_ = 0;
    }
    return *this;
}

bool BinaryWriter::open(const std::string& path) {
    if (out_.is_open()) {
        close();
    }

    out_.open(path, std::ios::binary | std::ios::trunc);
    if (!out_.is_open()) {
        CFLOG_ERROR("BinaryWriter: Cannot open file: " + path, true);
        return false;
    }

    path_ = path;
    bytes_written_ = 0;
    return true;
}

bool BinaryWriter::write(const uint8_t* data, size_t len) {
    if (!out_.is_open() || data == nullptr || len == 0) {
        return false;
    }

    out_.write(reinterpret_cast<const char*>(data), static_cast<std::streamsize>(len));
    if (!out_.good()) {
        CFLOG_ERROR("BinaryWriter: Write failed at offset " + std::to_string(bytes_written_), true);
        return false;
    }

    bytes_written_ += len;
    return true;
}

void BinaryWriter::flush() {
    if (out_.is_open()) {
        out_.flush();
    }
}

void BinaryWriter::close() {
    if (out_.is_open()) {
        out_.flush();
        out_.close();
    }
}

bool BinaryWriter::is_open() const {
    return out_.is_open();
}

} // namespace QC
