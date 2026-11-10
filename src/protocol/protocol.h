// Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
// SPDX-License-Identifier: BSD 3-Clause Clear License
//
// Protocol: framing-agnostic base for anything that speaks a protocol over
// a Transport.
//
// Owns the transport (unique_ptr<Transport>) plus all the machinery that
// used to live in Transport itself:
//   - the async lifecycle worker (request_open/request_close/wait_ready)
//   - the TX and RX pump threads and their raw-byte queues
//   - the optional receiver/dispatcher consumer threads
//   - port-trace recording
//   - the single-outstanding request/response slot
//
// The transport below deals only in raw bytes; framing, handshakes and
// response pairing are the subclass's job (see Diag).

#ifndef QC_PROTOCOL_H
#define QC_PROTOCOL_H

#include "types.h"
#include "exports/Definitions.h"
#include "file_writer/ptrace_writer.h"
#include "transport/transport.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <functional>
#include <future>
#include <memory>
#include <mutex>
#include <queue>
#include <string>
#include <thread>

namespace QC {

// ProtocolState (STATE_AVAILABLE / STATE_DISCONNECTED / STATE_UNRESPONSIVE /
// STATE_INITIALIZING) comes from exports/Definitions.h, which mirrors QIL.
// It is protocol-level and distinct from TransportState: the port can be
// open (CONNECTED) while the protocol is still handshaking
// (STATE_INITIALIZING) or the peer has stopped answering
// (STATE_UNRESPONSIVE).

/// Base class for protocol layers running on top of a Transport.
class Protocol {
public:
    /// Takes ownership of the transport it will drive.
    explicit Protocol(std::unique_ptr<Transport> transport);
    virtual ~Protocol();

    // Non-copyable
    Protocol(const Protocol&) = delete;
    Protocol& operator=(const Protocol&) = delete;

    // --- Async lifecycle (open/close on a background worker) ---
    //
    // The only public entry points for driving a protocol. Callers such
    // as the device-monitor callback must not block on open/close (which
    // may run a full handshake and join worker threads), so all lifecycle
    // work is funneled through a serialized worker.

    /// Set desired state to OPEN. Idempotent (equivalent stores collapse).
    void request_open();

    /// Set desired state to CLOSED and cancel an ongoing open. Idempotent.
    void request_close();

    /// Wait until the port is open and not being cancelled, or timeout.
    /// Protocol handshaking may still be in progress.
    bool wait_ready(std::chrono::milliseconds timeout);

    /// Wait until the port is closed and any opening/closing work has finished.
    bool wait_closed(std::chrono::milliseconds timeout);

    /// Port state, forwarded from the owned transport. Safe before open()
    /// (a null transport reads as closed).
    bool is_open() const;
    bool is_connected() const;
    bool is_disconnected() const;

    /// Human-readable description of the underlying transport.
    std::string description() const;

    // --- Port-trace statistics ---
    //
    // ptrace_ is protected, so a composing owner (as opposed to a subclass)
    // cannot read these directly. Forwarded here for the summary an owner
    // prints after close.

    /// Records written to the port trace so far. Valid after close too.
    uint64_t ptrace_record_count() const { return ptrace_.record_count(); }

    /// Bytes written to the port trace so far. Valid after close too.
    uint64_t ptrace_bytes_written() const { return ptrace_.bytes_written(); }

    /// Path of the port-trace file, or empty if tracing is disabled.
    const std::string& ptrace_path() const { return ptrace_.path(); }

    // --- Wire I/O statistics ---
    //
    // Counted in tx_loop/rx_loop, so these are RAW WIRE bytes: TX includes
    // the framing a subclass added, and RX is pre-decode read() output, not
    // decoded frames. An owner that needs frame-level counts must tally
    // those itself where it sees the frames.

    uint64_t tx_packets() const { return tx_packets_.load(std::memory_order_relaxed); }
    uint64_t tx_bytes()   const { return tx_bytes_.load(std::memory_order_relaxed); }
    uint64_t rx_reads()   const { return rx_reads_.load(std::memory_order_relaxed); }
    uint64_t rx_bytes()   const { return rx_bytes_.load(std::memory_order_relaxed); }

    // --- Protocol state ---

    /// Current protocol-level state.
    ProtocolState state() const { return protocol_state_.load(); }

    /// Whether the protocol is ready to carry commands.
    bool is_available() const { return protocol_state_.load() == STATE_AVAILABLE; }

    /// Human-readable name for a ProtocolState.
    static std::string state_to_string(ProtocolState state);

