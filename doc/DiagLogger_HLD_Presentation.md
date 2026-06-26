# Qualcomm Log Collector — High-Level Design Presentation Outline
*Format: PowerPoint slide-by-slide outline*

---

## SLIDE 1 — Title
**Title:** Qualcomm Log Collector
**Subtitle:** Qualcomm Diagnostic Log Collector is tool for collecting diagnostic data from the Qualcomm-based devices. 
**Tags:** Open Source · C++17 · Cross-Platform
**Footer:** Qualcomm Innovation Center · Apache 2.0

---

## SLIDE 2 — Agenda
1. Project Overview & Purpose
2. Architecture Overview
3. Module Deep-Dive
4. Protocol Stack
5. Data Flow (End-to-End)
6. Threading Model
7. Technology Stack
8. Configuration & Usage
9. Build & Deployment
10. Summary

---

## SLIDE 3 — Project Overview
**What is Qualcomm Log Collector?**
- Standalone CLI tool for capturing Qualcomm Diagnostic Protocol (Diag) data from mobile devices
- Lightweight alternative to QUTS (Qualcomm Universal Test Suite)
- Raw byte capture — no HDF, no Thrift RPC, no parsed packets

**Three Logging Modes (over a single connection):**
| Mode | Description |
|------|-------------|
| Diag | Standard log / event / message packets |
| QDSS | Hardware trace via Qualcomm Debug Subsystem |
| ADPL | Advanced Data Path Logging (IPA subsystem) |

**Output:** QUTS-compatible PTRACE CSV files

---

## SLIDE 4 — Architecture Overview
*(Diagram: layered block diagram)*

```
┌───────────────────────────────────────────────┐
│               main.cpp (CLI)                  │
│   CLI parsing · signal handling · lifecycle   │
└─────────────────────┬─────────────────────────┘
                      │
┌─────────────────────▼─────────────────────────┐
│             DiagLogger (Orchestrator)         │
│  Handshake · Mask Config · RX Loop · Stats    │
└──────┬──────────┬──────────┬──────────────────┘
       │          │          │
┌──────▼───┐ ┌───▼────┐ ┌───▼──────────┐
│ Transport│ │  HDLC  │ │ Ptrace Writer│
│ (USB /   │ │Decoder │ │  (CSV output)│
│  Serial) │ │Encoder │ │              │
└──────┬───┘ └────────┘ └──────────────┘
       │
┌──────▼────────────────────────────────────────┐
│         QcDevice USB Driver (QDS)             │
│      Ring Buffer · RX Worker Thread           │
└───────────────────────────────────────────────┘
       │
┌──────▼──────────────────────────────────────┐
│         Qualcomm Device (USB / Serial)      │
└─────────────────────────────────────────────┘
```

---

## SLIDE 5 — Module Overview
| Module | Location | Lines | Responsibility |
|--------|----------|-------|----------------|
| CLI Entry Point | `src/main.cpp` | ~190 | Argument parsing, signal handling |
| DiagLogger | `src/logger/` | ~665 | Central orchestrator (state machine) |
| HDLC Framing | `src/hdlc/` | ~300 | Encode/decode with CRC-16 and byte-stuffing |
| Diag Commands | `src/diag/` | ~1,245 | Command packet builders |
| Transport | `src/transport/` | ~700 | Abstract I/O layer (USB + serial) |
| Ptrace Writer | `src/ptrace/` | ~121 | QUTS-compatible CSV output |
| Types | `include/diaglogger/` | ~50 | Platform types, aliases |

---

## SLIDE 6 — Qualcomm Log Collector Orchestrator
**Central state machine — 4 startup phases:**

1. **Initialize**
   - Parse CFG file → extract log/event/message masks
   - Create transport (USB or serial) → open device
   - Open PTRACE output file

2. **Handshake** *(~10 sequential diag commands)*
   - Loopback → Status → Build ID
   - Switch to Non-HDLC framing
   - Security keys, Feature Query, DiagID setup
   - QDSS setup (if enabled), ADPL setup (if enabled)

3. **Configure Masks**
   - LOG_CONFIG (per equipment ID), EVENT_MASK, MSG_CONFIG

