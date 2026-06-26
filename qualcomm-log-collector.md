# DiagLogger - Open-Source Diag Protocol Logging Tool

## Context

A standalone, open-source C++ CLI tool called **DiagLogger** that captures Qualcomm diag protocol data from a device, using CFG files for logging mask configuration and saving raw data as ptrace files. This is a simplified alternative to QUTS -- no HDF, no parsed packets, no Thrift RPC, no service architecture. Just connect, configure masks, and capture raw bytes.

DiagLogger supports three logging modes over the **same diag serial connection**:
- **Diag logging** -- standard log/event/message packets via LOG_CONFIG, SET_EVENT_MASK, EXT_MSG_CFG
- **QDSS logging** -- Qualcomm Debug Subsystem hardware trace, configured via diag subsystem commands (0x4B), data flows back as diag packets on the same port
- **ADPL logging** -- Advanced Data Path Logging, configured via diag subsystem commands (0x4B with IPA subsystem), data flows back on the same port

All three modes are configured and captured through a single diag connection. The device determines which modes are available based on its capabilities.

---

## Project Structure

```
DiagLogger/
├── CMakeLists.txt                      # Top-level CMake build
├── LICENSE                             # Apache 2.0
├── README.md                           # Usage, build instructions
├── DiagLogger.md                       # This plan file
├── cmake/
│   └── CompilerWarnings.cmake          # Shared compiler warning flags
├── src/
│   ├── main.cpp                        # CLI entry point, argument parsing, signal handling
│   ├── hdlc/
│   │   ├── crc16.h                     # CRC-16-CCITT lookup table (header-only)
│   │   ├── hdlc.h                      # HDLC encoder/decoder API
│   │   └── hdlc.cpp                    # HDLC framing: 0x7E delimiters, escape, CRC
│   ├── diag/
│   │   ├── commands.h                  # Diag command IDs & packet builder API
│   │   ├── commands.cpp                # Build LOG_CONFIG, SET_EVENT_MASK, EXT_MSG_CFG, etc.
│   │   ├── subsystem_commands.h       # Subsystem command builder API (shared by QDSS/ADPL)
│   │   ├── subsystem_commands.cpp     # Build subsystem dispatch packets (cmd_id=0x4B)
│   │   ├── qdss_commands.h            # QDSS-specific subsystem command builders
│   │   ├── qdss_commands.cpp          # HW accel query, sink config, STM/ATB enable
│   │   ├── adpl_commands.h            # ADPL-specific subsystem command builders
│   │   ├── adpl_commands.cpp          # DPL enable/disable via IPA subsystem
│   │   ├── cfg_parser.h               # CFG file parser API
│   │   └── cfg_parser.cpp             # CFG v1/v2 binary format reader
│   ├── transport/
│   │   ├── transport.h                 # Abstract transport interface
│   │   ├── serial_port.h              # Cross-platform serial port header
│   │   └── serial_port.cpp            # Windows (CreateFile/DCB) + Linux (termios) impl
│   ├── ptrace/
│   │   ├── ptrace_writer.h            # Ptrace file writer API
│   │   └── ptrace_writer.cpp          # Binary ptrace format writer
│   └── logger/
│       ├── diag_logger.h              # Main orchestrator class
│       └── diag_logger.cpp            # Connect → configure → receive loop → shutdown
├── include/
│   └── diaglogger/
│       └── types.h                     # Shared types, platform macros, byte buffer alias
└── tests/
    ├── CMakeLists.txt
    ├── test_hdlc.cpp                   # HDLC encode/decode round-trip
    ├── test_cfg_parser.cpp             # CFG parsing with sample file
    ├── test_commands.cpp               # Command byte sequence validation
    ├── test_subsystem_commands.cpp     # Subsystem command byte validation
    ├── test_qdss_commands.cpp          # QDSS command byte validation
    ├── test_adpl_commands.cpp          # ADPL command byte validation
    └── test_data/
        └── sample.cfg                  # Sample CFG file for testing
```

---

## Core Modules

### 1. `hdlc/` -- HDLC Framing (Encode/Decode + CRC-16)

**Purpose**: Frame diag packets for serial transport using standard Qualcomm HDLC.

- **Constants**: `FLAG=0x7E`, `ESCAPE=0x7D`, `ESCAPE_MASK=0x20`
- **`encode(data, len) -> vector<uint8_t>`**: Compute CRC-16 -> append CRC (LE) -> escape special bytes -> wrap in 0x7E delimiters
- **`Decoder` class (streaming)**: `feed(data, len)` accepts raw serial bytes; `next_frame()` returns next complete HDLC-decoded payload (or empty). Handles partial frames across multiple `feed()` calls.
- **`crc16(data, len) -> uint16_t`**: CRC-16-CCITT with lookup table

**Status**: Implemented.

*Reference*: QUTS uses `System/Net/Hdlc/FrameStream` (Diag.cpp:2928).

### 2. `diag/commands` -- Diag Command Builder + CFG Parser

**Commands** (from QUTS emulator at `Emulator/Protocol/Diag.cpp`):

| Command | ID | Purpose |
|---|---|---|
| `LOOPBACK` | 0x7B | Connection verification handshake |
| `LOG_CONFIG` | 0x73 | Configure log code masks (op=0x03 SET_LOG_MASK) |
| `SET_EVENT_MASK` | 0x82 | Configure event ID bitmask |
| `EXT_MSG_CFG` | 0x7D | Configure debug message subsystem/level masks |
| `EVENT_RPT` | 0x60 | Enable (1) / disable (0) event reporting |
| `STATUS` | 0x0C | Device status request |

**Builders**:
- `build_loopback_request()` -> loopback packet
- `build_log_config_commands(masks)` -> one LOG_CONFIG per equipment ID (0-15)
- `build_set_event_mask(event_mask)` -> single SET_EVENT_MASK packet
- `build_event_report(enable)` -> EVENT_RPT enable/disable
- `build_ext_msg_config(msg_masks)` -> EXT_MSG_CFG packets per SSID range

