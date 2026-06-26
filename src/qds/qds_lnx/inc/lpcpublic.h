// Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
// SPDX-License-Identifier: BSD 3-Clause Clear License
#ifndef LPCPUB_H
#define LPCPUB_H

#include <stdio.h>
#include <errno.h>
//#include "pthread.h"
#include <pthread.h>

typedef long int HANDLE;
typedef int BOOL;
typedef unsigned long ULONG;
typedef unsigned int UINT;
typedef void *PVOID;
typedef void *LPVOID;
typedef unsigned long DWORD;
#define _stdcall
typedef ULONG *PULONG;
typedef UINT *PUINT;
typedef DWORD *LPDWORD;
typedef unsigned char UCHAR;
typedef char CHAR;
typedef unsigned short USHORT;
#define FALSE 0
#define TRUE 1

typedef void* HINSTANCE;
#define LoadLibrary dlopen
#define GetProcAddress dlsym
#define INVALID_HANDLE_VALUE -1


#define DBG(format, args...)    printf("%s:%d " format, __FUNCTION__, __LINE__, ##args)


#pragma once

#define QC_NAME_MAX 512

#pragma pack(push, 4)

typedef struct _DEVICE_CONTEXT
{
   //LIST_ENTRY List;
   ULONG  Cid;                       // Context ID
   CHAR   DevName[QC_NAME_MAX];      // for UI display
   CHAR   SymbolicName[QC_NAME_MAX]; // for opening device
   CHAR   OutFileName[QC_NAME_MAX];  // output file name
   HANDLE DevDepartureEvent;         // device removal event
   HANDLE ThHandle;                  // adapter-specific thread
   HANDLE DevHandle;
   ULONG  DevState; // 1 - arrival, 0 - departure
   ULONG totalBytesOut;
   ULONG totalBytesIn;
   ULONG current_rx_bytes;

   PVOID loopbk_data;
   PVOID receiveBuf;
   PVOID transferBuf;
   FILE *data_in_file;
   FILE *data_out_file;
   USHORT counter_out;
   USHORT counter_in;
   USHORT verification_failure;
   ULONG inject_failure;

} DEVICE_CONTEXT, *PDEVICE_CONTEXT;

typedef void (*TASK_CALLBACK)(PDEVICE_CONTEXT DevInfo);
typedef BOOL (*DEVAPI_INITIALIZE)(TASK_CALLBACK TaskCb);
typedef void (*DEVAPI_WAITANDFINISH)(void);
typedef ULONG (*DEVAPI_GETDEVICELIST)(PVOID Buffer, ULONG BufferSize, PULONG ActualSize);
typedef BOOL (*DEVAPI_READFROMDEVICE)(HANDLE, PVOID, DWORD, LPDWORD);
typedef BOOL (*DEVAPI_SENDTODEVICE)(HANDLE, PVOID, DWORD, LPDWORD);

#endif // LPCPUB_H
