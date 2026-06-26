// Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
// SPDX-License-Identifier: BSD 3-Clause Clear License
#ifndef DEVICE_CALLBACKS_H
#define DEVICE_CALLBACKS_H

namespace AlpacaQuts
{
#pragma pack(4)

#ifdef TOOLS_TARGET_WINDOWS
   struct CB_PARAMS
   {
      wchar_t* DevDesc;   // device description for display in unicode
      wchar_t* DevName;   // device name for I/O (normally the symbolic name for open/close)
      wchar_t* IfName;    // interface name for network adapter
      wchar_t* Loc;       // device's location on its bus
      wchar_t* DevPath;   // device's connection path
      wchar_t* SerNum;    // serial number from device
      wchar_t* SerNumMsm; // serial number from device
      unsigned long  Mtu;       // MTU for network connection (data call)
      unsigned long  Flag;      // flag to indicate device type, state, and availability of QC driver support
      unsigned long  Protocol;  // protocol code (low 8 bits) of the USB function, 0 means unknown protocol
      wchar_t* HwId;      // device hardware ID
      wchar_t* ParentDev; // name of parent device if present
      wchat_t* ParentLocationInfomation; // physical location of parent device if present
   };
#else
   struct CB_PARAMS
   {
      char* DevDesc;   // device description for display in unicode
      char* DevName;   // device name for I/O (normally the symbolic name for open/close)
      char* IfName;    // interface name for network adapter
      char* Loc;       // device's location on its bus
      char* DevPath;   // device's connection path
      char* SerNum;    // serial number from device
      char* SerNumMsm; // serial number from device
      unsigned long  Mtu;       // MTU for network connection (data call)
      unsigned long  Flag;      // flag to indicate device type, state, and availability of QC driver support
      unsigned long  Protocol;  // protocol code (low 8 bits) of the USB function, 0 means unknown protocol
      char* HwId;      // device hardware ID
      char* ParentDev; // name of parent device if present
      char* ParentLocationInfomation; // physical location of parent device if present
   };
#endif
#pragma pack()

   // Device change callback
   // Context can be set by the callee during arrival to track departure,
   //    meaning caller to use the same Context when departure happens

#ifdef TOOLS_TARGET_WINDOWS
   typedef void (__stdcall *DEVICECHANGE_CALLBACK)(CB_PARAMS* CbParams, void** context);
   typedef void (__stdcall *QCD_LOGGING_CALLBACK)(int Level, char* Message);  // NULL-terminated ANSI string
#else
   typedef void (*DEVICECHANGE_CALLBACK)(CB_PARAMS* CbParams, void** context);
   typedef void (*QCD_LOGGING_CALLBACK)(int Level, char* Message);  // NULL-terminated ANSI string
#endif
}

#endif