**CFG Parser**:
- Parse CFG v1/v2 binary format -> `CfgMasks` struct containing:
  - `log_masks[16]`: per-equipment-ID bitmasks for log codes
  - `event_mask`: bitmask for event IDs
  - `msg_masks[]`: array of {ssid_start, ssid_end, levels[]} for debug messages
- Auto-detect version from file header

**Status**: Implemented.

*Reference*: QUTS parses CFG in `DiagServiceHandler.cpp:2174-2260` via `CfgFile.read()`, `getMask(entityType, subId)`.

### 3. `diag/subsystem_commands` -- Subsystem Command Builder (NEW)

**Purpose**: Build diag subsystem dispatch packets (cmd_id=0x4B). Both QDSS and ADPL configuration use subsystem commands sent over the same diag serial connection.

**Subsystem command packet format**:
```
[1 byte]  cmd_id = 0x4B (SUBSYS)
[1 byte]  subsystem_id          -- target subsystem
[2 bytes] subsystem_cmd (LE)    -- subsystem-specific command code
[N bytes] payload               -- subsystem-specific fields
```

**Common subsystem IDs**:
| Subsystem | ID | Used By |
|---|---|---|
| `DIAG_SERV` | 0x12 | QDSS HW acceleration, transport query, STM config |
| `QDSS` | 0x5A | QDSS trace sink, SW trace filters, entity filters |
| `IPA` | 0x5A (cmd=16384) | ADPL enable/disable |

**Builders**:
- `build_subsystem_command(subsys_id, subcmd, payload)` -> generic subsystem packet

**Status**: Not yet implemented.

*Reference*: QUTS uses `QdssDiagManager::sendSubsystemCommand()` and `AdplDiagManager::sendSubsystemCommand()`.

### 4. `diag/qdss_commands` -- QDSS Configuration Commands (NEW)

**Purpose**: Build QDSS-specific subsystem commands. All sent over the existing diag serial connection -- no separate transport needed. QDSS-routed data (Diag-over-STM) arrives back as diag packets on the same port.

#### QDSS Configuration Flow

```
1. Query HW acceleration support (what does device support?)
2. Enable ATB/STM/LPASS-STM (hardware trace channels)
3. Set trace sink to USB (tell device where to route trace data)
4. Configure per-processor SW trace and entity filters
5. Device begins sending QDSS-routed data as diag packets on same port
```

#### Command Details

**HW Acceleration Query** (subsys=0x12, cmd=548/0x0224):
```
Request payload:
  [1 byte] cmd_version = 1
  [1 byte] operation   = 2 (QUERY)
  [1 byte] reserved    = 0
  [1 byte] hw_type     = 0 (ALL)
  [1 byte] hw_version  = 1

Response contains:
  - Supported HW types (ATB=2, STM=1, LPASS_STM=3)
  - Per-type DiagID masks and ATID assignments
  - Response version (1=static ATID, 2=dynamic ATID)
```

**HW Acceleration Enable** (subsys=0x12, cmd=548/0x0224):
```
Request payload:
  [1 byte]  cmd_version   = <from query response>
  [1 byte]  operation     = 1 (ENABLE)
  [1 byte]  reserved      = 0
  [1 byte]  hw_type       = 1(STM) | 2(ATB) | 3(LPASS_STM)
  [1 byte]  hw_version    = 1
  [4 bytes] diagid_mask   = <bitmask of diag IDs to enable>
```

**Transport Query** (subsys=0x12, cmd=547/0x0223):
```
Response: transport type (USB=0, PCIe=3, UART=1)
```

**Diag Over STM Enable** (subsys=0x12, cmd=532/0x0214):
```
Request payload:
  [1 byte] stm_command = 1 (ENABLE_DIAG_OVER_STM)
  [1 byte] processor_mask = 0x3F (ALL)

STM commands:
  0 = DISABLE_DIAG_OVER_STM
  1 = ENABLE_DIAG_OVER_STM
  2 = QUERY_PROCESSORS
  3 = AUTO_QUERY_PROCESSORS
```

**Trace Sink Configuration** (subsys=0x5A, cmd=257/0x0101):
```
Request payload:
  [1 byte] sink_type

Sink types:
  0 = ETB (Embedded Trace Buffer)
  1 = DDR (Device memory)
  2 = TPIU_SET_A
  3 = USB (default)
  4 = USB_NON_BAM
  5 = TPIU_SET_B
  6 = SD card
  7 = File
  8 = PCIe
```

**Per-Processor SW Trace Filter** (subsys=0x5A, various cmds):
```
Filter SW Trace commands (per processor):
  APPS:  0x0106 (262)
  MODEM: 0x0206 (518)
  WCNSS: 0x0306 (774)
  ADSP:  0x0506 (1286)
  SLPI:  0x0806 (2054)
  CDSP:  0x0D06 (3334)

Filter Entity commands (per processor):
  APPS:  0x0108 (264)
  MODEM: 0x0208 (520)
  WCNSS: 0x0308 (776)
  ADSP:  0x0508 (1288)
  SLPI:  0x0808 (2056)
  CDSP:  0x0D08 (3336)

Request payload:
  [1 byte] state (1=enable, 0=disable)
  [optional] entity_id
```

**Processor Masks**:
```
MODEM = 0x01
ADSP  = 0x02
WCNSS = 0x04
APSS  = 0x08
SLPI  = 0x10
CDSP  = 0x20
ALL   = 0x3F
```

**Key ATIDs** (Address Trace IDs):
- STM ATID: 16 (default)
- ATB ATID: 50
- LPASS STM ATID: 25