4. **Launch RX Thread**
   - Continuous: read → decode → write ptrace

---

## SLIDE 7 — Diag Command Modules
*(Organized into 5 command sub-modules)*

| Sub-Module | Key Commands |
|------------|-------------|
| `commands` | Loopback (0x7B), Status (0x0C), LOG_CONFIG (0x73), EVENT_MASK (0x82), MSG_CFG (0x7D) |
| `handshake_commands` | Non-HDLC switch, Security keys, Feature query, DiagID, Hypervisor |
| `subsystem_commands` | Generic 0x4B framing (subsys_id + subcmd + payload) |
| `qdss_commands` | HW accel query/enable, STM enable, Trace sink, SW trace filters |
| `adpl_commands` | ADPL enable/disable, version negotiation, ADPL packet decode |

**CFG Parser:** Supports v1 binary, v2 binary, and HDLC stream formats

---

## SLIDE 8 — Protocol Stack
*(Layered view of how diag data is framed)*

```
Application Layer (Diag Commands)
    ↓  payload bytes (e.g., LOG_CONFIG command)
Framing Layer
    ├── HDLC: CRC-16 + escape bytes + 0x7E delimiters
    └── Non-HDLC: 0x7E + version + 2B length + payload + 0x7E
         ↓
Transport Layer
    ├── QcDevice USB (primary, ring-buffered)
    └── Serial Port (COM / tty, fallback)
         ↓
Physical (USB or Serial to Qualcomm device)
```

**HDLC Details:**
- Flag byte: `0x7E`
- Escape byte: `0x7D`, XOR mask: `0x20`
- CRC-16-CCITT: poly `0x8408`, init `0xFFFF`
- Format: `payload + CRC(2B LE) → escape → 0x7E`

---

## SLIDE 9 — Transport Layer
*(Abstraction over USB and serial devices)*

**`Transport` (abstract interface):**
```
open() / close() / read(buf, timeout) / write(buf)
```

**`QcDeviceTransport`** (primary implementation):
- Wraps Qualcomm QDS (QcDevice) USB driver API
- **Ring buffer** (512 KB): bridges blocking `ReadFromDevice()` to timeout-based `read()`
- **RX worker thread**: continuously populates ring buffer from hardware
- Retry logic: 8 retries on open, 5 retries on write
- Device enumeration via `GetDeviceList()`

**`TransportFactory`**: creates the appropriate implementation based on device name

---

## SLIDE 10 — Threading Model
*(Only 3 threads in the entire application)*

```
┌──────────────────┐   ┌─────────────────────┐   ┌───────────────────────┐
│   Main Thread    │   │  QcDevice RxWorker  │   │ DiagLogger RxThread   │
│                  │   │  (inside Transport) │   │                       │
│ CLI parsing      │   │ QcDevice::          │   │ transport.read(buf)   │
│ Config setup     │   │  ReadFromDevice()   │   │  ← pop from ring buf  │
│ Handshake        │   │  (blocking)         │   │ hdlc.feed(buf)        │
│ Mask config      │──>│  → push ring buf    │──>│ while frame:          │
│ Wait for signal  │   │  → notify CV        │   │   ptrace.write_rec()  │
│ logger.stop()    │   │                     │   │ check running_        │
└──────────────────┘   └─────────────────────┘   └───────────────────────┘
```

**Thread Safety Primitives:**
- `std::atomic<bool> running_` — stop signal
- Mutex + condition variable — ring buffer producer/consumer
- Mutex — ptrace file writes

---

## SLIDE 11 — Data Flow (End-to-End)
*(From CLI invocation to ptrace file on disk)*