    /// Opaque handle identifying this protocol instance, stable for its
    /// lifetime. Passed to ProtocolStateCallback (see
    /// TransportManager::set_protocol_state_listener) so a listener watching
    /// several protocols can tell them apart.
    int64_t handle() const { return reinterpret_cast<int64_t>(this); }

    /// Send an already-formed protocol payload and wait for its paired
    /// response. timeout_ms=0 uses the protocol's configured default.
    /// Returns an empty buffer on timeout / transport-not-running.
    /// Subclasses apply their own framing and response matching.
    virtual ByteBuffer send_and_wait(const ByteBuffer& payload,
                                     uint32_t timeout_ms = 0) = 0;

protected:
    /// The driven transport. Non-null for the lifetime of the Protocol.
    std::unique_ptr<Transport> transport_;

    /// Protocol-level state. Driven by the base lifecycle:
    ///   STATE_DISCONNECTED -> STATE_INITIALIZING on lifecycle_open() entry
    ///   STATE_INITIALIZING -> STATE_AVAILABLE    once open + handshake pass
    ///   any                -> STATE_DISCONNECTED on close or failed open
    /// Subclasses set STATE_UNRESPONSIVE when the link is up but the peer
    /// has stopped answering.
    std::atomic<ProtocolState> protocol_state_{STATE_DISCONNECTED};

    /// Transition to a new protocol state, logging the change and notifying
    /// TransportManager's protocol-state listeners.
    void set_state(ProtocolState state);

    /// Enable port-trace recording. Trace file is opened by
    /// lifecycle_open() and closed by lifecycle_close(). Path and metadata
    /// must be set before the protocol is opened.
    void set_port_trace(bool enabled, const std::string& path) {
        port_trace_enabled_ = enabled;
        port_trace_path_ = path;
    }

    /// Set metadata written into the ptrace header. Optional; call before
    /// request_open() if used.
    void set_port_trace_metadata(const PtraceMetadata& meta) {
        port_trace_meta_ = meta;
    }

    /// Enable verbose per-packet I/O logging on the transport.
    void set_verbose(bool v);

    static constexpr size_t   IO_RX_BUFFER_SIZE     = 128 * 1024;
    static constexpr uint32_t IO_RX_READ_TIMEOUT_MS = 100;

    // Port trace (opened/closed automatically by lifecycle_open/close;
    // written from tx_loop/rx_loop when enabled).
    bool port_trace_enabled_ = false;
    std::string port_trace_path_;
    PtraceMetadata port_trace_meta_;
    PtraceWriter ptrace_;

    /// Set while the TX and RX threads should keep running. Derived classes
    /// may read this to abort their own waits during shutdown.
    std::atomic<bool> io_running_{false};

    // Wire I/O counters, bumped by tx_loop/rx_loop and exposed by the public
    // getters above. Relaxed ordering: these are monotonic statistics, never
    // used to synchronize.
    std::atomic<uint64_t> tx_packets_{0};
    std::atomic<uint64_t> tx_bytes_{0};
    std::atomic<uint64_t> rx_reads_{0};
    std::atomic<uint64_t> rx_bytes_{0};

    /// Launch the TX and RX threads. Call once the port is open.
    void start_io_threads();

    /// Signal the TX and RX threads and join them. Safe if never started,
    /// and safe to call more than once. The threads only call the
    /// transport's read()/write(), so a derived class need not join them
    /// before its own members die — but doing so in its destructor is still
    /// the clearest ordering.
    void stop_io_threads();

    /// Queue raw bytes for the TX thread to write. Returns false if the
    /// TX thread is not running.
    bool submit_tx(ByteBuffer data);

    /// Take the next chunk of raw received bytes, waiting up to timeout_ms.
    /// Returns an empty buffer on timeout or shutdown. Consumers call this
    /// from their own loop and do the framing there.
    ByteBuffer next_rx(uint32_t timeout_ms);

    /// High-level lifecycle hooks invoked by the async worker. Defaults
    /// open the port trace file (if enabled), open the port and start the
    /// TX/RX + receiver/dispatcher threads. On close, stop the threads
    /// then close the port, then close the trace file (so any last records
    /// land before the file goes away). Subclasses override to add
    /// handshake, framing setup, or extra teardown around this sequence.
    virtual bool lifecycle_open();
    virtual void lifecycle_close();

    /// Cancellation stays set until the worker begins a fresh opening attempt.
    bool open_cancelled() const { return open_cancel_requested_.load(); }

    /// Interrupt a handshake delay when request_close() arrives.
    bool wait_for_open_cancel(std::chrono::milliseconds timeout);

