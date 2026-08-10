// Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
// SPDX-License-Identifier: BSD 3-Clause Clear License
//
// Binary file writer — streaming raw byte output for QDSS/ADPL captures.

#ifndef QC_BINARY_WRITER_H
#define QC_BINARY_WRITER_H

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <fstream>
#include <string>

namespace QC {

/// A simple streaming binary file writer for raw capture data (QDSS, ADPL).
/// Writes raw bytes directly to disk as they arrive — no buffering of the
/// full capture in memory.
class BinaryWriter {
public:
    BinaryWriter() = default;
    ~BinaryWriter();

    // Non-copyable, movable
    BinaryWriter(const BinaryWriter&) = delete;
    BinaryWriter& operator=(const BinaryWriter&) = delete;
    BinaryWriter(BinaryWriter&&) noexcept;
    BinaryWriter& operator=(BinaryWriter&&) noexcept;

    /// Open a file for binary writing. Returns false on failure.
    bool open(const std::string& path);

    /// Write raw bytes to the file. Returns false on failure.
    bool write(const uint8_t* data, size_t len);

    /// Flush pending writes to disk.
    void flush();

    /// Close the file. Safe to call multiple times.
    void close();

    /// Returns true if the file is currently open and writable.
    bool is_open() const;

    /// Returns the total number of bytes written so far.
    size_t bytes_written() const { return bytes_written_; }

    /// Returns the file path (empty if not opened).
    const std::string& path() const { return path_; }

private:
    std::ofstream out_;
    std::string path_;
    size_t bytes_written_ = 0;
};

} // namespace QC

#endif // QC_BINARY_WRITER_H
