# qlc Release Notes

## v1.2.1

> **Note:** This version requires user space driver 1.0.2.2 (Windows) or
> 1.0.1.8 (Linux).

### Overview

This release adds ADPL logging and QShrink4 hash-file download, extends automatic
recovery to QDSS, and changes the capture file extension from `.ptrace` to
`.diag`.

### New Features

- **ADPL logging** — capture the modem's Accelerated Data Path Log with
  `--enable-adpl`. The tool negotiates the ADPL protocol version with the device
  (v1–v7) and writes raw wire bytes to a version-specific file
  (`.adpl`, `.adplv2` … `.adplv7`) in the capture output directory. DPL log codes
  `0x11EB` (LOG_DATA_PROTOCOL_LOGGING_C) and `0x1951` (LOG_DATA_ADPL_SIO_CONFIG,
  needed for ADPLv2+ de-mux) are merged into the equip-ID-1 log mask
  automatically.
- **QShrink4 hash-file download** — the tool now downloads the device's `.qdb`
  QShrink4 hash file over Diag on a background thread, validates it, and
  decompresses it to `msg_hash_<GUID>.qsr4` next to the capture, so message
  decoding no longer requires sourcing the hash file separately. Includes a
  vendored, self-contained zlib inflater.
- **Device recovery for QDSS** — automatic reboot/disconnect recovery now covers
  the QDSS interface in addition to the DIAG interface, resuming capture into the
  same output files after the device comes back.
- **Logging summary** — on stop, the tool prints a summary with RX/TX packet and
  byte counts, frame errors, capture file path, record count, file size, and (when
  ADPL is enabled) the ADPL file path and bytes captured.
- **Composite device grouping** — `--devices` now presents composite devices as a
  two-level device/protocol hierarchy and reports device serial numbers.

### Changes

- **Capture file extension is now `.diag`** (was `.ptrace`). Capture files are
  named `qlc_<timestamp>_<...>_PID_<pid>_<index>.diag` (QUTS-22534).
- **`--qdss` renamed to `--enable-qdss`** for consistency with `--enable-adpl`.
- QDSS `.bin` output file naming and default location corrected.
- QDSS port/interface detection and port-open handling improved.
- Debug logs continue to roll over at 100 MB.

### Fixes

- Fixed stop-logging shutdown path so commands issued during shutdown (for
  example, from the QShrink4 download thread) can no longer hang on join.
- Suppressed the spurious `ReadFromDevice` error reported after a graceful
  shutdown.
- Fixed the Linux build failure and corrected the affected documentation.
- Fixed Visual Studio project paths after the `file_writer` reorganization and
  suppressed a libusb conformance error.

### Internal

- Namespace renamed to `QC`; include guards renamed to the `QC_` prefix.
- `file_writer` and `cli` folders reorganized; `binary_writer` added as the shared
  base for QDSS and ADPL output.
- Coding standard documented in `doc/coding-standard.md` and enforced across the
  source tree (include ordering, naming, license headers).
- Removed remaining references to TUF and unused documentation.

### Known Limitations

- The `--qdss-processors` option is still disabled (planned for a future
  release).
- **ADB compatibility:**
  - **Userspace driver** — there is a known compatibility issue with ADB. Users
    must kill the ADB server (`adb kill-server`) before running `qlc`.
  - **Kernel driver** — recommended if you also need ADB running alongside log
    collection. No known ADB compatibility issues.

---

## v1.1.1 — Initial Release

> **Note:** Since this version, CLI only works with user space driver 1.0.2.2 (Windows) or 1.0.1.8 (Linux).

### Overview

`qlc` (qualcomm-log-collector) is an open-source command-line tool for capturing
diagnostic logs from Qualcomm devices over the Diag protocol.

### Major Features

- **Device discovery** — enumerate connected Qualcomm diag devices with
  `qlc --devices`
- **Diagnostic log capture** — connect to a device, configure logging masks from
  a `.cfg`/`.cfg2` file, and stream packets to a `.ptrace` capture file
- **Non-HDLC framing** — automatically negotiates efficient Non-HDLC framing
  when supported by the device (can be forced to classic HDLC with `--hdlc`)
- **Automatic recovery** — reconnects and re-applies masks if the device reboots
  or disconnects during capture (configurable timeout, disable with `--no-recovery`)
- **QLC metadata** — writes a `.qlc` binary metadata file alongside the capture
  containing device info, DiagID table, QShrink GUIDs, and QDSS HW acceleration
  data for downstream decoding
- **QDSS capture** — optionally stream the device's QDSS high-speed trace to a
  `.bin` file alongside the capture with `--enable-qdss` (the QDSS interface is
  auto-selected from the same device)
- **Cross-platform** — supports Windows 10/11 (x64) and Linux (x64)
- **Duration control** — auto-stop after seconds, minutes, or hours; or stop
  manually with Ctrl+C

### Known Limitations

- ADPL and `--qdss-processors` options are disabled (planned for a future
  release)
- **ADB compatibility:**
  - **Userspace driver** — there is a known compatibility issue with ADB. Users
    must kill the ADB server (`adb kill-server`) before running `qlc`.
  - **Kernel driver** — recommended if you also need ADB running alongside log
    collection. No known ADB compatibility issues.