    /// Optional consumer loops started alongside the TX/RX pumps. The
    /// receiver_loop() typically pulls raw bytes with next_rx() and does
    /// framing; the dispatcher_loop() typically routes decoded frames.
    /// Base defaults are no-ops so a protocol that only exposes raw bytes
    /// doesn't pay for either thread. Subclasses override to implement
    /// framing/dispatch, and the base machinery drives their lifetime
    /// alongside io_running_.
    virtual void receiver_loop()   {}
    virtual void dispatcher_loop() {}

    /// Stop the lifecycle worker if running. Call from a subclass
    /// destructor BEFORE its own members die, since the worker may still
    /// be executing lifecycle_open()/lifecycle_close() on `this`.
    void stop_lifecycle_worker();

    /// Full shutdown sequence for a subclass destructor:
    ///   1. request_close()          - ask the lifecycle worker to close
    ///   2. wait_closed(timeout)     - wait for lifecycle_close to finish,
    ///                                 which runs stop_io_threads() and
    ///                                 the transport's close() under the
    ///                                 still-valid vtable
    ///   3. stop_lifecycle_worker()  - join the lifecycle worker itself
    /// Cannot live in ~Protocol() because by then the derived vtable is
    /// gone and the virtual lifecycle_close() call would resolve to the
    /// base. Subclasses call this from their destructor while their vtable
    /// is still intact.
    void stop(std::chrono::milliseconds close_timeout = std::chrono::seconds(5));

    // --- Single-outstanding request/response slot ---
    //
    // Framing-agnostic mechanics for a "one request, one paired response"
    // pattern. The receiver_loop (in a subclass or the base default) is
    // responsible for calling try_fulfill_pending() when a decoded frame
    // matches the pending signature. Subclasses supply their own signature
    // comparison because it depends on the protocol.
    struct PendingCommand {
        ByteBuffer signature;
        std::promise<ByteBuffer>* promise = nullptr;
    };
    PendingCommand pending_;
    std::mutex pending_mutex_;

    // Serializes send-and-wait: only one request may be outstanding
    // against the single pending_ slot at a time.
    std::mutex send_mutex_;

    /// Signature for a paired response. Opaque to the base — the subclass
    /// stores whatever it needs to recognize the response later. The
    /// matcher passed to send_and_wait_frame() gets it back so it can
    /// decide whether an inbound frame matches.
    using MatchFn = std::function<bool(const ByteBuffer& signature,
                                       const ByteBuffer& frame)>;

    /// Send an already-framed request and wait for a paired response.
    /// Serialized by send_mutex_ (single outstanding request). The caller
    /// supplies the response signature and the matcher; the base handles
    /// the pending slot, TX submit, timeout, and cleanup on shutdown.
    /// Returns the matched response, or an empty buffer on timeout /
    /// transport-not-running.
    ByteBuffer send_and_wait_frame(ByteBuffer frame,
                                   ByteBuffer signature,
                                   MatchFn matcher,
                                   std::chrono::milliseconds timeout);

    /// Called from the receiver path with each decoded frame. If it
    /// matches the pending signature (via `matcher`), the promise is
    /// fulfilled and this returns true. Otherwise returns false so the
    /// caller can treat it as an async packet.
    bool try_fulfill_pending(const ByteBuffer& frame, const MatchFn& matcher);

private:
    std::thread tx_thread_;
    std::thread rx_thread_;
    std::thread receiver_thread_;
    std::thread dispatcher_thread_;

    std::queue<ByteBuffer> tx_queue_;
    std::mutex tx_mutex_;
    std::condition_variable tx_cv_;

    // Inbound raw bytes: rx_loop (producer) -> next_rx() (consumer).
    std::queue<ByteBuffer> rx_queue_;
    std::mutex rx_mutex_;
    std::condition_variable rx_cv_;

    // Raw write pump: drains tx_queue_ onto the wire.
    void tx_loop();

    // Raw read pump: fills rx_queue_ from the wire.
    void rx_loop();

    // --- Lifecycle worker ---
    std::thread lifecycle_thread_;
    std::mutex lifecycle_mutex_;
    std::condition_variable lifecycle_cv_;
    std::condition_variable ready_cv_;
    std::atomic<bool> lifecycle_running_{false};

    // Desired state driven by request_open()/request_close(). The worker
    // reconciles via lifecycle_open()/lifecycle_close(). Duplicate requests
    // collapse, but a cancelled attempt must close before any later reopen.
    std::atomic<bool> desired_open_{false};

    std::atomic<bool> open_cancel_requested_{false};
    // Guarded by lifecycle_mutex_; includes cleanup of a cancelled open.
    bool lifecycle_busy_ = false;

    void ensure_lifecycle_worker();
    void lifecycle_loop();
};

} // namespace QC

#endif // QC_PROTOCOL_H
