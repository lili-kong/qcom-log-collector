# qlc Release Notes

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
- **Cross-platform** — supports Windows 10/11 (x64) and Linux (x64)
- **Duration control** — auto-stop after seconds, minutes, or hours; or stop
  manually with Ctrl+C

### Known Limitations

- QDSS, ADPL, and `--qdss-processors` options are disabled (planned for a future
  release)
- **ADB compatibility:**
  - **Userspace driver** — there is a known compatibility issue with ADB. Users
    must kill the ADB server (`adb kill-server`) before running `qlc`.
  - **QUD driver** — recommended if you also need ADB running alongside log
    collection. No known ADB compatibility issues.