**Builders**:
- `build_qdss_hw_accel_query()` -> query supported HW acceleration types
- `build_qdss_hw_accel_enable(hw_type, diagid_mask)` -> enable ATB/STM/LPASS_STM
- `build_qdss_hw_accel_disable(hw_type)` -> disable
- `build_qdss_query_transport()` -> query transport type
- `build_qdss_set_trace_sink(sink_type)` -> set trace sink (default USB=3)
- `build_qdss_stm_enable(processor_mask)` -> enable Diag-over-STM
- `build_qdss_stm_disable()` -> disable Diag-over-STM
- `build_qdss_filter_sw_trace(processor, state)` -> per-processor SW trace filter
- `build_qdss_filter_entity(processor, state, entity_id)` -> per-processor entity filter

**Status**: Not yet implemented.

*Reference*: QUTS implements in `QdssDiagManager.cpp` (1800+ lines). Key constants from `QdssDiagManager.h`.

### 5. `diag/adpl_commands` -- ADPL Configuration Commands (NEW)

**Purpose**: Build ADPL-specific subsystem commands. All sent over the existing diag serial connection. ADPL is configured via the IPA subsystem. Once enabled, ADPL data flows back on the same diag connection as packets with ADPL-specific headers embedded in the payload.

#### ADPL Configuration Flow

```
1. Send DPL enable command via IPA subsystem
2. Parse response: negotiate DPL version (device returns supported version)
3. If version >= 3: query modem base timestamp for relative time calculation
4. Device begins sending ADPL-framed data as diag packets on same port
5. On shutdown: send DPL disable command
```

#### Command Details

**ADPL Enable** (subsys=IPA, cmd=16384/0x4000):

Raw bytes: `0x4B <IPA_SUBSYS_ID> 0x00 0x40 <fields...>`

```
Request payload (FieldValueList format):
  [1 byte] dplVersion    = 7 (ADPL_HIGHEST_SUPPORTED, negotiate down)
  [1 byte] packetLimit   = 0 (unlimited)
  [2 bytes] bufferSize   = 15360
  [1 byte] dplCommand    = 1 (enable)
  [1 byte] reserved      = 0

Response fields:
  [1 byte] DPL Version   -- actual version negotiated by device (1-7)
  [1 byte] DPL Status    -- 1=success, other=error
```

**ADPL Disable** (subsys=IPA, cmd=16384/0x4000):
```
Request payload:
  [1 byte] dplVersion    = 7 (ADPL_HIGHEST_SUPPORTED)
  [1 byte] packetLimit   = 4
  [2 bytes] bufferSize   = 15360
  [1 byte] dplCommand    = 0 (disable)
  [1 byte] reserved      = 0
```

**Modem Timestamp Query** (for V3+):
- Sent via diag subsystem command after enable
- Returns base CDMA timestamp used for relative time calculation in V3+ packets

#### ADPL Data Packet Versions (in received diag payloads)

Once ADPL is enabled, the device sends ADPL-framed data. Each ADPL packet has a version-specific header:

**V1 Header** (4 bytes):
```
[6 bits] padCount       -- padding byte count
[2 bits] reserved
[1 byte] muxId          -- multiplexer ID
[2 bytes] packetLength  -- payload length (network byte order)
```
- Payload follows immediately after header

**V2 Header**: Same as V1 but payload padded to 32-bit boundary.

**V3/V4 Header** (~16 bytes):
```
[1 byte]  statusOpcode     -- must be 0x10 for valid frame
[2 bytes] packetLength     -- payload length
[2 bytes] sequenceNumber   -- frame sequence counter
[1 byte]  endPointSrcIndex -- USB endpoint source
[1 byte]  reserved1
[1 byte]  reserved2
[1 byte]  reserved3
[1 byte]  muxId            -- multiplexer ID
[6 bytes] dplTimeOfDay     -- timestamp
```
- V3: timestamp in 100ms units (relative, needs base time from modem query)
- V4: timestamp in milliseconds (GPS time)
- Payload padded to 8-byte boundary: `pad = (len & 7) ? (8 - (len & 7)) : 0`

**V5 Header**: Same structure as V3/V4.
- `statusOpcode` must be 0x10
- Timestamp: `dplTimeOfDay * 10` (microseconds)
- 8-byte padding

**V6/V7 Header**: Extended timestamp.
- `statusOpcode` must be 0x0C or 0x0D (changed from 0x10)
- Added `timeOfDay` (16-bit) for high bits
- Full timestamp: `(dplTimeOfDay | (timeOfDay << 48))` in microseconds
- V6: 8-byte payload padding
- V7: 16-byte payload padding: `pad = (len & 0xF) ? (16 - (len & 0xF)) : 0`

**Builders**:
- `build_adpl_enable(version)` -> IPA subsystem enable command
- `build_adpl_disable()` -> IPA subsystem disable command
- `build_adpl_query_modem_timestamp()` -> modem timestamp query (for V3+)
- `parse_adpl_enable_response(data)` -> extract negotiated version and status

**ADPL Packet Decoder** (for parsing received data):
- `detect_adpl_version(header_bytes)` -> auto-detect version from statusOpcode
- `decode_adpl_packet(data, version)` -> extract header + diag payload from ADPL frame

**Status**: Not yet implemented.

*Reference*: QUTS implements in `AdplDiagManager.cpp` (configureAdpl at line 62, disableAdpl at line 141). ADPL packet parsing in `Adpl.cpp` (1700+ lines). Header structures in `System/Net/Adpl/`.

### 6. `transport/` -- Serial Port Abstraction

**Interface** (`Transport`): `open()`, `close()`, `is_open()`, `write()`, `read(timeout)`, `description()`

**`SerialPort`** implementation:
- **Windows**: `CreateFileA("\\\\.\\COM3")`, `SetCommState()`, `SetCommTimeouts()`, `ReadFile()`/`WriteFile()`
- **Linux**: `open("/dev/ttyUSB0")`, `cfmakeraw()`, `cfsetispeed()`, `read()`/`write()` with `select()` timeout

**Config**: port name, baud rate (default 115200), read timeout (default 100ms)

**Status**: Implemented.

*Reference*: QUTS uses `CommonIO` abstraction (`Communication/CommonIO.h`).

### 7. `ptrace/` -- Ptrace File Writer