```
User: $ diaglogger --cfg masks.cfg --port QcDevice1 --output trace.ptrace --qdss

  ↓ Parse args
  ↓ Parse CFG → log masks, event mask, msg masks
  ↓ Open QcDevice transport (USB)
  ↓ Open ptrace CSV file
  ↓ Loopback test (verify device alive)
  ↓ Handshake (10 commands: status, build ID, Non-HDLC, security, features, QDSS)
  ↓ Configure masks (LOG_CONFIG × N, EVENT_MASK, MSG_CFG × N)
  ↓ Launch RX thread

  [Concurrent]
  QcDevice RxWorker ─────────────────> Ring Buffer
  DiagLogger RxThread: read ring buf → HDLC decode → ptrace CSV row

  User Ctrl+C / --duration expires
  ↓ Signal running_=false
  ↓ Join RX thread
  ↓ Send disable commands (best-effort)
  ↓ Close ptrace + transport
  ↓ Print stats (packets received, bytes, errors)
  ↓ _exit(0)

Output: trace.ptrace (QUTS-compatible CSV, hex-encoded diag packets)
```

---

## SLIDE 12 — CFG File Formats
*(Input: mask configuration files)*

| Format | Magic | Description |
|--------|-------|-------------|
| CFG v1 | `0x00001A00` | Binary: log masks (per equip ID 0–15), event mask, msg masks |
| CFG v2 | `0x00001A01` | Binary v2 with subscription-aware variant |
| HDLC Stream | _(QUTS generated)_ | HDLC-framed diag command stream parsed to extract masks |

**Parsed result — `CfgMasks` struct:**
- `log_masks`: map of equipment ID → bitmask
- `event_mask`: 8192-bit bitmask
- `msg_masks`: SSID range → log level bitmask

---

## SLIDE 13 — QDSS Trace Flow
*(Optional: `--qdss` flag)*

```
Query HW acceleration types
    ↓ supports: STM · ATB · LPASS_STM
Enable HW acceleration per type
    ↓
Enable Diag-over-STM (per processor mask, default 0x3F = all)
    ↓
Set trace sink → USB (sink ID = 3)
    ↓
Configure SW trace filters (entity IDs per processor)
    ↓
QDSS data flows back as diag packets on the same port
    ↓
Captured in ptrace file alongside regular diag logs
```

---

## SLIDE 14 — ADPL Data Path Logging
*(Optional: `--adpl` flag)*

```
Enable ADPL via IPA subsystem (subsys 0x4B, cmd 0x4000)
    ↓
Device responds with negotiated version (v1–v7)
    ↓ (if v3+)
Query modem base timestamp for relative time calculation
    ↓
ADPL packets arrive on the diag channel
    ↓
Decoder parses version-specific header (v1–v7 have different timestamp formats)
    ↓
Extracts diag payload for ptrace write
```

**ADPL Packet Versions:**
| Version | Timestamp | Notes |
|---------|-----------|-------|
| v1–v2 | None / basic | Simple header |
| v3–v4 | Relative modem time | 32-bit base + offset |
| v5–v7 | Extended timestamps | 64-bit with modem sync |

---

## SLIDE 15 — Ptrace Output Format
*(QUTS-compatible CSV)*

```
"appname","version"
"DiagLogger","1.0"
"",""
"datetime","datatype","data"
"2024/04/24 15:30:45.123","Rx","7B 01 02 03 7E"
"2024/04/24 15:30:45.124","Tx","7B 01 02 7E"
...
```

**Columns:**
| Column | Description |
|--------|-------------|
| datetime | Wall clock timestamp (ms precision) |
| datatype | `Tx` (command sent) or `Rx` (packet received) |
| data | Space-separated hex bytes of raw diag packet |

**Viewer:** Openable in QUTS Port Trace Viewer for offline analysis

---

## SLIDE 16 — Technology Stack
| Category | Technology |
|----------|------------|
| Language | C++17 |
| Build System | CMake 3.14+ |
| USB Driver | QDS (QcDevice) — compiled in statically |
| Windows I/O | Win32 API: `CreateFile`, `ReadFile`, `WriteFile`, `SetCommState` |
| Linux I/O | POSIX: `open`, `termios`, `read`, `write`, `select` |
| Windows Device Discovery | `SetupAPI`, `CfgMgr32`, `ShlWAPI` |
| Threading | `std::thread`, `std::atomic`, `std::mutex`, `std::condition_variable` |
| Unit Testing | Catch2 (or similar, `tests/` directory) |
| No external libs | No Boost, Qt, or third-party libraries |

