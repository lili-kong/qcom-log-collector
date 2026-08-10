// Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
// SPDX-License-Identifier: BSD 3-Clause Clear License

#include "file_writer/qsr4_writer.h"
#include "inflate.h"

#include <cstdio>
#include <fstream>

namespace QC {

bool validate_qdb_header(const ByteBuffer& qdb, std::array<uint8_t, 16>& out_guid) {
    if (qdb.size() < QDB_HEADER_SIZE) return false;
    if (qdb[0] != 0x7F || qdb[1] != 'Q' || qdb[2] != 'D' || qdb[3] != 'B') return false;
    for (int i = 0; i < 16; ++i) out_guid[i] = qdb[4 + i];
    uint32_t version = static_cast<uint32_t>(qdb[20])
                     | (static_cast<uint32_t>(qdb[21]) << 8)
                     | (static_cast<uint32_t>(qdb[22]) << 16)
                     | (static_cast<uint32_t>(qdb[23]) << 24);
    if (version != 0) return false;
    return true;
}

std::string guid_to_string(const std::array<uint8_t, 16>& g) {
    static const char* hex = "0123456789abcdef";
    std::string s;
    s.reserve(36);
    auto push = [&](uint8_t b) { s.push_back(hex[b >> 4]); s.push_back(hex[b & 0xF]); };
    for (int i = 0; i < 16; ++i) {
        if (i == 4 || i == 6 || i == 8 || i == 10) s.push_back('-');
        push(g[i]);
    }
    return s;
}

bool decompress_qdb_to_qsr4(const ByteBuffer& qdb, const std::string& out_path) {
    if (qdb.size() <= QDB_HEADER_SIZE) return false;

    const uint8_t* comp = qdb.data() + QDB_HEADER_SIZE;
    size_t comp_len = qdb.size() - QDB_HEADER_SIZE;

    // .qdb payload is a zlib stream (matches TUF's ZLibFile::decompress).
    ByteBuffer out;
    if (!zlib_inflate(comp, comp_len, out)) return false;

    std::ofstream f(out_path, std::ios::binary | std::ios::trunc);
    if (!f) return false;
    f.write(reinterpret_cast<const char*>(out.data()), static_cast<std::streamsize>(out.size()));
    if (!f.good()) { f.close(); std::remove(out_path.c_str()); return false; }
    return true;
}

} // namespace QC