**Binary record format** (sequential, no file header):
```
Per record:
  [8 bytes] uint64_t timestamp    -- Windows FILETIME (100ns ticks since 1601-01-01)
  [1 byte]  uint8_t  direction    -- 0x00=TX, 0x01=RX
  [4 bytes] uint32_t payload_len  -- little-endian
  [N bytes] payload               -- raw HDLC-decoded diag packet bytes
```

**`PtraceWriter`**: `open()`, `write_record(direction, data, len)`, `close()`, stats (record count, bytes written)

**Timestamp helper** `now_as_filetime()`:
- Windows: `GetSystemTimeAsFileTime()`
- Linux: `clock_gettime(CLOCK_REALTIME)` -> convert: `(unix_ns / 100) + 116444736000000000ULL`

**Status**: Implemented.

*Reference*: QUTS ptrace extension at `DeviceManagerHandler.cpp:383`, read by `PortTraceReader`.

### 8. `logger/` -- DiagLogger Orchestrator

**`DiagLogger` class** -- owns transport, HDLC codec, ptrace writer, receive thread. Handles all three logging modes (diag, QDSS, ADPL) over the same connection.

**Lifecycle**:
```
start():
  1. Open serial port
  2. Open ptrace output file
  3. Send HDLC loopback -> wait for echo (confirms device alive)
  4. Send diag mask commands (LOG_CONFIG, SET_EVENT_MASK, EVENT_RPT, EXT_MSG_CFG)
  5. If QDSS enabled:
     a. Query HW acceleration support
     b. Enable ATB/STM for supported processors
     c. Set trace sink to USB
     d. Enable Diag-over-STM
     e. Configure SW trace and entity filters
  6. If ADPL enabled:
     a. Send DPL enable via IPA subsystem
     b. Parse response, negotiate version
     c. If V3+: query modem base timestamp
  7. Launch receive thread

rx_loop() [receive thread]:
  - Read serial data in a loop (100ms blocking reads)
  - Feed to HDLC Decoder
  - For each decoded frame -> write to PtraceWriter (RX direction)
  - Track stats: packet count, byte count, CRC errors
  - Check running_ atomic flag each iteration

stop():
  1. Set running_ = false, join rx thread
  2. If ADPL enabled: send DPL disable (best-effort)
  3. If QDSS enabled: disable STM (best-effort)
  4. Send EVENT_RPT disable + LOG_CONFIG disable (best-effort)
  5. Close ptrace file, close serial port
  6. Print summary to stdout
```

**Status**: Basic diag-only mode implemented. QDSS and ADPL configuration steps not yet implemented.

### 9. `main.cpp` -- CLI Entry Point

**Usage**:
```
diaglogger --cfg <file.cfg> --port <COM3|/dev/ttyUSB0> [options]
```

**Arguments**:
| Flag | Required | Default | Description |
|---|---|---|---|
| `--cfg` | Yes | -- | Path to CFG mask file |
| `--port` | Yes | -- | Serial port name |
| `--output` | No | `diag_<timestamp>.ptrace` | Output ptrace file path |
| `--baud` | No | 115200 | Baud rate |
| `--duration` | No | unlimited | Auto-stop after N seconds |
| `--verbose` | No | off | Print decoded command IDs to stderr |
| `--qdss` | No | off | Enable QDSS logging (if device supports) |
| `--adpl` | No | off | Enable ADPL logging (if device supports) |
| `--qdss-processors` | No | all | QDSS processor mask (all/modem/adsp/apss/etc.) |
| `--adpl-version` | No | auto | Request specific ADPL version (1-7) |

**Signal handling**: SIGINT/SIGTERM -> set global `g_should_stop` atomic -> DiagLogger::stop()

**Status**: Basic diag-only CLI implemented. QDSS/ADPL flags not yet implemented.

---

## Diag Connection Handshake Command Reference

All commands needed to establish a diag connection (including QDSS and ADPL). These are **not** mask configuration commands (LOG_CONFIG, SET_EVENT_MASK, EVENT_RPT, EXT_MSG_CFG) -- those happen after the connection handshake.

**Key insight**: All handshake commands use hardcoded C++ structures or programmatic byte arrays. None are defined in `.db` files (those only define log/event/message content for parsing).

### QUTS Handshake Sequence (Diag.cpp:8459)

```
Diag::connect()
  → doSimpleConnect()          -- LOOPBACK, STATUS, EXT_BUILD_ID
  → requestSwitchToNonHdlc()   -- NonHdlc Switch (subsys cmd)
  → requestIPKeys()             -- Security Key Request (subsys cmd)
  → diagFeatureQuery()          -- Feature Query (subsys cmd)
  → DiagID Feature Enable       -- if Feature Query says DiagID supported
  → DiagID Priority Config      -- if DiagID supported
  → DiagID Registration Table   -- if DiagID supported
  → DiagID Table Query           -- if DiagID supported
  → configureHypervisorOnDevice()  -- Hypervisor Feature Enable (optional)
  → attemptDiagOverQdssConnection()  -- QDSS setup (optional)
  → ADPL enable                       -- ADPL setup (optional)
```

### Non-Subsystem Commands (Direct Diag)

| Command | ID | Packet Format | Source |
|---|---|---|---|
| **LOOPBACK** | `0x7B` | `[0x7B][payload...]` -- device echoes back | Hardcoded byte |
| **STATUS** | `0x0C` | `[0x0C]` -- device returns status info | Hardcoded byte |
| **EXT_BUILD_ID** | `0x9F` | `[0x9F]` -- returns extended build identification | Hardcoded byte |

### DIAG_SERV Subsystem Commands (subsys_id=0x12)

All use format: `[0x4B][0x12][subcmd_lo][subcmd_hi][payload...]`

