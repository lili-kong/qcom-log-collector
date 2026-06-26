# CFG File Formats & Parser

This document explains the two Qualcomm diagnostic configuration file formats (`.cfg` and `.cfg2`) and how `cfg_parser` processes them.

## Overview

Qualcomm diagnostic tools use binary configuration files to define which diag masks, log codes, events, and message levels to enable on a device. These files contain pre-built diag command payloads wrapped in a framing format. The tool supports two framing formats:

| | `.cfg` (HDLC) | `.cfg2` (Non-HDLC) |
|---|---|---|
| **Framing** | CRC-16 + byte-stuffing | Length-prefixed |
| **Delimiter** | `0x7E` | `0x7E` |
| **Checksum** | CRC-16-CCITT (2 bytes) | None |
| **Byte-stuffing** | Yes | No |
| **Header per frame** | None | Version (1B) + Length (2B) |

Both formats store the same kind of payload -- raw diag command bytes. The only difference is how frames are delimited and validated.

---

## `.cfg` Format (HDLC-Framed)

Each command is HDLC-encoded, the same framing used on-wire for classic diag communication.

### Frame structure

```
... [payload][CRC-16 LE] (with byte-stuffing applied) [0x7E] ...
```

- **Payload**: Raw diag command bytes.
- **CRC-16**: CCITT CRC-16 computed over the payload, appended in little-endian order.
- **Byte-stuffing**: Any `0x7E` or `0x7D` byte within the payload+CRC region is escaped:
  - `0x7E` becomes `0x7D 0x5E`
  - `0x7D` becomes `0x7D 0x5D`
- **Delimiter**: `0x7E` marks frame boundaries. The trailing flag of one frame is the implicit leading flag of the next.

### Decoding process

1. Accumulate bytes until a `0x7E` flag is seen.
2. Reverse byte-stuffing (un-escape `0x7D` sequences).
3. Verify CRC-16 over the un-escaped data.
4. Strip the 2-byte CRC; the remainder is the payload.
5. Frames with CRC errors are silently discarded.

---

## `.cfg2` Format (Non-HDLC / Length-Prefixed)

A simpler framing designed to avoid the overhead of CRC computation and byte-stuffing.

### Frame structure

```
[0x7E][0x01][len_lo][len_hi][payload ...][0x7E]
```

| Field | Size | Description |
|---|---|---|
| Start flag | 1 byte | `0x7E` |
| Version | 1 byte | `0x01` (Non-HDLC v1) |
| Length | 2 bytes (LE) | Payload length (excludes header and end flag) |
| Payload | N bytes | Raw diag command bytes |
| End flag | 1 byte | `0x7E` |

### Decoding process

A 6-state machine:

```
WAIT_FLAG -> WAIT_VERSION -> WAIT_LEN_LO -> WAIT_LEN_HI -> READING_PAYLOAD -> WAIT_END_FLAG
```

1. Wait for `0x7E` start flag.
2. Read and validate version byte (must be `0x01`).
3. Read 2-byte length (little-endian).
4. Read exactly *length* payload bytes.
5. Expect trailing `0x7E`.
6. On any error, resynchronize by scanning for the next `0x7E`.

No CRC check or byte-stuffing reversal is needed.

---

## How `cfg_parser` Works

Source files:
- `src/diag/cfg_parser.h` -- Public API
- `src/diag/cfg_parser.cpp` -- Implementation

### API

```cpp
enum class CfgVersion { Unknown, Hdlc, NonHdlc };

// Detect format from file extension (.cfg -> Hdlc, .cfg2 -> NonHdlc)
CfgVersion detect_cfg_version(const ByteBuffer& data, const std::string& path);

// Parse a CFG file and return decoded command payloads
std::vector<ByteBuffer> parse_cfg_file(const std::string& path);
```

### Parsing flow

```
parse_cfg_file(path)
  |
  |-- Read entire file into memory (binary mode)
  |-- detect_cfg_version() based on file extension (case-insensitive)
  |
  |-- .cfg  --> parse_cfg_stream<HdlcDecoder>(data)
  |-- .cfg2 --> parse_cfg_stream<NonHdlcDecoder>(data)
  |-- other --> throw runtime_error
  |
  +-- Return std::vector<ByteBuffer> of raw command payloads
```

`parse_cfg_stream` is a generic template that works with any decoder exposing `feed()` and `next_frame()`:

```cpp
template<typename Decoder>
static std::vector<ByteBuffer> parse_cfg_stream(const ByteBuffer& data, ...) {
    Decoder decoder;
    decoder.feed(data.data(), data.size());
    std::vector<ByteBuffer> commands;
    while (auto frame = decoder.next_frame()) {
        commands.push_back(std::move(*frame));
    }
    // throw if empty
    return commands;
}
```

### Format detection

Detection is **extension-based only** (not content-based):

| Extension | Result |
|---|---|
| `.cfg` (case-insensitive) | `CfgVersion::Hdlc` |
| `.cfg2` (case-insensitive) | `CfgVersion::NonHdlc` |
| Anything else | `CfgVersion::Unknown` (throws) |

### Error handling

| Condition | Behavior |
|---|---|
| File not found | `runtime_error`: "Cannot open CFG file: ..." |
| Empty file | `runtime_error`: "CFG file is empty: ..." |
| Unknown extension | `runtime_error` with guidance on expected extensions |
| No valid frames decoded | `runtime_error`: "No valid frames found..." |

---

## Usage in DiagLogger

The orchestrator (`src/logger/diag_logger.cpp`) calls `parse_cfg_file()` early in `start()`:

```cpp
cfg_commands_ = parse_cfg_file(config_.cfg_path);
```

Later, each decoded payload is sent to the device:

```cpp
for (const auto& cmd : cfg_commands_) {
    send_command(cmd);  // applies HDLC or Non-HDLC framing based on current mode
}
```

Key point: the parser always returns **raw payloads** (no framing). The appropriate framing (HDLC or Non-HDLC) is applied at send time by `send_command()`, based on the transport's current framing mode -- which may differ from the file's framing format.

---

## Creating CFG Files

CFG/CFG2 files are binary files generated by QXDM.

### Steps to generate a CFG file in QXDM

1. Start QXDM from the start menu.
2. Go to **Tools -> CFG File Generator**.
3. Select the relevant masks to capture (e.g., Event Reports, Log Packets, Message Packets) and configure the specific categories you need.
4. In the **SD Logging** dropdown at the bottom of the CFG File Generator window:
   - Select **"Save Diag masks for Stream 1"** to generate a `.cfg` file (HDLC mode).
   - Select **"Save Diag masks for Stream 2"** to generate a `.cfg2` file (Non-HDLC mode).
5. Choose a save location, name the file (e.g., `Diag.cfg`), and click Save.
6. Pass the saved file to this tool via the `--cfg` CLI flag.
