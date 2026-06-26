# QLC Metadata Format

The metadata header is written sequentially at the beginning of each capture file.
All multi-byte integers are little-endian.

## Metadata Header Fields

| # | Field | Length (bytes) | Description | Source Command |
|---|-------|----------------|-------------|----------------|
| 1 | HeaderLength | 4 | Total length of the metadata header | |
| 2 | Version | 6 | qlc version string, e.g. `v0.0.1` | |
| 3 | HDLCDataType | 1 | `0` = HDLC encoding removed | [\[1\]](#note-1-hdlc-datatype) |
|   |       |   | `1` = HDLC encoding enabled | |
| 4 | GUIDListEntryCount | 4 | Number of GUIDs that follow | |
| 5 | GUID\[GUIDListEntryCount\] | 16 \* count | Array of available GUIDs | Qshrink FILE\_LIST [\[5\]](#note-5-qshrink-file_list-command) |
| 6 | DiagIDEntryCount | 4 | Number of DiagID entries that follow | |
| 7 | (DiagID, DiagID string, GUID) \[DiagIDEntryCount\] | 47 \* count | DiagID (1) + name (30) + GUID (16) per entry | DiagID Table Query + Qshrink FILE\_LIST [\[2\]](#note-2-diagid-diagid-string-guid) |
| 8 | KeyCount | 1 | Number of security key entries that follow | |
| 9 | KeyInfo\[KeyCount\] | variable | Version(1) + Classifier(1) + PublicType(1) + Reserved(1) + PublicLen(2) + WrappedLen(2) + PublicKey + WrappedKey | SECURITY\_KEY\_LIST [\[6\]](#note-6-security_key_list-command) |
| 10 | ATID Entry Count | 2 | Number of HW accel ATID entries that follow | |
| 11 | (HW ACCEL TYPE, ATID) \[ATIDEntryCount\] | 2 \* count | HW Accel Type (1) + ATID Assigned (1) per entry | HW\_ACCELERATION [\[3\]](#note-3-hw_acceleration-response-format) |
| 12 | DeviceInfoLength | 4 | Length of the DeviceInfo string that follows | |
| 13 | DeviceInfo | DeviceInfoLength | Device information strings | 4 diag commands [\[4\]](#note-4-device-info-diag-commands) |

---

## Notes

### Note 1: HDLC DataType

HDLC mode negotiation (for regular diag protocol):

- The tool tracks `HdlcModeStatus` with states: `NOT_SET`, `ENABLE`, `DISABLE`
- During connection setup, the tool checks the device's HDLC capability
- If the device supports non-HDLC mode and the tool requests it, HDLC is disabled -> dataType = `0`
- If the device only supports HDLC or negotiation keeps HDLC -> dataType = `1`
- An HDLC loopback test is used to verify the device is in the expected mode

### Note 2: DiagID, DiagID String, GUID

DiagID is the primary key. Two commands are combined:

1. **Diag ID Table Query** (subsystem `0x12`, subcmd `0x222` / 546) -- returns `DiagID -> process name`
2. **QShrink FILE\_LIST** (see [\[5\]](#note-5-qshrink-file_list-command)) -- returns `DiagID -> GUID`

The tool joins both responses on DiagID to form each `<DiagID, DiagID String, GUID>` entry.

### Note 3: HW\_ACCELERATION Response Format

**Command:** `0x4B`, Subsystem `0x12` (DIAG\_SERV), Command `548` (0x0224)

**Request:** `[1 byte] operation=QUERY(2), [1 byte] hw_type=ALL(0)`

**Version 1 (NO\_DYNAMIC\_ATID):**

```
[1] status
[1] num_types
per type: [1] hw_type, [1] supported, [4] diagid_mask (LE)
```

Uses default ATIDs: STM=16, ATB=50, LPASS\_STM=25.

**Version 2 (DYNAMIC\_ATID):**

```
... same as v1, plus:
[1] response_version
[1] num_atid_entries
per entry: [1] hw_type, [1] atid    <- dynamic ATID overrides
```

### Note 4: Device Info Diag Commands

Four commands are used to gather device information:

1. **Version Info** -- Command `0x00` (VERNO)
   - Request: single byte `0x00`
   - Returns: compile date/time, release date/time, MSM version, firmware revision

2. **Extended Build ID** -- Command `0x9F` (159)
   - Request: single byte `0x9F`
   - Returns: MSM revision, model number, software revision string, model name string
   - This is the primary device info command

3. **ESN** -- Command `0x01`
   - Request: single byte `0x01`
   - Returns: 4-byte Electronic Serial Number

4. **Status** -- Command `0x0C`
   - Request: single byte `0x0C`
   - Returns: DMSS status information

### Note 5: QShrink FILE\_LIST Command

- Cmd: `0x80`, SubSystem: `0x12`, Subcmd (per peripheral):
  - APPS = 527 (0x020F)
  - MPSS = 2070 (0x0816)
  - ADSP = 3600 (0x0E10)
  - WCNSS = 5151 (0x141F)
  - SLPI = 6680 (0x1A18)
- Operation: `FILE_LIST = 0`

### Note 6: SECURITY\_KEY\_LIST Command

- Cmd: `0x80`, Subsystem `0x12`, SubCmd `0x0833` (2099)
- Operation: `OP_CODE_RETRIEVE`
- Key entry size: `8 + PublicLen + WrappedLen` bytes
- The layout above reflects version 1 of the KeyInfo field