| Command | SubCmd | Packet Format | C++ Source |
|---|---|---|---|
| **NonHdlc Switch** | varies | `[0x4B][0x12][cmd_lo][cmd_hi][fields...]` | `::Diag::NonHdlcSubsystemRequest` (TABL library) |
| **Security Key Request** | `0x0833` | `[0x4B][0x12][0x33][0x08][key_data...]` | `::Diag::SecurityKeyRequest` (TABL library) |
| **Feature Query** | varies | `[0x4B][0x12][cmd_lo][cmd_hi][version][feature_mask...]` | `::Diag::DiagFeatureQueryRequest` (TABL library) |
| **DiagID Feature Enable** | `0x0232` | `[0x4B][0x12][0x32][0x02][version][enable]` | Programmatic (Diag.cpp:14560) |
| **DiagID Priority Config** | `0x0234` | `[0x4B][0x12][0x34][0x02][version][priority_data...]` | `DiagIDPriorityConfigRequest` (Diag.cpp:175, `#pragma pack(push,1)`) |
| **DiagID Registration Table** | `0x0233` | `[0x4B][0x12][0x33][0x02][table_data...]` | Programmatic (Diag.cpp) |
| **DiagID Table Query** | `0x0222` | `[0x4B][0x12][0x22][0x02]` | Programmatic (Diag.cpp) |
| **Hypervisor Feature Enable** | varies | `[0x4B][0x12][cmd_lo][cmd_hi][config...]` | `HypervisorFeatureEnableRequest` (Diag.cpp:228, `#pragma pack(push,1)`) |

### QDSS Setup Commands (subsys_id=0x12)

Sent by `QdssDiagManager.cpp` during `attemptDiagOverQdssConnection()`.

| Command | SubCmd | Subsys | Packet Format | Purpose |
|---|---|---|---|---|
| **HW Accel Query** | `0x0224` (548) | 0x12 | `[0x4B][0x12][0x24][0x02][ver=1][op=2][0][hw=0][hw_ver=1]` | Query supported HW types (ATB/STM/LPASS_STM) |
| **HW Accel Enable** | `0x0224` (548) | 0x12 | `[0x4B][0x12][0x24][0x02][ver][op=1][0][hw_type][hw_ver][diagid_mask(4B)]` | Enable HW acceleration per type |
| **STM Query** | `0x0214` (532) | 0x12 | `[0x4B][0x12][0x14][0x02][cmd=2][proc_mask]` | Query which processors support STM |
| **STM Enable** | `0x0214` (532) | 0x12 | `[0x4B][0x12][0x14][0x02][cmd=1][proc_mask=0x3F]` | Enable Diag-over-STM |
| **Transport Query** | `0x0223` (547) | 0x12 | `[0x4B][0x12][0x23][0x02]` | Query transport type (USB/PCIe/UART) |
| **Island Buffering** | `0x0235` (565) | 0x12 | `[0x4B][0x12][0x35][0x02][enable]` | Enable/disable island mode buffering |

### QDSS Trace Configuration Commands (subsys_id=0x5A)

Sent after QDSS setup to configure trace routing.

| Command | SubCmd | Packet Format | Purpose |
|---|---|---|---|
| **Trace Sink** | `0x0101` (257) | `[0x4B][0x5A][0x01][0x01][sink_type]` | Set trace sink (USB=3) |
| **STM Filter** | `0x0103` (259) | `[0x4B][0x5A][0x03][0x01][state]` | Enable/disable STM filtering |
| **SW Trace Filter** | per-processor | `[0x4B][0x5A][cmd_lo][cmd_hi][state]` | Per-processor SW trace filter |
| **Entity Filter** | per-processor | `[0x4B][0x5A][cmd_lo][cmd_hi][state][entity_id]` | Per-processor entity filter |

SW Trace / Entity filter subcmds by processor:

| Processor | SW Trace SubCmd | Entity SubCmd |
|---|---|---|
| APPS | 0x0106 (262) | 0x0108 (264) |
| MODEM | 0x0206 (518) | 0x0208 (520) |
| WCNSS | 0x0306 (774) | 0x0308 (776) |
| ADSP | 0x0506 (1286) | 0x0508 (1288) |
| SLPI | 0x0806 (2054) | 0x0808 (2056) |
| CDSP | 0x0D06 (3334) | 0x0D08 (3336) |

### ADPL Commands (subsys_id=0x5A, subcmd=0x4000)

Sent by `AdplDiagManager.cpp` to enable/disable Advanced Data Path Logging.

| Command | SubCmd | Packet Format | Purpose |
|---|---|---|---|
| **ADPL Enable** | `0x4000` (16384) | `[0x4B][0x5A][0x00][0x40][ver=7][pktLimit=0][bufSz=15360(2B)][cmd=1][rsv=0]` | Enable ADPL, negotiate version |
| **ADPL Disable** | `0x4000` (16384) | `[0x4B][0x5A][0x00][0x40][ver=7][pktLimit=4][bufSz=15360(2B)][cmd=0][rsv=0]` | Disable ADPL |

After enable, if negotiated version >= 3, a **Modem Timestamp Query** is sent to get the base CDMA timestamp for relative time calculation.

### C++ Structure Sources Summary

| Structure | Location | Used By |
|---|---|---|
| `::Diag::CommandRecord` | TABL library (external) | Basic diag command framing |
| `::Diag::SubsystemCommandRecord` | TABL library (external) | Subsystem command framing |
| `::Diag::NonHdlcSubsystemRequest` | TABL library (external) | NonHdlc Switch |
| `::Diag::SecurityKeyRequest` | TABL library (external) | IP Key Request |
| `::Diag::DiagFeatureQueryRequest` | TABL library (external) | Feature Query |
| `DiagIDPriorityConfigRequest` | Diag.cpp:175 (`#pragma pack(push,1)`) | DiagID Priority Config |
| `HypervisorFeatureEnableRequest` | Diag.cpp:228 (`#pragma pack(push,1)`) | Hypervisor Feature Enable |
| `CommandRequestHypervisor` | Diag.h:63-86 | Hypervisor config request |
| `CommandResponseHypervisor` | Diag.h:88-104 | Hypervisor config response |

