// Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
// SPDX-License-Identifier: BSD 3-Clause Clear License

#include <array>
#include <atomic>
#include <chrono>
#include <functional>
#include <future>
#include <iostream>
#include <stdexcept>

#include "qdpublic.h"
#include "transport/qcdevice_transport.h"
#include "transport/transport_manager.h"

namespace {

using namespace std::chrono_literals;

std::function<HANDLE()> native_open;
std::function<BOOL()> native_read = [] { return TRUE; };
int open_calls = 0;
int close_calls = 0;

#define CHECK(condition)                                                                           \
    do {                                                                                           \
        if (!(condition)) {                                                                        \
            throw std::runtime_error("Check failed: " #condition);                                 \
        }                                                                                          \
    } while (false)

void test_cancel_before_open() {
    open_calls = 0;
    std::atomic<bool> cancelled{true};
    QC::QcDeviceTransport transport("test port");
    CHECK(!transport.open(cancelled));
    CHECK(open_calls == 0);
}

void test_cancel_stops_retries() {
    open_calls = 0;
    std::atomic<bool> cancelled{false};
    std::promise<void> first_attempt;
    auto attempted = first_attempt.get_future();
    native_open = [&] {
        if (open_calls == 1) {
            first_attempt.set_value();
        }
        return INVALID_HANDLE_VALUE;
    };
    QC::QcDeviceTransport transport("test port");
    auto opening = std::async(std::launch::async, [&] { return transport.open(cancelled); });
    CHECK(attempted.wait_for(1s) == std::future_status::ready);
    cancelled.store(true);
    CHECK(opening.wait_for(1s) == std::future_status::ready);
    CHECK(!opening.get());
    CHECK(open_calls == 1);
    CHECK(!transport.is_open());
}

void test_cancel_closes_late_handle() {
    close_calls = 0;
    std::atomic<bool> cancelled{false};
    native_open = [&] {
        cancelled.store(true);
        return CreateEventW(nullptr, TRUE, FALSE, nullptr);
    };
    QC::QcDeviceTransport transport("test port");
    CHECK(!transport.open(cancelled));
    CHECK(!transport.is_open());
    CHECK(close_calls == 1);
    transport.close();
    CHECK(close_calls == 1);
}

void test_retry_then_success_without_cancellation() {
    open_calls = 0;
    close_calls = 0;
    native_open = [] {
        return open_calls == 1 ? INVALID_HANDLE_VALUE : CreateEventW(nullptr, TRUE, FALSE, nullptr);
    };
    const std::atomic<bool> cancelled{false};
    QC::QcDeviceTransport transport("test port");
    CHECK(transport.open(cancelled));
    CHECK(transport.is_connected());
    CHECK(open_calls == 2);
    transport.close();
    CHECK(close_calls == 1);
}

void test_read_failure_defers_handle_close(bool throw_on_read) {
    close_calls = 0;
    native_open = [] { return CreateEventW(nullptr, TRUE, FALSE, nullptr); };
    native_read = [throw_on_read]() -> BOOL {
        if (throw_on_read) {
            throw std::runtime_error("Simulated driver read failure");
        }
        return FALSE;
    };
    const std::atomic<bool> cancelled{false};
    QC::QcDeviceTransport transport("test port");
    CHECK(transport.open(cancelled));
    std::array<uint8_t, 16> buffer{};
    CHECK(transport.read(buffer.data(), buffer.size(), 100) == 0);
    CHECK(transport.is_disconnected());
    CHECK(transport.is_open());
    CHECK(close_calls == 0);
    transport.close();
    CHECK(!transport.is_open());
    CHECK(close_calls == 1);
    transport.close();
    CHECK(close_calls == 1);
}

} // namespace

// Stub the discovery boundary so these tests never query real devices.
namespace QC {

TransportManager& TransportManager::instance() {
    static TransportManager manager;
    return manager;
}

std::string TransportManager::resolve_device_name(const std::string& name) { return name; }
std::string TransportManager::extract_com_port(const std::string&) { return {}; }
bool TransportManager::is_com_port_in_use(const std::string&) { return false; }
std::wstring TransportManager::to_wide(const std::string& name) {
    return std::wstring(name.begin(), name.end());
}

} // namespace QC

namespace QcDevice {

HANDLE OpenDevice(PVOID, DWORD, BOOL) {
    ++open_calls;
    return native_open();
}

VOID CloseDevice(HANDLE handle) {
    ++close_calls;
    CloseHandle(handle);
}

BOOL ReadFromDevice(HANDLE, PVOID, DWORD, PDWORD bytes) {
    *bytes = 0;
    return native_read();
}

BOOL SendToDevice(HANDLE, PVOID, DWORD size, PDWORD bytes) {
    *bytes = size;
    return TRUE;
}

} // namespace QcDevice

int main() {
    try {
        test_cancel_before_open();
        test_cancel_stops_retries();
        test_cancel_closes_late_handle();
        test_retry_then_success_without_cancellation();
        test_read_failure_defers_handle_close(false);
        test_read_failure_defers_handle_close(true);
        std::cout << "QcDevice transport cancellation tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
