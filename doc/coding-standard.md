# Qualcomm Log Collector — Coding Standard

Conventions observed and enforced across the codebase.

## Language & Build

- **C++17** (`CMAKE_CXX_STANDARD 17`, `/std:c++17`)
- Visual Studio 2019 (v142) on Windows, GCC/Clang on Linux
- Platform macros: `TOOLS_TARGET_WINDOWS`, `TOOLS_TARGET_LINUX`, `TOOLS_TARGET_OSX`

## File Layout

| Item | Convention |
|------|-----------|
| File names | `snake_case.cpp`, `snake_case.h` |
| License header | `// SPDX-License-Identifier: Apache-2.0` on first line |
| Copyright | `// Copyright 2024 Qualcomm Innovation Center, Inc.` |
| Include guards | `#ifndef QC_<FILENAME>_H` / `#define` / `#endif` (no `#pragma once`) |
| Include order | System headers first, then project headers |
| Namespace | All code in `namespace diaglogger { }` |

## Naming

| Item | Style | Example |
|------|-------|---------|
| Classes / Structs | PascalCase | `HdlcDecoder`, `DiagLogger`, `PtraceMetadata` |
| Enum types | `enum class`, PascalCase | `enum class Direction : uint8_t { TX, RX }` |
| Enum values | PascalCase or UPPER_CASE | `TransportState::CONNECTED`, `CfgVersion::HdlcStream` |
| Functions / Methods | snake_case | `hdlc_encode()`, `is_open()`, `parse_cfg_file()` |
| Member variables | snake_case with trailing `_` | `device_name_`, `ring_head_`, `running_` |
| Struct data members | snake_case, **no** trailing `_` | `ssid_start`, `app_name`, `baud_rate` |
| Local variables | snake_case | `frame_size`, `header_offset` |
| Parameters | snake_case | `device_name`, `timeout_ms` |
| Constants | UPPER_CASE in namespaces or `constexpr` | `HDLC_FLAG`, `RX_BUFFER_SIZE` |
| Type aliases | PascalCase | `using ByteBuffer = std::vector<uint8_t>;` |
| Namespaces | PascalCase for constant groups | `DiagCmd::STATUS`, `LogConfigOp::SET_MASK` |

## Formatting

- **4 spaces** indentation (no tabs)
- Opening brace on same line as statement (K&R / "Allman-ish")
- Always use braces for `if`, `for`, `while` — even single-line bodies
- `switch` cases at same indent as `switch`, body indented one level

```cpp
if (frame_size > 0) {
    total_sent += frame_size;
}

switch (state_) {
case State::WAIT_FLAG:
    if (byte == NON_HDLC_FLAG) {
        state_ = State::WAIT_VERSION;
    }
    break;
}
```

## Comments

- `//` for single-line and multi-line comments
- `///` for function documentation in headers
- No Doxygen-style `@param` / `@return` — keep docs brief and inline
- Don't comment obvious code; do comment protocol formats and non-obvious logic

```cpp
/// Encode a raw diag payload into an HDLC frame.
/// Appends CRC-16, escapes special bytes, wraps in 0x7E delimiters.
ByteBuffer hdlc_encode(const uint8_t* data, size_t len);

// Format: [0x7E][version=0x01][len_lo][len_hi][payload...][0x7E]
```

## Classes & Constructors

- Mark single-argument constructors `explicit`
- Use member initializer lists; one member per line with leading comma
- Delete copy/move for non-copyable classes
- Default constructors: `= default` when possible

```cpp
explicit QcDeviceTransport(const std::string& device_name,
                           uint32_t baud_rate = 38400);

QcDeviceTransport::QcDeviceTransport(const std::string& device_name,
                                     uint32_t baud_rate)
    : device_name_(device_name)
    , baud_rate_(baud_rate)
    , ring_(RING_BUFFER_SIZE)
{
}
```

## Error Handling

| Situation | Approach |
|-----------|----------|
| Parse/config failures | `throw std::runtime_error("message")` |
| I/O operations | Return `bool` (success/failure) or `size_t` (0 = error) |
| Optional results | `std::optional<T>` |
| Warnings / diagnostics | `std::cerr <<` |

```cpp
if (!file.is_open()) {
    throw std::runtime_error("Cannot open CFG file: " + path);
}

std::optional<ByteBuffer> frame = decoder.next_frame();
if (frame.has_value()) {
    process(*frame);
}
```

## Const & References

- Const-qualify methods that don't modify state: `bool is_open() const`
- Pass large types by `const&`: `const ByteBuffer& data`
- Pass arrays by pointer + size: `const uint8_t* data, size_t len`
- Output via return value (preferred) or pointer parameter

## Smart Pointers & Ownership

- `std::unique_ptr<T>` for exclusive ownership
- No `std::shared_ptr` in the codebase
- Raw pointers only for non-owning / C API interop
- Use `std::move()` for transferring ownership

```cpp
std::unique_ptr<Transport> transport_;
transport_ = create_transport(config_.port_name, config_.baud_rate);
commands.push_back(std::move(pkt));
```

## Threading & Atomics

- `std::atomic<T>` for shared flags and counters
- `std::mutex` + `std::lock_guard` / `std::unique_lock` for critical sections
- `std::condition_variable` for producer/consumer signaling
- Explicit memory ordering when needed: `std::memory_order_relaxed`

```cpp
std::atomic<bool> running_{false};
std::atomic<TransportState> state_{TransportState::CLOSED};
record_count_.fetch_add(1, std::memory_order_relaxed);
```

## `auto` Keyword

Use `auto` for:
- Complex return types (`auto frame = decoder.next_frame();`)
- Iterators and range-for (`for (const auto& dev : devices)`)
- Chrono types (`auto now = std::chrono::system_clock::now();`)

Don't use `auto` for:
- Plain types where the type is informative (`int`, `size_t`, `bool`, `uint8_t`)

## Constants

- `constexpr` for compile-time constants
- Group related constants in nested namespaces or as `static constexpr` class members
- No `#define` for constants
- Avoid hard-coded numbers in source code — use named constants instead

```cpp
constexpr uint8_t HDLC_FLAG = 0x7E;

namespace DiagCmd {
    constexpr uint8_t STATUS    = 0x0C;
    constexpr uint8_t LOOPBACK  = 0x7B;
}

class QcDeviceTransport {
    static constexpr size_t RX_BUFFER_SIZE = 128 * 1024;
};
```

## Configuration

- Use structs with default member values for config bundles
- Prefer config structs over long parameter lists

```cpp
struct DiagLoggerConfig {
    std::string cfg_path;
    std::string port_name;
    uint32_t baud_rate = 115200;
    bool verbose = false;
};

explicit DiagLogger(const DiagLoggerConfig& config);
```

## Platform-Specific Code

- Use `#ifdef TOOLS_TARGET_WINDOWS` / `#else` blocks
- Keep platform differences localized (in transport layer, not scattered)

## Tests

- File naming: `test_<module>.cpp`
- Lightweight custom test framework (macro-based registration, `ASSERT_EQ` / `ASSERT_TRUE`)
- Tests auto-register via static constructors
- Test patterns: round-trip, streaming byte-by-byte, edge cases, file I/O with temp files

## Things We Don't Do

- No `#pragma once` — use include guards
- No `std::string_view` (not adopted yet)
- No `std::shared_ptr`
- No Doxygen / heavyweight doc comments
- No `using namespace std;`
- No C-style casts — use `static_cast<>`, `reinterpret_cast<>`
- No macros for constants — use `constexpr`