**Platform Support:**
- Windows (MSVC 2019+)
- Linux (GCC / Clang)

---

## SLIDE 17 — CLI Usage
*(Full command reference)*

```bash
diaglogger --cfg <file.cfg>             # Mask config (required)
           --port <QcDevice_name>       # USB device or COM port (required)
           --output <trace.ptrace>      # Output file (default: timestamped)
           --baud <rate>                # Baud rate (default: 115200)
           --duration <seconds>         # Auto-stop after N seconds
           --verbose                    # Print command IDs to stderr
           --qdss                       # Enable QDSS trace setup
           --adpl                       # Enable ADPL data path logging
           --qdss-processors <hex>      # Processor mask (default: 0x3F = all)
           --adpl-version <N>           # ADPL version to request (default: 7)
           --list-devices               # Enumerate QcDevice devices and exit
           --help                       # Show usage
```

---

## SLIDE 18 — Build & Deployment
*(Single executable, no runtime dependencies)*

**Windows:**
```bash
cmake -B build -DQDS_ROOT=../../QDS -G "Visual Studio 16 2019"
cmake --build build --config Release
# Output: build/Release/diaglogger.exe
```

**Linux:**
```bash
cmake -B build -DQDS_ROOT=../../QDS
cmake --build build --config Release
# Output: build/diaglogger
```

**CMake Targets:**
| Target | Type | Description |
|--------|------|-------------|
| `diaglogger_lib` | Static library | All modules + QDS sources |
| `diaglogger` | Executable | CLI wrapper |
| `test_*` | Executables | Unit tests (optional: `-DBUILD_TESTS=ON`) |

---

## SLIDE 19 — Unit Test Coverage
*(10 test files covering all major modules)*

| Test File | Module Tested |
|-----------|---------------|
| `test_hdlc.cpp` | HDLC encode/decode, CRC-16 |
| `test_non_hdlc.cpp` | Non-HDLC length-prefixed framing |
| `test_commands.cpp` | Basic diag commands |
| `test_cfg_parser.cpp` | CFG v1/v2/HDLC stream parsing |
| `test_handshake_commands.cpp` | Handshake packet builders |
| `test_subsystem_commands.cpp` | Generic 0x4B framing |
| `test_qdss_commands.cpp` | QDSS configuration commands |
| `test_adpl_commands.cpp` | ADPL enable, packet decode |

---

## SLIDE 20 — Key Design Decisions
| Decision | Rationale |
|----------|-----------|
| Raw byte capture (no parsing) | Simplicity, no schema dependency, QUTS does the analysis |
| Transport abstraction layer | Supports USB (QcDevice) and serial; easily extensible |
| HDLC streaming decoder | Handles partial frames and fragmented TCP/serial reads |
| Ring buffer for USB RX | Decouples blocking QcDevice reads from the RX processing thread |
| CFG file reuse from QUTS | No need to manually specify mask IDs; existing configs just work |
| HDLC → Non-HDLC switchover | Non-HDLC is more efficient; fall back silently if unsupported |
| `_exit(0)` on shutdown | QDS library has cleanup issues; hard exit avoids hangs |
| Statically linked QDS | Single executable, no DLL dependency on the target machine |

---

## SLIDE 21 — Summary
**Qualcomm Log Collector in one sentence:**
> A minimal, cross-platform C++ CLI tool that configures a Qualcomm device for diagnostic logging and captures the raw packet stream to a QUTS-compatible file — with no commercial toolchain required.

**Key Strengths:**
- Clean 6-layer modular architecture
- Three logging modes over a single connection (Diag, QDSS, ADPL)
- Full Diag handshake protocol implementation
- Cross-platform: Windows MSVC + Linux GCC
- No third-party runtime dependencies
- Comprehensive unit test coverage
- QUTS-compatible output — drop-in for existing workflows

**Future Extensions:**
- Serial port transport (Linux tty / Windows COM) *(placeholder exists)*
- Live packet statistics / monitoring mode
- Additional CFG format versions
- macOS support *(CMake placeholders present)*

---
*Qualcomm Log Collector — Qualcomm Innovation Center — Apache 2.0*