Static byte arrays (Diag.cpp:373-381): `s_featureQueryRequestBytes`, `s_diagIdTableQueryRequestBytes`, `s_diagIdRegistrationTableRequestBytes` -- prebuilt command payloads used directly.

---

## Threading Model

```
┌─────────────────────┐       ┌──────────────────────────────┐
│    Main Thread       │       │      Receive Thread          │
│                      │       │                              │
│ 1. Parse CLI args    │       │  loop:                       │
│ 2. Parse CFG file    │       │    serial.read(buf, 100ms)   │
│ 3. Open serial port  │       │    hdlc_decoder.feed(buf)    │
│ 4. Send diag masks   │       │    while frame = next_frame()│
│ 5. Send QDSS config  │       │      ptrace.write(RX, frame) │
│ 6. Send ADPL config  │       │    check running_ flag       │
│ 7. Start rx thread ──┼──────>│                              │
│ 8. Wait for signal   │       │  (exits when !running_)      │
│ 9. Stop rx thread <──┼───────│                              │
│ 10. Send ADPL disable│       └──────────────────────────────┘
│ 11. Send QDSS disable│
│ 12. Send diag disable│
│ 13. Close & summary  │
└─────────────────────┘
```

Only 2 threads. All configuration and data flows over the same serial connection. The receive thread captures everything -- diag packets, QDSS-routed packets, and ADPL packets all arrive as HDLC-framed data on the same port.

---

## Build System

- **CMake 3.14+**, C++17, no external dependencies
- Windows: MSVC, links only Win32 API
- Linux: GCC/Clang, links only POSIX (no Boost, no Qt, no libusb)
- Optional: `BUILD_TESTS` flag for unit tests

---

## Implementation Phases

### Phase 1: Foundation -- HDLC + CRC + Serial Port
- [x] `crc16.h` -- CRC-16-CCITT lookup table
- [x] `hdlc.h/cpp` -- Encoder + streaming Decoder
- [x] `test_hdlc.cpp` -- Round-trip tests
- [x] `transport.h` + `serial_port.h/cpp` -- Cross-platform serial I/O
- [x] Minimal `main.cpp` -- open port, send loopback, print response

### Phase 2: Diag Commands (can parallel with Phase 3)
- [x] `commands.h/cpp` -- All command builders (loopback, LOG_CONFIG, SET_EVENT_MASK, EVENT_RPT, EXT_MSG_CFG)
- [x] `test_commands.cpp` -- Byte sequence validation
- [x] Synchronous send-and-receive helper (encode -> write -> read -> decode -> validate)

### Phase 3: CFG Parser (can parallel with Phase 2)
- [x] `cfg_parser.h/cpp` -- Parse CFG v1 format -> CfgMasks
- [x] `test_cfg_parser.cpp` -- Parse sample CFG file
- [x] Add CFG v2 support

### Phase 4: Ptrace Writer (can parallel with Phase 2 & 3)
- [x] `ptrace_writer.h/cpp` -- Binary record writing with FILETIME timestamps
- [x] `types.h` -- Shared platform macros and type aliases

### Phase 5: Diag Orchestration (requires Phases 1-4)
- [x] `diag_logger.h/cpp` -- Full connect/configure/log/shutdown workflow
- [x] CLI argument parsing in `main.cpp`
- [x] Signal handling (SIGINT/SIGTERM)
- [x] Receive thread with HDLC decode -> ptrace write
- [x] Duration-based auto-stop

### Phase 6: Polish (Diag)
- [x] `README.md`, `LICENSE`, `CMakeLists.txt` finalization
- [ ] Build verification (Windows MSVC)
- [ ] Build verification (Linux GCC)
- [ ] Progress output (packets/sec, bytes, CRC errors)
- [ ] Error handling edge cases

### Phase 7: Subsystem Command Framework (NEW -- foundation for QDSS/ADPL)
- [ ] `subsystem_commands.h/cpp` -- Generic subsystem command builder (cmd_id=0x4B)
  - [ ] `build_subsystem_command(subsys_id, subcmd, payload)` -> HDLC-ready packet
  - [ ] Response parser for subsystem command responses
- [ ] `test_subsystem_commands.cpp` -- Byte sequence validation

### Phase 8: QDSS Commands (NEW -- requires Phase 7)
- [ ] `qdss_commands.h/cpp` -- QDSS-specific subsystem command builders
  - [ ] HW acceleration query (subsys=0x12, cmd=548): query ATB/STM/LPASS_STM support
  - [ ] HW acceleration enable/disable (subsys=0x12, cmd=548): enable/disable per HW type
  - [ ] Transport query (subsys=0x12, cmd=547): query USB/PCIe/UART
  - [ ] Diag-over-STM enable/disable (subsys=0x12, cmd=532): route diag over STM
  - [ ] Trace sink config (subsys=0x5A, cmd=257): set sink to USB
  - [ ] Per-processor SW trace filters (subsys=0x5A, cmds=262/518/774/1286/2054/3334)
  - [ ] Per-processor entity filters (subsys=0x5A, cmds=264/520/776/1288/2056/3336)
  - [ ] Response parsers for HW accel query (extract supported types, DiagID masks, ATIDs)
- [ ] `test_qdss_commands.cpp` -- Byte sequence validation
- [ ] Integrate into DiagLogger: add `--qdss` flag, configure in start(), disable in stop()

### Phase 9: ADPL Commands (NEW -- requires Phase 7)
- [ ] `adpl_commands.h/cpp` -- ADPL-specific subsystem command builders
  - [ ] DPL enable (subsys=IPA, cmd=16384): version/packetLimit/bufferSize/dplCommand fields
  - [ ] DPL disable (subsys=IPA, cmd=16384): dplCommand=0, packetLimit=4
  - [ ] Enable response parser: extract negotiated version and status
  - [ ] Modem timestamp query (for V3+)
  - [ ] ADPL packet header decoder: auto-detect V1-V7, extract payload
    - V1/V2: 4-byte header, network byte order length
    - V3/V4: statusOpcode=0x10, 8-byte padding, timestamp in 100ms/ms
    - V5: statusOpcode=0x10, timestamp in us (dplTimeOfDay * 10)
    - V6: statusOpcode=0x0C/0x0D, 8-byte padding, extended timestamp
    - V7: statusOpcode=0x0C/0x0D, 16-byte padding, extended timestamp
