// Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
// SPDX-License-Identifier: BSD 3-Clause Clear License

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <iostream>
#include <memory>
#include <mutex>
#include <queue>
#include <stdexcept>
#include <string>
#include <utility>

#include "protocol/diag.h"
#include "protocol/protocol.h"

namespace {

using namespace std::chrono_literals;

#define CHECK(condition)                                                                           \
    do {                                                                                           \
        if (!(condition)) {                                                                        \
            throw std::runtime_error("Check failed: " #condition);                                 \
        }                                                                                          \
    } while (false)

struct PortActivity {
    std::mutex mutex;
    std::condition_variable cv;
    bool release_open = false;
    int opens = 0;
    int closes = 0;
    int writes = 0;
    bool echo_responses = false;
    int reads = 0;
    bool block_read = false;
    bool release_read = false;
    bool read_in_progress = false;
    bool closed_during_read = false;

    bool wait_for(int PortActivity::* counter, int count) {
        std::unique_lock<std::mutex> lock(mutex);
        return cv.wait_for(lock, 2s, [&] { return this->*counter >= count; });
    }

    void allow_open() {
        std::lock_guard<std::mutex> lock(mutex);
        release_open = true;
        cv.notify_all();
    }
};

class TestTransport : public QC::Transport {
  public:
    explicit TestTransport(PortActivity& activity) : activity_(activity) {}

    bool open(const std::atomic<bool>& cancel_requested) override {
        if (cancel_requested.load()) {
            return false;
        }
        std::unique_lock<std::mutex> lock(activity_.mutex);
        ++activity_.opens;
        activity_.cv.notify_all();
        // Model a native open call that returns a handle after a close request.
        if (!activity_.cv.wait_for(lock, 2s, [&] { return activity_.release_open; })) {
            return false;
        }
        state_.store(QC::TransportState::CONNECTED);
        return true;
    }

    void close() override {
        std::lock_guard<std::mutex> lock(activity_.mutex);
        activity_.closed_during_read = activity_.closed_during_read || activity_.read_in_progress;
        if (is_open()) {
            ++activity_.closes;
        }
        state_.store(QC::TransportState::CLOSED);
        activity_.cv.notify_all();
    }

    size_t write(const uint8_t* data, size_t size) override {
        std::lock_guard<std::mutex> lock(activity_.mutex);
        ++activity_.writes;
        if (activity_.echo_responses) {
            responses_.emplace(data, data + size);
        }
        activity_.cv.notify_all();
        return size;
    }

    size_t read(uint8_t* data, size_t size, uint32_t timeout_ms) override {
        std::unique_lock<std::mutex> lock(activity_.mutex);
        ++activity_.reads;
        activity_.cv.notify_all();
        if (activity_.block_read) {
            activity_.read_in_progress = true;
            activity_.cv.wait_for(lock, 2s, [&] { return activity_.release_read; });
            activity_.read_in_progress = false;
            return 0;
        }
        activity_.cv.wait_for(lock, std::chrono::milliseconds(timeout_ms),
                              [&] { return !responses_.empty(); });
        if (responses_.empty()) {
            return 0;
        }
        const auto response = std::move(responses_.front());
        responses_.pop();
        CHECK(response.size() <= size);
        std::copy(response.begin(), response.end(), data);
        return response.size();
    }

    std::string description() const override { return "test port"; }

  private:
    PortActivity& activity_;
    std::queue<QC::ByteBuffer> responses_;
};

class TestProtocol : public QC::Protocol {
  public:
    explicit TestProtocol(PortActivity& activity)
        : Protocol(std::make_unique<TestTransport>(activity)) {}

    ~TestProtocol() override { stop(); }

    QC::ByteBuffer send_and_wait(const QC::ByteBuffer&, uint32_t) override { return {}; }
};

void test_close_waits_for_open_cleanup() {
    PortActivity activity;
    TestProtocol protocol(activity);
    protocol.request_open();
    CHECK(activity.wait_for(&PortActivity::opens, 1));
    protocol.request_close();
    const bool closed_while_opening = protocol.wait_closed(20ms);
    activity.allow_open();
    CHECK(activity.wait_for(&PortActivity::closes, 1));
    CHECK(protocol.wait_closed(1s));
    CHECK(!closed_while_opening);
    CHECK(!protocol.is_open());
    CHECK(protocol.state() == QC::STATE_DISCONNECTED);
}

void test_close_then_open_restarts_cancelled_attempt() {
    PortActivity activity;
    TestProtocol protocol(activity);
    protocol.request_open();
    CHECK(activity.wait_for(&PortActivity::opens, 1));
    protocol.request_close();
    protocol.request_open();
    activity.allow_open();
    CHECK(protocol.wait_ready(1s));
    {
        std::lock_guard<std::mutex> lock(activity.mutex);
        CHECK(activity.opens == 2);
        CHECK(activity.closes == 1);
    }
    protocol.request_close();
    CHECK(protocol.wait_closed(1s));
}

void test_duplicate_open_keeps_current_attempt() {
    PortActivity activity;
    TestProtocol protocol(activity);
    protocol.request_open();
    CHECK(activity.wait_for(&PortActivity::opens, 1));
    protocol.request_open();
    activity.allow_open();
    CHECK(protocol.wait_ready(1s));
    {
        std::lock_guard<std::mutex> lock(activity.mutex);
        CHECK(activity.opens == 1);
        CHECK(activity.closes == 0);
    }
    protocol.request_close();
    CHECK(protocol.wait_closed(1s));
}

void test_close_cancels_diag_response_wait() {
    PortActivity activity;
    activity.allow_open();
    QC::DiagConfig config;
    config.command_timeout_ms = 3000;
    QC::Diag protocol(config, std::make_unique<TestTransport>(activity));
    protocol.request_open();
    // First write nudges HDLC; second write begins the paired loopback wait.
    CHECK(activity.wait_for(&PortActivity::writes, 2));
    protocol.request_close();
    CHECK(!protocol.wait_ready(20ms));
    CHECK(protocol.wait_closed(1s));
    CHECK(protocol.state() == QC::STATE_DISCONNECTED);
    std::lock_guard<std::mutex> lock(activity.mutex);
    CHECK(activity.writes == 2);
}

void test_close_cancels_diag_startup_delay_and_allows_reopen() {
    PortActivity activity;
    activity.allow_open();
    QC::DiagConfig config;
    config.hdlc_only = true;
    QC::Diag protocol(config, std::make_unique<TestTransport>(activity));
    protocol.request_open();
    CHECK(activity.wait_for(&PortActivity::writes, 1));
    protocol.request_close();
    CHECK(protocol.wait_closed(1s));
    {
        std::lock_guard<std::mutex> lock(activity.mutex);
        CHECK(activity.writes == 1);
        activity.echo_responses = true;
    }
    protocol.request_open();
    CHECK(protocol.wait_ready(2s));
    CHECK(protocol.state() == QC::STATE_AVAILABLE);
    protocol.request_close();
    CHECK(protocol.wait_closed(1s));
}

void test_close_waits_for_rx_exit() {
    PortActivity activity;
    activity.block_read = true;
    activity.allow_open();
    TestProtocol protocol(activity);
    protocol.request_open();
    CHECK(activity.wait_for(&PortActivity::reads, 1));
    protocol.request_close();
    const bool closed_before_read_returned = protocol.wait_closed(20ms);
    {
        std::lock_guard<std::mutex> lock(activity.mutex);
        activity.release_read = true;
        activity.cv.notify_all();
    }
    CHECK(protocol.wait_closed(1s));
    CHECK(!closed_before_read_returned);
    CHECK(protocol.state() == QC::STATE_DISCONNECTED);
    std::lock_guard<std::mutex> lock(activity.mutex);
    CHECK(activity.closes == 1);
    CHECK(activity.reads == 1);
    CHECK(!activity.read_in_progress);
    CHECK(!activity.closed_during_read);
}

} // namespace

int main() {
    const std::array<std::pair<const char*, void (*)()>, 6> tests = {{
        {"close waits for open cleanup", test_close_waits_for_open_cleanup},
        {"close then open restarts cancelled attempt",
         test_close_then_open_restarts_cancelled_attempt},
        {"duplicate open keeps current attempt", test_duplicate_open_keeps_current_attempt},
        {"close cancels DIAG response wait", test_close_cancels_diag_response_wait},
        {"close cancels DIAG startup delay and allows reopen",
         test_close_cancels_diag_startup_delay_and_allows_reopen},
        {"close waits for RX exit", test_close_waits_for_rx_exit},
    }};
    int failures = 0;
    for (const auto& [name, test] : tests) {
        try {
            test();
            std::cout << "PASS: " << name << '\n';
        } catch (const std::exception& error) {
            ++failures;
            std::cerr << "FAIL: " << name << ": " << error.what() << '\n';
        }
    }
    return failures == 0 ? 0 : 1;
}
