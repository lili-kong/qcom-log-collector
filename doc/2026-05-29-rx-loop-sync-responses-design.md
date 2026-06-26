# Unified RX Dispatch with Request/Response Pairing

**Date:** 2026-05-29
**Component:** `src/logger/diag_logger.{h,cpp}`
**Status:** Approved — ready for implementation planning

## Problem

The diag logger runs two threads that both read from the same `transport_`:

- **`tx_loop`** pops a `TxRequest` from `tx_queue_`, encodes it, writes it to the
  transport, **then reads and decodes the response itself** (diag_logger.cpp:273–315)
  and fulfills the request's `std::promise`. Synchronous request/response handling
  lives entirely inside `tx_loop`.
- **`rx_loop`** *also* reads from `transport_`, decodes frames, and only counts
  the asynchronous packets (log / event / message / qtrace / qmetric) — it does not
  route them anywhere.

Because both threads read the same transport concurrently (rx_thread is launched at
`start()`, before the handshake even runs), there is a race: a synchronous response
can be consumed by `rx_loop`, and an asynchronous log packet can be consumed by
`tx_loop`. The `tx_response_pending_` flag is only a partial guard used by the
keep-alive poll path.

## Goal

Make `rx_loop` the single reader of the transport. Every frame that arrives is
placed on a response queue first. A dispatcher then classifies each frame and
either pairs it to the outstanding request (synchronous diag, subsystem, and
subsystem V2 responses) or routes it as an asynchronous packet. This brings
synchronous responses under the same receive path as asynchronous packets and
adds an explicit request/response pairing mechanism.

## Design Decisions

These were settled during brainstorming:

1. **Classification** — by **request-signature match**. The dispatcher pairs the
   first frame matching the outstanding request's signature; everything else is
   asynchronous. (Rejected: next-frame heuristic, async-allowlist classify — both
   less precise when async and sync share a command byte, e.g. `0x60`.)
2. **Outstanding model** — **single outstanding request + promise**. Matches diag's
   real one-at-a-time behavior. (Rejected: a multi-entry pending map — unnecessary
   complexity and signature collisions.)
3. **Queue consumer** — a **separate dispatcher thread**. `rx_loop` stays fast and
   never blocks on processing. (Rejected: draining the queue inline in `rx_loop`.)
4. **Polling** — **folded into the pairing path** (changed from an earlier decision
   to keep it separate). One response-wait code path for both queued commands and
   the keep-alive poll.

## Thread Model & Ownership

Three threads, with clear single-owner roles:

| Thread             | Role          | Responsibilities |
|--------------------|---------------|------------------|
| `tx_loop`          | sole *writer* | Pop `TxRequest` (or build an internal STATUS poll), encode, write to transport, register the single outstanding `PendingCommand`, wait for resolution/timeout. **No longer reads the transport.** |
| `rx_loop`          | sole *reader* | Read raw bytes → write raw to ptrace (unchanged) → decode frames → push each decoded frame onto `rx_queue_`. Retains duration-limit, idle poll-request detection, and disconnect/recovery logic. |
| `dispatcher_loop`  | queue consumer| Pop `rx_queue_`, classify each frame, pair to the pending request or route as async. |

Writes remain serialized on `tx_loop`; reads remain serialized on `rx_loop`. No
transport contention.

## The Response Queue

```cpp
std::queue<ByteBuffer> rx_queue_;          // decoded frames
std::mutex             rx_queue_mutex_;
std::condition_variable rx_queue_cv_;
```

`rx_loop` is the only producer; `dispatcher_loop` is the only consumer. This is the
"all frames go into the queue first" handoff. Frames are stored **decoded** (command
byte first), matching what `send_command()` returns today.

## Request/Response Pairing

A single outstanding-request mechanism serves **both** queued commands and the
keep-alive poll.

### Pending registration

```cpp
struct PendingCommand {
    ByteBuffer            signature;   // bytes the response must match
    std::promise<ByteBuffer>* promise; // fulfilled by the dispatcher
};
// guarded by pending_mutex_; at most one entry at a time
```

### Signature derivation (from the request payload)

- `payload[0] == 0x4B` (subsystem) **or** `0x80` (subsystem V2)
  → signature = first **4 bytes** (`cmd, subsys, subcmd_lo, subcmd_hi`)
- otherwise (plain diag response)
  → signature = first **1 byte** (the echoed command code)

A frame matches if its leading bytes equal the signature.

### `tx_loop` helper

`tx_loop` gets one internal helper used for every outbound request:

```
send_and_wait(TxRequest&):
    encode (HDLC or Non-HDLC per framing_mode_)
    write frame to transport, record TX to ptrace, update tx stats
    register PendingCommand{ signature(payload), &request.promise } under pending_mutex_
    wait up to request.timeout_ms on resolve_cv_ for the dispatcher to fulfill it
    on timeout: fulfill promise with empty buffer, clear pending
```

Both dequeued `tx_queue_` requests and the internally-built STATUS poll go through
this same helper.

### Dispatcher classification

For each frame popped from `rx_queue_`:

1. **A command is pending and the frame matches its signature**
   → fulfill that promise with the frame, clear pending, notify `resolve_cv_`.
2. **Otherwise**
   → asynchronous packet (log / event / message / qtrace / qmetric)
   → update `stats_.rx_packets` / `rx_bytes`, verbose-log. (Already persisted to
     ptrace as raw bytes by `rx_loop`.)

Because only one command is outstanding at a time, signatures never collide between
commands.

## Polling Folded Into the Pairing Path

- `do_polling()`'s inline write + spin-wait is **removed**, and the
  `tx_response_pending_` flag is **deleted entirely**.
- When `rx_loop` detects idle (existing `last_rx_time_` / `POLL_IDLE_TIMEOUT_MS`
  logic) and sets `poll_requested_`, `tx_loop` builds a STATUS (`0x0C`) `TxRequest`
  internally and runs it through the **same `send_and_wait()` helper**. Its
  signature is the 1-byte `0x0C`; the dispatcher pairs the STATUS response like any
  other command. `do_polling()` becomes a thin caller that logs whether the response
  arrived.
- The single-outstanding **gate is maintained naturally**: `tx_loop` blocks inside
  `send_and_wait()` until the current request resolves before it dequeues the next
  queued command or fires another poll. No separate gate flag is needed.

`poll_requested_`, `last_rx_time_`, and the idle-detection in `rx_loop` remain as the
trigger for *when* to poll — only the *how* (the wait) is unified.

## Timeout & Shutdown

- **Timeout** — contract unchanged: `send_and_wait()` returns an empty buffer on
  timeout (for both queued commands and the poll) and clears pending.
  `send_command()`'s signature and "empty buffer = timeout/failure" contract are
  preserved, so all existing callers are unchanged.
- **Shutdown** — `stop()` signals all three threads (`running_`, `tx_cv_`,
  `rx_queue_cv_`, `resolve_cv_`); the dispatcher drains/exits; any outstanding
  pending promise is fulfilled with an empty buffer; the `tx_queue_` drain-on-exit
  logic is unchanged.

## What Does Not Change

- `send_command()` public signature and the "empty buffer = failure" contract.
- All handshake / QDSS / ADPL / disconnect callers.
- ptrace raw-byte recording (TX in `tx_loop`, RX in `rx_loop`).
- The HDLC / Non-HDLC framing decoders and `framing_mode_` switching.
- `poll_requested_` / `last_rx_time_` idle-detection that triggers a poll.

## Affected Code

- `src/logger/diag_logger.h` — add `rx_queue_` (+ mutex/cv), `PendingCommand` +
  `pending_mutex_` + `resolve_cv_`, `dispatcher_thread_`; remove
  `tx_response_pending_`; declare `dispatcher_loop()` and `send_and_wait()`.
- `src/logger/diag_logger.cpp` —
  - `tx_loop`: stop reading the transport; route every send through
    `send_and_wait()`; build the internal STATUS poll request.
  - `rx_loop`: push decoded frames onto `rx_queue_` instead of just counting them.
  - new `dispatcher_loop()`: classify + pair/route.
  - `do_polling()`: reduce to a thin caller (or remove, folding into `tx_loop`).
  - `start()` / `stop()`: launch and join `dispatcher_thread_`; updated shutdown
    signaling.

## Testing

- **Pairing correctness** — a queued command followed by interleaved async frames:
  the dispatcher pairs only the signature-matching frame; async frames go to stats.
- **Subsystem vs. V2 signatures** — verify 4-byte signature matching for `0x4B` and
  `0x80`, including a non-matching subcmd that must *not* pair.
- **Timeout** — no matching frame arrives → `send_and_wait()` returns empty within
  `timeout_ms`; pending is cleared.
- **Poll path** — idle triggers a STATUS poll through `send_and_wait()`; STATUS
  response pairs and resolves; no `tx_response_pending_` involved.
- **Shutdown with a request in flight** — pending promise is fulfilled empty and all
  three threads join.