- [ ] `test_adpl_commands.cpp` -- Byte sequence validation + header decode tests
- [ ] Integrate into DiagLogger: add `--adpl` flag, configure in start(), disable in stop()

### Phase 10: Integration & Testing (NEW -- requires Phases 8, 9)
- [ ] Update DiagLogger orchestrator to support combined diag + QDSS + ADPL
- [ ] Update CLI with --qdss, --adpl, --qdss-processors, --adpl-version flags
- [ ] Shutdown ordering: disable ADPL first, then QDSS, then diag masks
- [ ] Update README.md with QDSS/ADPL documentation
- [ ] End-to-end testing: diag-only, diag+QDSS, diag+ADPL, diag+QDSS+ADPL

### Dependency Graph
```
Phase 1 (HDLC + Serial)
   ├──> Phase 2 (Diag Commands)   ──┐
   ├──> Phase 3 (CFG Parser)      ──┼──> Phase 5 (Diag Orch.) ──> Phase 6 (Polish)
   └──> Phase 4 (Ptrace Writer)   ──┘        │
                                              │
Phase 7 (Subsystem Cmd Framework) ───────────┤
   ├──> Phase 8 (QDSS Commands)  ──┐         │
   └──> Phase 9 (ADPL Commands)  ──┼──> Phase 10 (Integration)
                                    │         │
                                    └─────────┘
                                     (requires Phase 5)
```

---

## Verification Plan

1. **Unit tests**: HDLC round-trip, CFG parsing, command byte validation, subsystem cmd bytes, QDSS cmd bytes, ADPL cmd bytes
2. **Loopback test**: Connect to device, send loopback, verify echo
3. **Diag mask test**: Send LOG_CONFIG -> verify ACK (cmd 0x73 echoed)
4. **QDSS test**: Send HW accel query -> verify response; enable STM -> verify ACK
5. **ADPL test**: Send DPL enable -> verify version/status response
6. **Diag-only capture**: `diaglogger --cfg test.cfg --port COM3 --duration 10`
7. **QDSS capture**: `diaglogger --cfg test.cfg --port COM3 --qdss --duration 10`
8. **ADPL capture**: `diaglogger --cfg test.cfg --port COM3 --adpl --duration 10`
9. **Combined**: `diaglogger --cfg test.cfg --port COM3 --qdss --adpl --duration 10`
10. **Cross-platform**: Build and run on both Windows (MSVC) and Linux (GCC)

---

## Files Inventory

### Existing (Implemented)

| File | Purpose | Status |
|---|---|---|
| `CMakeLists.txt` | Top-level CMake build | Done |
| `cmake/CompilerWarnings.cmake` | Shared compiler warning flags | Done |
| `include/diaglogger/types.h` | Shared types, platform macros | Done |
| `src/hdlc/crc16.h` | CRC-16-CCITT lookup table | Done |
| `src/hdlc/hdlc.h` | HDLC encoder/decoder API | Done |
| `src/hdlc/hdlc.cpp` | HDLC framing implementation | Done |
| `src/transport/transport.h` | Abstract transport interface | Done |
| `src/transport/serial_port.h` | Serial port header | Done |
| `src/transport/serial_port.cpp` | Windows + Linux serial impl | Done |
| `src/diag/commands.h` | Diag command builder API | Done |
| `src/diag/commands.cpp` | Command builder implementation | Done |
| `src/diag/cfg_parser.h` | CFG parser API | Done |
| `src/diag/cfg_parser.cpp` | CFG v1/v2 parser implementation | Done |
| `src/ptrace/ptrace_writer.h` | Ptrace writer API | Done |
| `src/ptrace/ptrace_writer.cpp` | Ptrace writer implementation | Done |
| `src/logger/diag_logger.h` | DiagLogger orchestrator API | Done |
| `src/logger/diag_logger.cpp` | DiagLogger orchestrator impl | Done |
| `src/main.cpp` | CLI entry point | Done |
| `tests/CMakeLists.txt` | Test build configuration | Done |
| `tests/test_hdlc.cpp` | HDLC unit tests | Done |
| `tests/test_commands.cpp` | Command builder unit tests | Done |
| `tests/test_cfg_parser.cpp` | CFG parser unit tests | Done |
| `tests/test_data/sample.cfg` | Test data placeholder | Done |
| `LICENSE` | Apache 2.0 | Done |
| `README.md` | Documentation | Done |

### New (To Be Implemented)

| File | Purpose | Phase |
|---|---|---|
| `src/diag/subsystem_commands.h` | Generic subsystem command builder API | 7 |
| `src/diag/subsystem_commands.cpp` | Subsystem command packet construction | 7 |
| `src/diag/qdss_commands.h` | QDSS subsystem command builder API | 8 |
| `src/diag/qdss_commands.cpp` | QDSS HW accel, STM, sink, filter cmds | 8 |
| `src/diag/adpl_commands.h` | ADPL subsystem command builder API | 9 |
| `src/diag/adpl_commands.cpp` | ADPL IPA enable/disable, header decoder | 9 |
| `tests/test_subsystem_commands.cpp` | Subsystem command byte validation | 7 |
| `tests/test_qdss_commands.cpp` | QDSS command byte validation | 8 |
| `tests/test_adpl_commands.cpp` | ADPL command + header decode tests | 9 |
| `src/transport/qcdevice_transport.h` | QcDevice USB transport header | 11 |
| `src/transport/qcdevice_transport.cpp` | QcDevice USB transport implementation | 11 |
| `src/transport/transport_factory.h` | Transport factory + device enumeration | 11 |
| `src/transport/transport_factory.cpp` | Transport factory implementation | 11 |

