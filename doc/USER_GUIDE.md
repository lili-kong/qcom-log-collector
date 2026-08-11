# qlc (qualcomm-log-collector) — User Guide

`qlc` is an open-source command-line tool for capturing logs
from Qualcomm devices over the **Diag** protocol. It connects to a device's diag
port, configures which logs/events/messages the device should emit (from a `.cfg`
file), and streams the resulting diagnostic packets to a `.diag` capture file
for later analysis.

- **License:** BSD 3-Clause Clear License
- **Platforms:** Windows 10/11 (x64) and Linux (x64)

---

## Table of Contents

1. [What it does](#what-it-does)
2. [Requirements](#requirements)
3. [Building from source](#building-from-source)
4. [Finding your device](#finding-your-device)
5. [Quick start](#quick-start)
6. [Command-line options](#command-line-options)
7. [CFG configuration files](#cfg-configuration-files)
8. [Output files](#output-files)
9. [ADPL Logging](#adpl-logging)
10. [How it works](#how-it-works)
11. [Stopping a capture](#stopping-a-capture)
12. [Troubleshooting](#troubleshooting)
13. [Getting more detail (verbose mode)](#getting-more-detail-verbose-mode)

---

## What it does

- **Connects** to a Qualcomm diag port (USB) and verifies the link with a
  loopback handshake.
- **Negotiates framing** — switches the link to the more efficient Non-HDLC
  framing when the device supports it (can be forced to classic HDLC).
- **Configures logging** by sending the diag mask commands contained in your
  `.cfg`/`.cfg2` file (which logs, events, and debug messages to emit).
- **Captures** the asynchronous diagnostic stream (logs, events, F3 messages,
  QTrace, QMetrics, …) and writes it to a `.diag` file.
- **Stays alive** by sending a periodic keep-alive when the link is idle.
- **Recovers automatically** if the device reboots or briefly disconnects
  (enabled by default).

---

## Requirements

**To run the tool**
- A Qualcomm device exposing a diag port over USB, with the appropriate USB
  drivers installed (see [USB drivers](#usb-drivers)).
- The diag port must be **free** — no other diag tool may hold it open at
  the same time.
- A `.cfg` (or `.cfg2`) mask-configuration file describing what to log.

**To build from source**
- **Windows:** Visual Studio 2019 (toolset v142) and CMake ≥ 3.14.
- **Linux:** a C++17 compiler (GCC/Clang), CMake ≥ 3.14, and `libudev`
  development headers.

---

## USB drivers

Installing Qualcomm USB drivers is optional if the Qualcomm kernel drivers 
are already present on your system. If drivers have not been installed,
it is recommended to install the open-source userspace drivers. Driver
installation is a one-time process.

Two open-source driver options are available:

| Driver | Repository | Notes |
|--------|-----------|-------|
| **Userspace driver** | [qcom-usb-userspace-drivers](https://github.com/qualcomm/qcom-usb-userspace-drivers/releases) | Lightweight, libusb-based. Has a known compatibility issue with ADB (see [Known limitations](#known-limitations)). |
| **Kernel driver** | [qcom-usb-kernel-drivers](https://github.com/qualcomm/qcom-usb-kernel-drivers/releases) | Recommended if you also need ADB running alongside log collection. |

To install either driver, find the release matching your platform, expand the
**Assets** section, and either run the installer executable directly or
download the signed zip and run the executable inside it.

Choose based on your ADB use case — if you need ADB concurrently, use the kernel driver.
If ADB is not needed, either driver works.

---

## Building from source

The project uses CMake.

### Windows

```bash
cmake -B out/build/x64-Debug -G "Visual Studio 16 2019" -A x64
cmake --build out/build/x64-Debug --config Debug
```

The executable is produced at `Build/Windows/x64/bin/Debug/qlc.exe`.

For an optimized build, configure a Release directory and build `--config Release`.

### Linux

```bash
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build
```

The executable is produced at `Build/Linux/x86_64/bin/qlc`.

### Running the unit tests (optional)

```bash
ctest --test-dir out/build/x64-Debug -C Debug --output-on-failure
```

---

## Finding your device

List the connected Qualcomm devices and their diagnostic interfaces:

```bash
qlc --devices
```

Example output:

```
Found 1 device(s):

  Device [0] Qualcomm USB Composite Device 90DB (0003)
    ADB Serial Number: 12345678
    MSM Serial Number: ABCD1234
    Available Protocols:
      [0.0] Qualcomm DPL Data 90DB (25) [ADPL]
      [0.1] QDSS Data 90DB (25) [QDSS]
      [0.2] Qualcomm HS-USB Diagnostics 90DB (25) [DIAG]

Use --collect-log --device="<device description>" to connect.
```

Devices are shown as a 2-level hierarchy: composite devices with their child
interfaces grouped underneath. Only interfaces relevant to log collection are
shown: DIAG, QDSS, and ADPL.

Pass the **device description** (or a unique substring) to `--device` when
capturing — the tool will automatically find and connect to the DIAG interface
within that device.

---

## Quick start

Capture for 60 seconds using a mask file, writing the `.diag` to the default
location:

```bash
qlc --collect-log --cfg=masks.cfg --device="Qualcomm USB Composite Device 90DB (0003)" --duration=60
```

The `--device` value must exactly match a device description shown by
`--devices`. The tool resolves the device to its DIAG interface automatically.

A typical run prints:

```
Resolved device "Qualcomm USB Composite Device 90DB (0003)" to DIAG interface: Qualcomm HS-USB Diagnostics 90DB (25)
Parsing CFG file: masks.cfg
Opening Qualcomm HS-USB Diagnostics 90DB (25)
Writing to: C:\ProgramData\QFS\DiagLogger\PTraceLogs
Sending loopback...
Device connected.
Running device handshake...
Sending CFG commands...
Logging started. Press Ctrl+C to stop.
```

When the duration elapses (or you press Ctrl+C) a summary is printed with the
packet/byte counts and the path to the capture file.

---

## Command-line options

The tool uses a two-level command system. The first argument is always a
**command** (`--devices` or `--collect-log`), followed by **options** in
`--option=value` format.

### Commands

| Command | Description |
|---------|-------------|
| `--devices` | List connected diag devices and exit. Takes no options. |
| `--collect-log` | Collect diagnostic logs from a device. See options below. |
| `--help`, `-h` | Show usage and exit. |
| `--version`, `-V` | Print the tool version and exit. |

### Options for `--collect-log`

`--cfg` and `--device` are always required, and **one** duration option is
required.

| Option | Default | Description |
|--------|---------|-------------|
| `--cfg=<file>` | — | **Required.** Path to the `.cfg`/`.cfg2` mask-configuration file. |
| `--device=<device>` | — | **Required.** Exact device description from `--devices`. The DIAG interface is auto-selected. |
| `--duration=<seconds>` | — | **Required (one of three).** Auto-stop after N seconds. |
| `--duration-minutes=<N>` | — | **Required (one of three).** Auto-stop after N minutes. |
| `--duration-hours=<N>` | — | **Required (one of three).** Auto-stop after N hours. |
| `--out=<directory>` | platform default (see below) | Directory where the `.diag` capture is written. |
| `--baud=<rate>` | `115200` | Serial baud rate. |
| `--verbose` | off | Print decoded command IDs and per-packet I/O to stderr. |
| `--hdlc` | off | Force classic HDLC framing (skip the Non-HDLC switch). |
| `--enable-qdss` | off | Also capture QDSS high-speed trace to a `.bin` file. The QDSS interface is auto-selected from the same device. |
| `--enable-adpl` | off | Also capture ADPL data-path logs to a `.adplvX` file (see [ADPL Logging](#adpl-logging)). |
| `--no-recovery` | recovery on | Disable automatic reboot/disconnect recovery. |
| `--recovery-timeout=<seconds>` | `120` | Max time to wait for the device to come back during recovery. |

> **Note:** Provide exactly one duration option; for an effectively "unlimited"
> run, pass a large value (e.g. `--duration-hours=24`) and stop manually with
> Ctrl+C.

> **Note:** `--enable-qdss` requires the device to expose a QDSS interface; run
> `--devices` to confirm one is listed. The QDSS capture is written alongside the
> `.diag` file in the `--out` directory as
> `qlc_qdss_<timestamp>__PID_<pid>.bin`.

---

## CFG configuration files

A CFG file is a stream of pre-built Diag **mask-configuration commands** that
tell the device which logs, events, and debug (F3) messages to emit. The tool
parses the file into individual commands and sends them to the device right
after connecting.

Two formats are supported, selected by file extension:

| Extension | Framing | Notes |
|-----------|---------|-------|
| `.cfg` | HDLC-framed | Classic diag framing. |
| `.cfg2` | Non-HDLC-framed | Length-prefixed framing. |

CFG files are normally produced by Qualcomm mask-configuration tooling (e.g.
QXDM mask exports). A small example used by the test suite lives at
`tests/test_data/sample.cfg`.

If the CFG file cannot be parsed (no valid frames found), the tool reports an
error and exits before opening the device.

---

## Output files

### Capture (`.diag`)

The captured diagnostic stream is written to a `.diag` file. By default it is
placed in a platform directory; use `--out=<directory>` to choose your own.

| Platform | Default capture directory |
|----------|---------------------------|
| Windows | `C:\ProgramData\QFS\QLC\PTraceLogs\` |
| Linux | `/var/tmp/QFS/QLC/PTraceLogs/` |

The `.diag` file records the raw, on-the-wire bytes for both transmitted and
received frames, so it can be decoded later by compatible Qualcomm log-analysis
tools.

### Metadata (`.qlc`)

Alongside the `.diag` file, the tool writes a `.qlc` binary metadata file
containing device and session information gathered during the handshake. This
file is used by downstream log-analysis tools to correctly decode the capture.

The `.qlc` file includes:

- **Version** — metadata format version.
- **HDLC data type** — whether the capture uses HDLC or Non-HDLC framing.
- **QShrink GUIDs** — hash file GUIDs for QShrink4 message decoding.
- **DiagID table** — processor-to-DiagID mappings for multi-processor targets.
- **Security keys** — encryption key info (if the device uses secured logging).
- **QDSS HW acceleration** — ATID entries for hardware-accelerated trace.
- **Device info** — version number, extended build ID, ESN, and status responses.

The `.qlc` file is written to the same directory as the `.diag` capture file.

### QShrink4 hash file (`.qsr4`)

During the session the tool downloads the device's QShrink4 hash database
(`.qdb`) over Diag on a background thread, validates it, and decompresses it to
`msg_hash_<GUID>.qsr4` in the same directory as the capture. Log-analysis tools
use this file to expand QShrink-compressed F3 debug messages into readable text,
so it no longer has to be sourced separately for the build under test.

If a matching `msg_hash_<GUID>.qsr4` already exists in the output directory, the
download is skipped.

### QDSS capture (`.bin`)

When `--enable-qdss` is passed, the tool opens the device's QDSS interface in
parallel with the DIAG interface and streams its raw high-speed trace data to a
`.bin` file named `qlc_qdss_<timestamp>__PID_<pid>.bin`.

This file is written to the same directory as the `.diag` capture. Its contents
are raw binary bytes straight off the wire — no HDLC framing is applied — and are
intended for decoding by Qualcomm trace-analysis tooling.

### Debug logs

The tool also writes its own diagnostic/debug log (via the bundled kLogger) to:

| Platform | Default debug-log directory |
|----------|-----------------------------|
| Windows | `C:\ProgramData\QFS\QLC\DebugLogs\` |
| Linux | `/var/tmp/QFS/QLC/DebugLogs/` |

Log files roll over at 100 MB.

---

## ADPL Logging

ADPL (Accelerated Data Path Logging) captures the modem's data-path packet log,
which is delivered over the IPA subsystem. When enabled, the tool negotiates ADPL
with the device, receives the ADPL frames on the existing diag connection, and
writes them to a separate binary file alongside the usual `.diag` capture.

### Enabling ADPL

Add the `--enable-adpl` flag to a `--collect-log` command:

```
qlc --collect-log --cfg=mask.cfg --device="<device description>" --duration=60 --enable-adpl
```

Requirements:

- The target must be an **ADPL-capable device** that exposes an IPA subsystem on
  a diag port. Devices without ADPL support will reject the enable command and no
  ADPL file is produced (the normal `.diag` capture continues unaffected).
- ADPL uses the same diag connection as the main capture — no extra port or cable
  is needed.

### Version negotiation

The tool requests the highest supported ADPL protocol version (**v7**) via the
IPA `ACCELERATED_IPA_REQUEST` (`0x4000`) subsystem command. The device replies
with the version it will actually use (1–7) and a status code. The **negotiated**
version — not the requested one — determines how incoming frames are parsed and
which file extension is used.

### Output file (`.adplvX`)

When ADPL is enabled, the tool writes a binary file named `adpl_capture` 
with a version-specific extension in the capture output directory
(the same directory as the `.diag` file; override with `--out=<directory>`):

| Negotiated version | File name |
|--------------------|-----------|
| v1 | `adpl_capture.adpl` |
| v2 | `adpl_capture.adplv2` |
| v3 | `adpl_capture.adplv3` |
| v4 | `adpl_capture.adplv4` |
| v5 | `adpl_capture.adplv5` |
| v6 | `adpl_capture.adplv6` |
| v7 | `adpl_capture.adplv7` |


### Notes

- A summary of the ADPL file path, frame count, and bytes written is printed at the
  end of the session and in verbose output.

---

## How it works

Understanding the sequence helps when reading verbose output or diagnosing a
failed capture.

1. **Parse CFG** — the mask file is read and decoded into individual commands.
2. **Open the port** — the diag device is opened (with retries).
3. **Loopback handshake** — a loopback command confirms the device is a
   responsive diag port. If there's no response, the capture aborts.
4. **Device handshake** — status/build-id queries, then (unless `--hdlc`) a
   switch to Non-HDLC framing, plus capability negotiation.
5. **Send masks** — the CFG commands are sent to enable the requested logging.
6. **Capture loop** — asynchronous diagnostic packets stream in and are written
   to the `.diag` file. Synchronous command responses are paired to their
   requests internally.
7. **Keep-alive** — when the receive side is idle, a periodic STATUS poll keeps
   the link healthy.
8. **Recovery** *(default on)* — if the device disconnects or reboots, the tool
   waits (up to `--recovery-timeout`) for it to return, then re-runs the
   handshake and re-applies the masks automatically. Disable with
   `--no-recovery`.

---

## Stopping a capture

A capture ends when **either**:

- the configured duration elapses, or
- you press **Ctrl+C**.

On stop, the tool sends a graceful disconnect sequence (disabling the masks it
enabled), closes the capture file, and prints a summary:

```
--- Logging Summary ---
RX packets:  12345
RX bytes:    6789012
TX packets:  42
TX bytes:    310
Frame errors: 0
Ptrace file: C:\ProgramData\QFS\DiagLogger\PTraceLogs\...
Records:     12387
File size:   6789322 bytes
```

---

## Known limitations

- **ADB conflict (userspace driver only):** The userspace driver has a known
  compatibility issue with ADB. If you are using the userspace driver, kill
  the ADB server before running the tool:
  ```bash
  adb kill-server
  ```
  If you need ADB running concurrently with log collection, use the kernel
  driver instead (see [USB drivers](#usb-drivers)).

---

## Troubleshooting

**"Device did not respond to loopback."**
The tool connected to the port but got no diag response. Common causes:
- Another application has the COM/diag port open and is
  consuming responses. Close all other diag tools and retry.
- The chosen `--device` is not actually a Qualcomm diag port.
- The device is in a mode that doesn't answer diag commands.

**"Cannot open serial port" / device won't open.**
- Verify the device with `--devices` and copy the exact description.
- Make sure the USB drivers are installed and the device is enumerated.
- On Windows, run from a terminal with sufficient privileges; on Linux ensure
  your user can access the USB device (udev permissions).

**"No QcDevice devices found."**
- The device isn't enumerated as a diag port. Re-plug it, confirm drivers, and
  check it's in a diag-capable USB composition.

**"Error parsing CFG file" / "contains no commands".**
- The file isn't a valid `.cfg`/`.cfg2` command stream, or the extension
  doesn't match its framing. Regenerate the mask file or check the extension.

**Capture file is empty or tiny.**
- The masks may not enable any active logging for what the device is doing,
  or the device went idle. Confirm the CFG enables the logs you expect.

**"Unknown option" errors.**
- Options must use `--option=value` format (with `=` sign), not `--option value`.
- Flags (`--verbose`, `--hdlc`, `--no-recovery`, `--enable-qdss`) do not accept a
  value.
- The QDSS flag is `--enable-qdss`; earlier builds used `--qdss`, which is no
  longer accepted.

---

## Getting more detail (verbose mode)

Add `--verbose` to print decoded command IDs, request/response pairing, and
per-packet TX/RX byte dumps to stderr. This is the first thing to enable when a
capture isn't behaving as expected — it shows the handshake step-by-step and
whether packets are flowing.

```bash
qlc --collect-log --cfg=masks.cfg --device="Qualcomm USB Composite Device 90DB (0003)" --duration=30 --verbose
```

Without `--verbose`, routine per-packet I/O is suppressed and only high-level
progress, warnings, and errors are shown.
