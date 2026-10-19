// Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
// SPDX-License-Identifier: BSD 3-Clause Clear License
//
// QShrink4 database helpers: validate a downloaded .qdb buffer and decompress
// its zlib payload into a .qsr4 file.

#ifndef QC_QSR4_WRITER_H
#define QC_QSR4_WRITER_H

#include "types.h"
#include <array>
#include <string>

namespace QC {

// .qdb header is 64 bytes: [0x7F]["QDB"][16B GUID][u32 version][40B reserved].
constexpr size_t QDB_HEADER_SIZE = 64;

/// Validate a downloaded .qdb buffer: checks magic 0x7F 'Q' 'D' 'B', extracts the
/// 16-byte GUID (bytes 4..19), and requires header version (bytes 20..23 LE) == 0.
/// Returns true and fills out_guid on success.
bool validate_qdb_header(const ByteBuffer& qdb, std::array<uint8_t, 16>& out_guid);

/// Format a 16-byte GUID as the standard string xxxxxxxx-xxxx-xxxx-xxxx-xxxxxxxxxxxx.
std::string guid_to_string(const std::array<uint8_t, 16>& guid);

/// Decompress the zlib payload after the 64-byte header of `qdb` and write the
/// result to `out_path`. Returns true on success. Logs nothing (caller logs).
bool decompress_qdb_to_qsr4(const ByteBuffer& qdb, const std::string& out_path);

} // namespace QC

#endif // QC_QSR4_WRITER_H
