// Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
// SPDX-License-Identifier: BSD 3-Clause Clear License
//
// QLC metadata file writer. Produces a binary .qlc file capturing
// device metadata gathered during the handshake.

#ifndef DIAGLOGGER_QLC_WRITER_H
#define DIAGLOGGER_QLC_WRITER_H

#include "diaglogger/types.h"
#include "diaglogger/version.h"
#include "diag/handshake_commands.h"
#include "diag/qdss_commands.h"

#include <string>

namespace diaglogger {

/// Device information aggregated from four diag commands.
struct QlcDeviceInfo {
    VernoResponse verno;
    ExtBuildIdResponse ext_build_id;
    EsnResponse esn;
    StatusResponse status;
};

/// Aggregated metadata for the .qlc file.
struct QlcMetadata {
    std::string version = VERSION_STRING;

    // HDLC data type: 0 = hdlc encoding removed, 1 = hdlc encoding enabled.
    uint8_t hdlc_data_type = 1;

    // GUIDList from QShrink4 FILE_LIST responses
    std::vector<QshrinkFileEntry> qshrink_guids;

    // DiagID table
    DiagIdTableResult diagid_table;

    // Security keys
    SecurityKeyResult security_keys;

    // QDSS ATID info
    QdssHwAccelInfo qdss_hw_accel;

    // Device info from 4 diag commands
    QlcDeviceInfo device_info;
};

class QlcWriter {
public:
    /// Write the .qlc metadata file. Returns true on success.
    static bool write(const std::string& path, const QlcMetadata& metadata);
};

} // namespace diaglogger

#endif // DIAGLOGGER_QLC_WRITER_H
