// SPDX-License-Identifier: Apache-2.0
// Copyright 2024 Qualcomm Innovation Center, Inc.

#ifndef DIAGLOGGER_TYPES_H
#define DIAGLOGGER_TYPES_H

#include <cstdint>
#include <cstddef>
#include <vector>
#include <string>

namespace diaglogger {

using ByteBuffer = std::vector<uint8_t>;

enum class Direction : uint8_t {
    TX = 0x00,
    RX = 0x01
};

enum class TransportState { CLOSED, CONNECTED, DISCONNECTED };

// Platform detection
#if defined(_WIN32) || defined(_WIN64)
    #define TOOLS_TARGET_WINDOWS 1
#elif defined(__linux__)
    #define TOOLS_TARGET_LINUX 1
#elif defined(__APPLE__)
    #define TOOLS_TARGET_OSX 1
#endif

} // namespace diaglogger

#endif // DIAGLOGGER_TYPES_H
