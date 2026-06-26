// SPDX-License-Identifier: Apache-2.0
// Copyright 2024 Qualcomm Innovation Center, Inc.
//
// Single source of truth for the qualcomm-log-collector tool version.
// The CI/CD pipeline updates MAJOR/MINOR/PATCH at build time.

#ifndef DIAGLOGGER_VERSION_H
#define DIAGLOGGER_VERSION_H

#define STRINGIZE2(S) #S
#define STRINGIZE(S) STRINGIZE2(S)

#define MAJOR_VERSION 0
#define MINOR_VERSION 0
#define PATCH_VERSION 1

#define VERSION_STRING "v" STRINGIZE(MAJOR_VERSION) "." STRINGIZE(MINOR_VERSION) "." STRINGIZE(PATCH_VERSION)

namespace diaglogger {

constexpr const char* kToolVersion = VERSION_STRING;

} // namespace diaglogger

#endif // DIAGLOGGER_VERSION_H
