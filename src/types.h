// Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
// SPDX-License-Identifier: BSD 3-Clause Clear License

#ifndef QC_TYPES_H
#define QC_TYPES_H

#include <cstdint>
#include <cstddef>
#include <vector>
#include <string>

namespace QC {

using ByteBuffer = std::vector<uint8_t>;

enum class Direction : uint8_t {
    TX = 0x00,
    RX = 0x01
};

enum class TransportState { CLOSED, CONNECTED, DISCONNECTED };

// Platform detection
#if defined(_WIN32) || defined(_WIN64)
    #ifndef TOOLS_TARGET_WINDOWS
        #define TOOLS_TARGET_WINDOWS
    #endif
#elif defined(__linux__)
    #ifndef TOOLS_TARGET_LINUX
        #define TOOLS_TARGET_LINUX
    #endif
#elif defined(__APPLE__)
    #ifndef TOOLS_TARGET_OSX
        #define TOOLS_TARGET_OSX
    #endif
#endif

} // namespace QC

#endif // QC_TYPES_H
