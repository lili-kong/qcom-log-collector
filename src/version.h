// Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
// SPDX-License-Identifier: BSD 3-Clause Clear License
//
// Single source of truth for the qualcomm-log-collector tool version.
// The CI/CD pipeline updates MAJOR/MINOR/PATCH at build time.

#ifndef QC_VERSION_H
#define QC_VERSION_H

#define STRINGIZE2(S) #S
#define STRINGIZE(S) STRINGIZE2(S)

#define MAJOR_VERSION 1
#define MINOR_VERSION 2
#define PATCH_VERSION 2

#define VERSION_STRING "v" STRINGIZE(MAJOR_VERSION) "." STRINGIZE(MINOR_VERSION) "." STRINGIZE(PATCH_VERSION)

namespace QC {

constexpr const char* kToolVersion = VERSION_STRING;

} // namespace QC

#endif // QC_VERSION_H
