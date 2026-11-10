// Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
// SPDX-License-Identifier: BSD 3-Clause Clear License
#pragma once

// Shared visibility definitions for QLC's C and C++ public APIs.
// QLC_STATIC is used by the CLI and static-library consumers. QLC_DLL_EXPORTS
// is defined only while building the shared library itself.
#if defined(QLC_STATIC)
#  define QLC_API
#elif defined(_WIN32)
#  if defined(QLC_DLL_EXPORTS)
#    define QLC_API __declspec(dllexport)
#  else
#    define QLC_API __declspec(dllimport)
#  endif
#else
#  define QLC_API __attribute__((visibility("default")))
#endif

#define QLC_C_API QLC_API
