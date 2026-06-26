// SPDX-License-Identifier: Apache-2.0
// Copyright 2024 Qualcomm Innovation Center, Inc.
//
// Unit tests for CFG file parser (HDLC and Non-HDLC command stream formats).

#include "diag/cfg_parser.h"
#include "diag/commands.h"
#include "hdlc/hdlc.h"
#include "hdlc/non_hdlc.h"
#include <cassert>
#include <iostream>
#include <fstream>
#include <cstring>
#include <cstdio>

using namespace diaglogger;

static int tests_passed = 0;
static int tests_failed = 0;

#define TEST(name) \
    static void test_##name(); \
    static struct Register_##name { \
        Register_##name() { \
            std::cout << "  " #name "... "; \
            try { test_##name(); std::cout << "PASS\n"; ++tests_passed; } \
            catch (const std::exception& e) { std::cout << "FAIL: " << e.what() << "\n"; ++tests_failed; } \
            catch (...) { std::cout << "FAIL: unknown exception\n"; ++tests_failed; } \
        } \
    } register_##name; \
    static void test_##name()

#define ASSERT_EQ(a, b) do { \
    if ((a) != (b)) throw std::runtime_error( \
        std::string("ASSERT_EQ failed at line ") + std::to_string(__LINE__)); \
} while(0)

#define ASSERT_TRUE(x) do { \
    if (!(x)) throw std::runtime_error( \
        std::string("Assertion failed at line ") + std::to_string(__LINE__)); \
} while(0)

// Helper: build an HDLC stream from a list of command payloads
static ByteBuffer build_hdlc_stream(const std::vector<ByteBuffer>& payloads) {
    ByteBuffer stream;
    for (const auto& payload : payloads) {
        ByteBuffer frame = hdlc_encode(payload);
        stream.insert(stream.end(), frame.begin(), frame.end());
    }
    return stream;
}

// Helper: build a Non-HDLC stream from a list of command payloads
static ByteBuffer build_non_hdlc_stream(const std::vector<ByteBuffer>& payloads) {
    ByteBuffer stream;
    for (const auto& payload : payloads) {
        ByteBuffer frame = non_hdlc_encode(payload);
        stream.insert(stream.end(), frame.begin(), frame.end());
    }
    return stream;
}

// Helper: write bytes to a temporary file, returns the path
static std::string write_temp_file(const ByteBuffer& data, const std::string& extension) {
    std::string path = "test_cfg_temp" + extension;
    std::ofstream f(path, std::ios::binary);
    f.write(reinterpret_cast<const char*>(data.data()), data.size());
    f.close();
    return path;
}

// --- Detection tests ---

TEST(detect_hdlc_by_extension) {
    ByteBuffer dummy = {0x00, 0x01, 0x02};
    CfgVersion ver = detect_cfg_version(dummy, "test.cfg");
    ASSERT_EQ(static_cast<int>(ver), static_cast<int>(CfgVersion::Hdlc));
}

TEST(detect_non_hdlc_by_extension) {
    ByteBuffer dummy = {0x00, 0x01, 0x02};
    CfgVersion ver = detect_cfg_version(dummy, "test.cfg2");
    ASSERT_EQ(static_cast<int>(ver), static_cast<int>(CfgVersion::NonHdlc));
}

TEST(detect_unknown_without_extension) {
    ByteBuffer dummy = {0x7E, 0x01, 0x02};
    CfgVersion ver = detect_cfg_version(dummy, "test.bin");
    ASSERT_EQ(static_cast<int>(ver), static_cast<int>(CfgVersion::Unknown));
}

TEST(detect_unknown_empty_path) {
    ByteBuffer dummy = {0x7E, 0x01, 0x02};
    CfgVersion ver = detect_cfg_version(dummy);
    ASSERT_EQ(static_cast<int>(ver), static_cast<int>(CfgVersion::Unknown));
}

// --- HDLC parsing tests ---

TEST(parse_hdlc_returns_all_commands) {
    ByteBuffer cmd1 = {DiagCmd::LOG_CONFIG, 0x00, 0x00, 0x00, 0x03, 0x00, 0x00, 0x00};
    ByteBuffer cmd2 = {DiagCmd::SET_EVENT_MASK, 0x00, 0x00, 0x00, 0x08, 0x00, 0xFF};
    ByteBuffer cmd3 = {DiagCmd::EVENT_RPT, 0x01};

    ByteBuffer stream = build_hdlc_stream({cmd1, cmd2, cmd3});

    std::string path = write_temp_file(stream, ".cfg");
    auto commands = parse_cfg_file(path);
    std::remove(path.c_str());

    ASSERT_EQ(commands.size(), 3u);
    ASSERT_EQ(commands[0], cmd1);
    ASSERT_EQ(commands[1], cmd2);
    ASSERT_EQ(commands[2], cmd3);
}

TEST(parse_hdlc_preserves_subsystem_commands) {
    // Subsystem command: 0x4B + subsys_id + subcmd_lo + subcmd_hi + payload
    ByteBuffer subsys_cmd = {0x4B, 0x12, 0x18, 0x02, 0x01};
    ByteBuffer log_cmd = {DiagCmd::LOG_CONFIG, 0x00, 0x00, 0x00};

    ByteBuffer stream = build_hdlc_stream({subsys_cmd, log_cmd});

    std::string path = write_temp_file(stream, ".cfg");
    auto commands = parse_cfg_file(path);
    std::remove(path.c_str());

    ASSERT_EQ(commands.size(), 2u);
    ASSERT_EQ(commands[0], subsys_cmd);
    ASSERT_EQ(commands[1], log_cmd);
}

// --- Non-HDLC parsing tests ---

TEST(parse_non_hdlc_returns_all_commands) {
    ByteBuffer cmd1 = {DiagCmd::LOG_CONFIG, 0x00, 0x00, 0x00, 0x03, 0x00, 0x00, 0x00};
    ByteBuffer cmd2 = {DiagCmd::SET_EVENT_MASK, 0x00, 0x00, 0x00, 0x08, 0x00, 0xFF};
    ByteBuffer cmd3 = {DiagCmd::EXT_MSG_CFG, 0x04, 0x64, 0x00, 0x66, 0x00, 0x00, 0x00};

    ByteBuffer stream = build_non_hdlc_stream({cmd1, cmd2, cmd3});

    std::string path = write_temp_file(stream, ".cfg2");
    auto commands = parse_cfg_file(path);
    std::remove(path.c_str());

    ASSERT_EQ(commands.size(), 3u);
    ASSERT_EQ(commands[0], cmd1);
    ASSERT_EQ(commands[1], cmd2);
    ASSERT_EQ(commands[2], cmd3);
}

// --- Error handling tests ---

TEST(parse_cfg_file_nonexistent_throws) {
    bool threw = false;
    try {
        parse_cfg_file("/nonexistent/path/file.cfg");
    } catch (const std::runtime_error&) {
        threw = true;
    }
    ASSERT_TRUE(threw);
}

TEST(parse_cfg_file_unknown_format_throws) {
    ByteBuffer garbage = {0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08};
    std::string path = write_temp_file(garbage, ".bin");
    bool threw = false;
    try {
        parse_cfg_file(path);
    } catch (const std::runtime_error&) {
        threw = true;
    }
    std::remove(path.c_str());
    ASSERT_TRUE(threw);
}

int main() {
    std::cout << "CFG Parser Tests:\n";
    std::cout << "\nResults: " << tests_passed << " passed, "
              << tests_failed << " failed\n";
    return tests_failed > 0 ? 1 : 0;
}
