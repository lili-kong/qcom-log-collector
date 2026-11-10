// Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
// SPDX-License-Identifier: BSD 3-Clause Clear License

#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <stdexcept>
#include <string>

#include "file_writer/ptrace_writer.h"

using namespace QC;

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

#define ASSERT_TRUE(value) do { \
    if (!(value)) throw std::runtime_error( \
        std::string("Assertion failed at line ") + std::to_string(__LINE__)); \
} while (0)

TEST(writes_to_requested_directory_and_reopens_same_capture) {
    const auto unique = std::chrono::steady_clock::now().time_since_epoch().count();
    const auto output_dir = std::filesystem::temp_directory_path() /
        ("qlc_ptrace_writer_test_" + std::to_string(unique));
    std::error_code ec;
    std::filesystem::remove_all(output_dir, ec);

    PtraceMetadata metadata;
    metadata.app_version = "test-version";
    metadata.device_description = "unit-test-device";

    PtraceWriter writer;
    ASSERT_TRUE(writer.open(output_dir.string(), metadata));
    const std::filesystem::path trace_path(writer.path());
    ASSERT_TRUE(trace_path.parent_path().lexically_normal() == output_dir.lexically_normal());

    const uint8_t tx[] = {0xDE, 0xAD, 0xBE, 0xEF};
    ASSERT_TRUE(writer.write_record(Direction::TX, tx, sizeof(tx)));
    writer.close();
    ASSERT_TRUE(std::filesystem::exists(trace_path));

    // A DIAG port can close and reopen during recovery. It should keep writing
    // to the same capture without duplicating the file header.
    ASSERT_TRUE(writer.open(output_dir.string(), metadata));
    const uint8_t rx[] = {0x01, 0x02};
    ASSERT_TRUE(writer.write_record(Direction::RX, rx, sizeof(rx)));
    writer.close();

    std::ifstream input(trace_path);
    ASSERT_TRUE(input.is_open());
    const std::string contents((std::istreambuf_iterator<char>(input)),
                               std::istreambuf_iterator<char>());
    ASSERT_TRUE(contents.find("pid,appName,appVersion,hostname,osFamily") != std::string::npos);
    ASSERT_TRUE(contents.find("Send to unit-test-device size: 4") != std::string::npos);
    ASSERT_TRUE(contents.find("DE AD BE EF ") != std::string::npos);
    ASSERT_TRUE(contents.find("Receive from unit-test-device Size: 2") != std::string::npos);
    ASSERT_TRUE(contents.find("01 02 ") != std::string::npos);
    const size_t header_at = contents.find("pid,appName,appVersion,hostname,osFamily");
    ASSERT_TRUE(contents.find("pid,appName,appVersion,hostname,osFamily", header_at + 1) ==
                std::string::npos);

    std::filesystem::remove_all(output_dir, ec);
}

// Two DiagLoggers running in the same process (the MCP server scenario) must
// not share an output directory. Each PtraceWriter instance owns its own
// path independently of any other instance or process-global state.
TEST(independent_instances_do_not_share_output_directory) {
    const auto unique = std::chrono::steady_clock::now().time_since_epoch().count();
    const auto dir_a = std::filesystem::temp_directory_path() /
        ("qlc_ptrace_writer_test_a_" + std::to_string(unique));
    const auto dir_b = std::filesystem::temp_directory_path() /
        ("qlc_ptrace_writer_test_b_" + std::to_string(unique));
    std::error_code ec;
    std::filesystem::remove_all(dir_a, ec);
    std::filesystem::remove_all(dir_b, ec);

    PtraceMetadata metadata_a;
    metadata_a.device_description = "device-a";
    PtraceMetadata metadata_b;
    metadata_b.device_description = "device-b";

    PtraceWriter writer_a;
    PtraceWriter writer_b;
    // Both open before either writes, so a shared/global directory would be
    // visible to both instances rather than only to whichever opened last.
    ASSERT_TRUE(writer_a.open(dir_a.string(), metadata_a));
    ASSERT_TRUE(writer_b.open(dir_b.string(), metadata_b));

    const std::filesystem::path path_a(writer_a.path());
    const std::filesystem::path path_b(writer_b.path());
    ASSERT_TRUE(path_a.parent_path().lexically_normal() == dir_a.lexically_normal());
    ASSERT_TRUE(path_b.parent_path().lexically_normal() == dir_b.lexically_normal());

    // Interleave writes so one instance's call can't reset the other's state.
    const uint8_t a1[] = {0xAA};
    const uint8_t b1[] = {0xBB};
    const uint8_t a2[] = {0xA2};
    ASSERT_TRUE(writer_a.write_record(Direction::TX, a1, sizeof(a1)));
    ASSERT_TRUE(writer_b.write_record(Direction::TX, b1, sizeof(b1)));
    ASSERT_TRUE(writer_a.write_record(Direction::RX, a2, sizeof(a2)));
    writer_a.close();
    writer_b.close();

    std::ifstream input_a(path_a);
    ASSERT_TRUE(input_a.is_open());
    const std::string contents_a((std::istreambuf_iterator<char>(input_a)),
                                  std::istreambuf_iterator<char>());
    std::ifstream input_b(path_b);
    ASSERT_TRUE(input_b.is_open());
    const std::string contents_b((std::istreambuf_iterator<char>(input_b)),
                                  std::istreambuf_iterator<char>());

    ASSERT_TRUE(contents_a.find("device-a") != std::string::npos);
    ASSERT_TRUE(contents_a.find("AA ") != std::string::npos);
    ASSERT_TRUE(contents_a.find("A2 ") != std::string::npos);
    // Neither instance's data or metadata leaked into the other's file.
    ASSERT_TRUE(contents_a.find("device-b") == std::string::npos);
    ASSERT_TRUE(contents_a.find("BB ") == std::string::npos);

    ASSERT_TRUE(contents_b.find("device-b") != std::string::npos);
    ASSERT_TRUE(contents_b.find("BB ") != std::string::npos);
    ASSERT_TRUE(contents_b.find("device-a") == std::string::npos);
    ASSERT_TRUE(contents_b.find("AA ") == std::string::npos);

    std::filesystem::remove_all(dir_a, ec);
    std::filesystem::remove_all(dir_b, ec);
}

int main() {
    std::cout << "\nPtraceWriter Tests\n";
    std::cout << "==================\n";
    std::cout << "Passed: " << tests_passed << "\n";
    std::cout << "Failed: " << tests_failed << "\n";
    return tests_failed == 0 ? 0 : 1;
}
