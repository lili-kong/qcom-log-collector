// SPDX-License-Identifier: Apache-2.0
// Copyright 2024 Qualcomm Innovation Center, Inc.

#include "qlc_writer.h"

#include <fstream>
#include <cstring>
#include <KL/Macros.h>

namespace diaglogger {

static void append_u32(ByteBuffer& buf, uint32_t val) {
    buf.push_back(static_cast<uint8_t>(val & 0xFF));
    buf.push_back(static_cast<uint8_t>((val >> 8) & 0xFF));
    buf.push_back(static_cast<uint8_t>((val >> 16) & 0xFF));
    buf.push_back(static_cast<uint8_t>((val >> 24) & 0xFF));
}

static void append_u16(ByteBuffer& buf, uint16_t val) {
    buf.push_back(static_cast<uint8_t>(val & 0xFF));
    buf.push_back(static_cast<uint8_t>((val >> 8) & 0xFF));
}

static void write_fixed_ascii(ByteBuffer& buf, const std::string& str, size_t width) {
    for (size_t i = 0; i < width; ++i) {
        buf.push_back(i < str.size() ? static_cast<uint8_t>(str[i]) : 0x00);
    }
}

bool QlcWriter::write(const std::string& path, const QlcMetadata& metadata) {
    ByteBuffer buf;

    // --- HeaderLength placeholder (4 bytes LE) ---
    append_u32(buf, 0);

    // --- Version (6 bytes, fixed-width ASCII) ---
    write_fixed_ascii(buf, metadata.version, 6);

    // --- HDLCDataType (1 byte) ---
    buf.push_back(metadata.hdlc_data_type);

    // --- GUIDListEntryCount (4 bytes LE) + GUID array (16 bytes each) ---
    uint32_t guid_count = static_cast<uint32_t>(metadata.qshrink_guids.size());
    append_u32(buf, guid_count);
    for (const auto& entry : metadata.qshrink_guids) {
        buf.insert(buf.end(), entry.guid.begin(), entry.guid.end());
    }

    // --- DiagIDEntryCount (4 bytes LE) ---
    uint32_t diagid_count = static_cast<uint32_t>(metadata.diagid_table.entries.size());
    append_u32(buf, diagid_count);

    // --- DiagID entries: each is 47 bytes = [1] DiagID + [30] string + [16] GUID ---
    for (const auto& entry : metadata.diagid_table.entries) {
        buf.push_back(entry.diag_id);
        write_fixed_ascii(buf, entry.processor_name, 30);
        buf.insert(buf.end(), entry.guid.begin(), entry.guid.end());
    }

    // --- KeyCount (1 byte) ---
    uint8_t key_count = static_cast<uint8_t>(metadata.security_keys.keys.size());
    buf.push_back(key_count);

    // --- KeyInfo entries (variable size per entry) ---
    for (const auto& key : metadata.security_keys.keys) {
        buf.push_back(key.version);
        buf.push_back(key.classifier);
        buf.push_back(key.public_type);
        buf.push_back(key.reserved);
        append_u16(buf, key.public_len);
        append_u16(buf, key.wrapped_len);
        buf.insert(buf.end(), key.public_key.begin(), key.public_key.end());
        buf.insert(buf.end(), key.wrapped_key.begin(), key.wrapped_key.end());
    }

    // --- ATIDEntryCount (2 bytes LE) + ATID entries ---
    const auto& hw = metadata.qdss_hw_accel;
    struct AtidEntry { uint8_t hw_type; uint8_t atid; };
    AtidEntry atid_entries[3];
    uint16_t atid_count = 0;
    if (hw.stm_supported) atid_entries[atid_count++] = { QdssHwAccelType::STM, hw.stm_atid };
    if (hw.atb_supported) atid_entries[atid_count++] = { QdssHwAccelType::ATB, hw.atb_atid };
    if (hw.lpass_stm_supported) atid_entries[atid_count++] = { QdssHwAccelType::LPASS_STM, hw.lpass_stm_atid };
    append_u16(buf, atid_count);
    for (uint16_t i = 0; i < atid_count; ++i) {
        buf.push_back(atid_entries[i].hw_type);
        buf.push_back(atid_entries[i].atid);
    }

    // --- DeviceInfoLength (4 bytes LE) + DeviceInfo (raw bytes) ---
    const auto& dev = metadata.device_info;
    uint32_t dev_info_len = static_cast<uint32_t>(
        dev.verno.raw.size() + dev.ext_build_id.raw.size() +
        dev.esn.raw.size() + dev.status.raw.size());
    append_u32(buf, dev_info_len);
    buf.insert(buf.end(), dev.verno.raw.begin(), dev.verno.raw.end());
    buf.insert(buf.end(), dev.ext_build_id.raw.begin(), dev.ext_build_id.raw.end());
    buf.insert(buf.end(), dev.esn.raw.begin(), dev.esn.raw.end());
    buf.insert(buf.end(), dev.status.raw.begin(), dev.status.raw.end());

    // --- Fill in HeaderLength placeholder (first 4 bytes) ---
    uint32_t header_length = static_cast<uint32_t>(buf.size());
    buf[0] = static_cast<uint8_t>(header_length & 0xFF);
    buf[1] = static_cast<uint8_t>((header_length >> 8) & 0xFF);
    buf[2] = static_cast<uint8_t>((header_length >> 16) & 0xFF);
    buf[3] = static_cast<uint8_t>((header_length >> 24) & 0xFF);

    // --- Write to file ---
    std::ofstream out(path, std::ios::binary);
    if (!out.is_open()) {
        CFLOG_ERROR("Cannot open QLC metadata file: " + path, true);
        return false;
    }

    out.write(reinterpret_cast<const char*>(buf.data()),
              static_cast<std::streamsize>(buf.size()));
    out.flush();
    return out.good();
}

} // namespace diaglogger