---

## Phase 11: QcDevice USB Transport (NEW -- enables real device communication)

### Context

DiagLogger's `SerialPort` class uses standard COM port APIs (`CreateFile`/`ReadFile` on Windows, `open`/`read` on Linux). Qualcomm diagnostic devices appear as USB bulk endpoints managed by the QcDevice/QDS driver library -- **not** as standard COM ports. This is why no data can be read from the serial port. The fix is to add `QcDeviceTransport` wrapping the QDS `QcDevice` API, following patterns proven in TUF's `Usb.h`/`Usb.cpp`.

**Decision**: Always link against QDS (no optional/fallback mode).

### Files to Create

#### 1. `src/transport/qcdevice_transport.h` -- QcDeviceTransport class

- Implements `Transport` interface
- Members: `HANDLE handle_`, `std::thread rx_worker_`, ring buffer (512KB, mutex + CV), `std::atomic<bool> running_`
- Ring buffer bridges QcDevice's blocking `ReadFromDevice` to DiagLogger's timeout-based `Transport::read()`
- Constants: `RX_BUFFER_SIZE = 128 * 1024`, `RING_BUFFER_SIZE = 512 * 1024`

#### 2. `src/transport/qcdevice_transport.cpp` -- Implementation

| Method | Behavior |
|--------|----------|
| `open()` | `QcDevice::OpenDevice(device_name, baud_rate)`, retry up to 8x with 250ms delay. On success, launch `rx_worker_` thread |
| `rx_worker` | Blocking loop: `ReadFromDevice(handle, 128KB buf)` → push into ring buffer under lock + notify CV. On failure: `running_ = false`, exit |
| `read(buf, max_len, timeout_ms)` | Lock mutex, CV wait with timeout for ring buffer data. Copy min(available, max_len) bytes out. Return 0 on timeout |
| `write(data, len)` | `QcDevice::SendToDevice(handle, data, len)`, retry up to 5x if 0 bytes sent |
| `close()` | Set `running_ = false`, `QcDevice::CloseDevice(handle)` (unblocks read), join thread, reset handle |

Windows: convert `std::string` device_name to `wchar_t*` for OpenDevice (QDS uses PVOID/PWCHAR on Windows).

#### 3. `src/transport/transport_factory.h/.cpp` -- Factory + device enumeration

- `create_transport(port_name, baud_rate)` → `SerialPort` if port_name starts with `COM` or `/dev/`, else `QcDeviceTransport`
- `enumerate_qcdevices()` → call `QcDevice::GetDeviceList()`, parse `DEV_PARAMS_N` array, filter `DEV_PROTOCOL_DIAG` (0x30)

### Files to Modify

#### 4. `CMakeLists.txt` -- Add QDS linkage

- Add `qcdevice_transport.cpp` and `transport_factory.cpp` to `TRANSPORT_SOURCES`
- Set `QDS_ROOT` cache variable (default: `../../QDS`)
- Add QDS include dirs and source files (`qcdev.cpp`, `utils.cpp`)
- Link `setupapi`, `cfgmgr32`, `shlwapi` on Windows

#### 5. `src/logger/diag_logger.cpp` (lines 20-28) -- Use factory

Replace hardcoded `SerialPort` creation with `create_transport(config_.port_name, config_.baud_rate)`.

#### 6. `src/main.cpp` -- Add `--list-devices` flag

- New `--list-devices` option: enumerate and print QcDevice diag devices, then exit
- Update `--port` help text to accept QcDevice descriptions
- Update `print_usage()` accordingly

### Key Design Decisions

| Decision | Choice | Rationale |
|----------|--------|-----------|
| Ring buffer vs callback | Ring buffer (512KB) | DiagLogger's `Transport::read()` is timeout-based pull; QcDevice is push (blocking). Ring buffer bridges them cleanly |
| Read buffer size | 128KB | Matches TUF `Usb.cpp` `BUFFER_READ_SIZE` |
| Send retries | 5 attempts | Matches TUF `MAX_SEND_RETRY` |
| Open retries | 8 attempts, 250ms delay | Matches TUF `RECONNECT_RETRY_COUNT` |
| QDS integration | Compile QDS sources into `diaglogger_lib` | Avoids DLL dependency |
| Port detection | Prefix-based (`COM`/`/dev/` = serial, else = QcDevice) | Simple, no ambiguity |

### Reference Files

- `C:\GitHub1\TUF\UTS\Source\Device\Communication\Usb.h` / `Usb.cpp` -- Proven QcDevice usage patterns
- `C:\GitHub1\QDS\QDS_WIN\qdpublic.h` -- QcDevice API declarations

### Updated Threading Model (with QcDeviceTransport)

```
┌─────────────────────┐       ┌───────────────────────┐       ┌──────────────────────┐
│    Main Thread       │       │   QcDevice RxWorker   │       │    Receive Thread     │
│                      │       │                       │       │                       │
│ 1. Parse CLI args    │       │  loop:                │       │  loop:                │
│ 2. Parse CFG file    │       │    ReadFromDevice()   │       │    transport.read()   │
│ 3. Open transport ───┼──────>│    → push to ring buf │──────>│    (pops from ring)   │
│ 4. Send handshake    │       │    → notify CV        │       │    hdlc.feed(buf)     │
│ 5. Send diag masks   │       │                       │       │    while frame=next() │
│ 6. Start rx thread ──┼───────┼───────────────────────┼──────>│      ptrace.write()   │
│ 7. Wait for signal   │       │  (exits on close)     │       │    check running_     │
│ 8. Stop rx thread    │       └───────────────────────┘       │                       │
│ 9. Send disables     │                                       │  (exits !running_)    │
│10. Close & summary   │                                       └──────────────────────┘
└─────────────────────┘
```

3 threads when using QcDeviceTransport (vs 2 with SerialPort). The RxWorker is internal to QcDeviceTransport.
