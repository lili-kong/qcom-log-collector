// Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
// SPDX-License-Identifier: BSD 3-Clause Clear License
//
// Protocol base implementation: lifecycle worker, TX and RX worker threads,
// and the single-outstanding request/response slot. Raw bytes only — no
// framing. All I/O goes through the owned Transport.

#include "protocol.h"
#include "transport/transport_manager.h"

#include <KL/Macros.h>

#include <chrono>
#include <utility>
#include <vector>

namespace QC {

Protocol::Protocol(std::unique_ptr<Transport> transport)
    : transport_(std::move(transport))
{
}

Protocol::~Protocol() {
    // Subclasses should call stop() first in their own destructor while
    // their vtable is still valid. This call is a safety net — if the
    // subclass already stopped cleanly, state is CLOSED and every step
    // here is a no-op.
    stop();
}

bool Protocol::is_open() const {
    return transport_ && transport_->is_open();
}

bool Protocol::is_connected() const {
    return transport_ && transport_->is_connected();
}

bool Protocol::is_disconnected() const {
    return transport_ && transport_->is_disconnected();
}

std::string Protocol::description() const {
    return transport_ ? transport_->description() : std::string();
}

void Protocol::set_verbose(bool v) {
    if (transport_) {
        transport_->set_verbose(v);
    }
}

// --- Protocol state ---

std::string Protocol::state_to_string(ProtocolState state) {
    switch (state) {
        case STATE_AVAILABLE:    return "AVAILABLE";
        case STATE_DISCONNECTED: return "DISCONNECTED";
        case STATE_UNRESPONSIVE: return "UNRESPONSIVE";
        case STATE_INITIALIZING: return "INITIALIZING";
        default:                 return "UNKNOWN";
    }
}

void Protocol::set_state(ProtocolState state) {
    ProtocolState prev = protocol_state_.exchange(state);
    if (prev != state) {
        FLOG_DEBUG("Protocol state: " + state_to_string(prev)
                   + " -> " + state_to_string(state));
        TransportManager::instance().notify_protocol_state_changed(handle(), state);
    }
}

bool Protocol::lifecycle_open() {
    if (open_cancelled()) {
        return false;
    }
    set_state(STATE_INITIALIZING);
    if (port_trace_enabled_) {
        // Fill description into the metadata if the subclass didn't.
        if (port_trace_meta_.device_description.empty()) {
            port_trace_meta_.device_description = description();
        }
        if (!ptrace_.open(port_trace_path_, port_trace_meta_)) {
            CFLOG_ERROR("Error: Cannot create port trace file in " + port_trace_path_, true);
            set_state(STATE_DISCONNECTED);
            return false;
        }
        CFLOG_INFO("Port trace enabled: " + port_trace_path_, true);
    }
    if (!transport_->open(open_cancel_requested_)) {
        if (port_trace_enabled_) ptrace_.close();
        set_state(STATE_DISCONNECTED);
        return false;
    }
    if (open_cancelled()) {
        lifecycle_close();
        return false;
    }
    start_io_threads();
    // A subclass that overrides this marks STATE_AVAILABLE itself once its
    // handshake passes; for a bare Protocol an open port is enough.
    set_state(STATE_AVAILABLE);
    return true;
}

void Protocol::lifecycle_close() {
    // Close before joining: closing is what unblocks a pending transport_->read().
    transport_->close();
    stop_io_threads();
    if (port_trace_enabled_) {
        ptrace_.close();
    }
    set_state(STATE_DISCONNECTED);
}

void Protocol::start_io_threads() {
    if (io_running_.load()) {
        return;
    }
    io_running_.store(true);
    tx_thread_ = std::thread(&Protocol::tx_loop, this);
    rx_thread_ = std::thread(&Protocol::rx_loop, this);
    receiver_thread_ = std::thread([this] { receiver_loop(); });
    dispatcher_thread_ = std::thread([this] { dispatcher_loop(); });
}

void Protocol::stop_io_threads() {
    io_running_.store(false);

    tx_cv_.notify_all();
    rx_cv_.notify_all();
    if (tx_thread_.joinable()) {
        tx_thread_.join();
    }
    if (rx_thread_.joinable()) {
        rx_thread_.join();
    }
    // Join the consumer loops after the raw RX pump has stopped feeding
    // them and notified rx_cv_ on exit — any pending next_rx() returns
    // and the loops can see io_running_ cleared.
    if (receiver_thread_.joinable()) {
        receiver_thread_.join();
    }
    if (dispatcher_thread_.joinable()) {
        dispatcher_thread_.join();
    }

    {
        std::lock_guard<std::mutex> lock(tx_mutex_);
        while (!tx_queue_.empty()) {
            tx_queue_.pop();
        }
    }
    // rx_queue_ is intentionally NOT drained here: consumers (e.g. the
    // receiver loop) may still need to process bytes already buffered
    // before the port went down.
}

bool Protocol::submit_tx(ByteBuffer data) {
    if (!io_running_.load() || open_cancelled()) {
        return false;
    }
    {
        std::lock_guard<std::mutex> lock(tx_mutex_);
        tx_queue_.push(std::move(data));
    }
    tx_cv_.notify_one();
    return true;
}

void Protocol::tx_loop() {
    while (io_running_.load()) {
        // Port must exist and be available, or the TX loop ends.
        if (!is_open() || !is_connected()) {
            break;
        }

        ByteBuffer data;
        {
            std::unique_lock<std::mutex> lock(tx_mutex_);
            tx_cv_.wait(lock, [this] {
                return !tx_queue_.empty() || !io_running_.load();
            });

            if (!io_running_.load()) {
                break;
            }
            data = std::move(tx_queue_.front());
            tx_queue_.pop();
        }

        // Re-check after the wait: the port may have gone away meanwhile.
        if (!is_open() || !is_connected() || open_cancelled()) {
            break;
        }
        if (!data.empty()) {
            if (port_trace_enabled_) {
                ptrace_.write_record(Direction::TX, data);
            }
            transport_->write(data.data(), data.size());
            tx_packets_.fetch_add(1, std::memory_order_relaxed);
            tx_bytes_.fetch_add(data.size(), std::memory_order_relaxed);
        }
    }
}

ByteBuffer Protocol::next_rx(uint32_t timeout_ms) {
    std::unique_lock<std::mutex> lock(rx_mutex_);
    if (rx_queue_.empty()) {
        rx_cv_.wait_for(lock, std::chrono::milliseconds(timeout_ms), [this] {
            return !rx_queue_.empty() || !io_running_.load();
        });
    }
    if (rx_queue_.empty()) {
        return {};
    }
    ByteBuffer data = std::move(rx_queue_.front());
    rx_queue_.pop();
    return data;
}

void Protocol::rx_loop() {
    std::vector<uint8_t> buf(IO_RX_BUFFER_SIZE);

    while (io_running_.load()) {
        // Port must exist and be available, or the RX loop ends.
        if (!is_open() || !is_connected()) {
            break;
        }

        size_t n = transport_->read(buf.data(), buf.size(), IO_RX_READ_TIMEOUT_MS);
        if (n > 0) {
            rx_reads_.fetch_add(1, std::memory_order_relaxed);
            rx_bytes_.fetch_add(n, std::memory_order_relaxed);
            if (port_trace_enabled_) {
                ptrace_.write_record(Direction::RX, buf.data(), n);
            }
            {
                std::lock_guard<std::mutex> lock(rx_mutex_);
                rx_queue_.emplace(buf.data(), buf.data() + n);
            }
            rx_cv_.notify_one();
        }
    }

    // Release any consumer blocked in next_rx().
    rx_cv_.notify_all();
}

// --- Lifecycle worker ---

void Protocol::request_open() {
    ensure_lifecycle_worker();
    {
        std::lock_guard<std::mutex> lock(lifecycle_mutex_);
        desired_open_.store(true);
    }
    lifecycle_cv_.notify_all();
}

void Protocol::request_close() {
    ensure_lifecycle_worker();
    {
        std::lock_guard<std::mutex> lock(lifecycle_mutex_);
        desired_open_.store(false);
        open_cancel_requested_.store(true);
    }
    lifecycle_cv_.notify_all();
    ready_cv_.notify_all();
}

void Protocol::ensure_lifecycle_worker() {
    if (lifecycle_running_.load()) return;
    std::lock_guard<std::mutex> lock(lifecycle_mutex_);
    if (lifecycle_running_.load()) return;
    lifecycle_running_.store(true);
    lifecycle_thread_ = std::thread(&Protocol::lifecycle_loop, this);
}

void Protocol::lifecycle_loop() {
    std::unique_lock<std::mutex> lock(lifecycle_mutex_);
    while (true) {
        lifecycle_cv_.wait(lock, [this] {
            return desired_open_.load() != is_open() || !lifecycle_running_.load() ||
                   (is_open() && open_cancelled());
        });
        if (!lifecycle_running_.load()) {
            return;
        }

        const bool opening = desired_open_.load() && !is_open();
        lifecycle_busy_ = true;
        if (opening) {
            open_cancel_requested_.store(false);
        }
        // Slow hooks and state callbacks must not hold the request mutex.
        lock.unlock();
        if (opening) {
            lifecycle_open();
        } else {
            lifecycle_close();
        }
        lock.lock();
        if (opening && open_cancelled()) {
            // A later request_open() must not rescue the cancelled attempt.
            lock.unlock();
            lifecycle_close();
            lock.lock();
        }
        lifecycle_busy_ = false;
        ready_cv_.notify_all();
    }
}

bool Protocol::wait_ready(std::chrono::milliseconds timeout) {
    std::unique_lock<std::mutex> lock(lifecycle_mutex_);
    return ready_cv_.wait_for(
        lock, timeout, [this] { return !open_cancelled() && desired_open_.load() && is_open(); });
}

bool Protocol::wait_closed(std::chrono::milliseconds timeout) {
    std::unique_lock<std::mutex> lock(lifecycle_mutex_);
    return ready_cv_.wait_for(
        lock, timeout, [this] { return !lifecycle_busy_ && !desired_open_.load() && !is_open(); });
}

bool Protocol::wait_for_open_cancel(std::chrono::milliseconds timeout) {
    std::unique_lock<std::mutex> lock(lifecycle_mutex_);
    return lifecycle_cv_.wait_for(lock, timeout, [this] { return open_cancelled(); });
}

void Protocol::stop_lifecycle_worker() {
    if (!lifecycle_running_.load() && !lifecycle_thread_.joinable()) return;
    {
        std::lock_guard<std::mutex> lock(lifecycle_mutex_);
        desired_open_.store(false);
        open_cancel_requested_.store(true);
        lifecycle_running_.store(false);
    }
    lifecycle_cv_.notify_all();
    ready_cv_.notify_all();
    if (lifecycle_thread_.joinable()) {
        lifecycle_thread_.join();
    }
}

void Protocol::stop(std::chrono::milliseconds close_timeout) {
    request_close();
    wait_closed(close_timeout);
    stop_lifecycle_worker();
}

// --- Pending-slot request/response ---

ByteBuffer Protocol::send_and_wait_frame(ByteBuffer frame,
                                         ByteBuffer signature,
                                         MatchFn /*matcher*/,
                                         std::chrono::milliseconds timeout) {
    // Only one request may be outstanding against the single pending_ slot.
    std::lock_guard<std::mutex> send_lock(send_mutex_);
    if (open_cancelled()) {
        return {};
    }

    std::promise<ByteBuffer> resp_promise;
    std::future<ByteBuffer> resp_future = resp_promise.get_future();
    {
        std::lock_guard<std::mutex> lock(pending_mutex_);
        pending_.signature = std::move(signature);
        pending_.promise = &resp_promise;
    }

    if (!submit_tx(std::move(frame))) {
        std::lock_guard<std::mutex> lock(pending_mutex_);
        pending_.promise = nullptr;
        pending_.signature.clear();
        return {};
    }

    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (io_running_.load() && !open_cancelled()) {
        if (resp_future.wait_for(std::chrono::milliseconds(10)) == std::future_status::ready) {
            std::lock_guard<std::mutex> lock(pending_mutex_);
            pending_.promise = nullptr;
            pending_.signature.clear();
            return resp_future.get();
        }
        if (std::chrono::steady_clock::now() >= deadline) {
            break;
        }
    }

    std::lock_guard<std::mutex> lock(pending_mutex_);
    pending_.promise = nullptr;
    pending_.signature.clear();
    return {};
}

bool Protocol::try_fulfill_pending(const ByteBuffer& frame,
                                   const MatchFn& matcher) {
    std::lock_guard<std::mutex> lock(pending_mutex_);
    if (pending_.promise == nullptr) return false;
    if (!matcher || !matcher(pending_.signature, frame)) return false;
    pending_.promise->set_value(frame);
    pending_.promise = nullptr;
    pending_.signature.clear();
    return true;
}

} // namespace QC
