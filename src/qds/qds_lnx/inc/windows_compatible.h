// Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
// SPDX-License-Identifier: BSD 3-Clause Clear License
#ifndef LNX_WINDOWS_COMPATIBLE_H
#define LNX_WINDOWS_COMPATIBLE_H

#include <stdio.h>
#include <errno.h>

typedef long int HANDLE;
typedef int BOOL;
typedef unsigned long long ULONGLONG;
typedef ULONGLONG *PULONGLONG;
typedef char *PCHAR;
typedef unsigned long ULONG;
typedef void *PVOID;
typedef void *LPVOID;
typedef wchar_t *PWCHAR;
typedef unsigned long DWORD;
typedef char* PTSTR;
#define _stdcall
#define _cdecl
typedef ULONG *PULONG;
typedef DWORD *LPDWORD;
typedef unsigned char UCHAR;
typedef char CHAR;
typedef UCHAR *PUCHAR;
typedef unsigned short USHORT;
typedef int                 INT;
typedef unsigned int        UINT;
typedef unsigned int        *PUINT;
#define FALSE 0
#define TRUE 1
#define WINAPI
#define IN
#define OUT

typedef void* HINSTANCE;
#define LoadLibrary dlopen
#define GetProcAddress dlsym
#define TEXT
/////
#define QCDEVLIB_API
typedef long LONG;
#define NO_ERROR 0
#define FORCEINLINE inline
typedef void VOID;
typedef bool BOOLEAN;
#define INVALID_HANDLE_VALUE -1
/////

FORCEINLINE
DWORD GetLastError(void)
{
   perror("Status:");
   return errno;
}

FORCEINLINE
LONG InterlockedExchange(LONG* Target, LONG Value)
{
   LONG ret;
   ret = *Target;
   *Target = Value;
   return ret;
}

#endif
