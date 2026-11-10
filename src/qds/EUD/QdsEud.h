// Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
// SPDX-License-Identifier: BSD 3-Clause Clear License

#ifndef QDSEUD_H
#define QDSEUD_H

#ifdef _WIN32
#  include <windows.h>
#else
   /* On Linux, Windows-compatible types (BOOL, HANDLE, PVOID, DWORD,
    * LPDWORD, VOID) are provided by windows_compatible.h, which is
    * pulled in via qdpublic.h in the Linux build. */
#  include "windows_compatible.h"
#endif
#include <stdint.h>
#include <unordered_map>
#include <string>

/* -----------------------------------------------------------------------
 * EUD USB Monitor types
 * These mirror the definitions in eud/inc/usb_monitor.h so that QDS does
 * not need a hard include-path dependency on the EUD source tree.
 * ----------------------------------------------------------------------- */
#define EUD_MAX_DEVICE_NAME 128
#define EUD_MAX_SERIAL_ID    64

/* Peripheral type identifiers */
#define EUD_PERIPH_TYPE_CTL  1
#define EUD_PERIPH_TYPE_TRC  2
#define EUD_PERIPH_TYPE_SWD  3
#define EUD_PERIPH_TYPE_JTG  4
#define EUD_PERIPH_TYPE_COM  5

/**
 * @brief Device information passed to the USB monitor callback.
 *        flag == 1 → device arrived, flag == 0 → device left.
 */
typedef struct _EUD_DEVICE_INFO
{
    char     device_name[EUD_MAX_DEVICE_NAME]; /* e.g. "Qualcomm EUD Control Device" */
    char     serial_id[EUD_MAX_SERIAL_ID];     /* e.g. "402DE978"                    */
    uint32_t device_id;
    uint8_t  peripheral_type;                  /* EUD_PERIPH_TYPE_* above             */
    uint8_t  flag;                             /* 1 = arrived, 0 = departed           */
} EUD_DEVICE_INFO;

/** Handle values assigned to EUD CTL and TRC peripherals. */
#define QC_EUD_CTL_HANDLE  ((HANDLE)(intptr_t)0x1)
#define QC_EUD_TRC_HANDLE  ((HANDLE)(intptr_t)0x2)

/** Callback prototype used by usb_monitor_init / QcEud_StartMonitor. */
typedef void (*usb_monitor_cb_t)(const EUD_DEVICE_INFO *dev_info);

/**
 * @brief EUD logging callback type.
 *        Sends eud logs to QDS
 */
typedef void (*eud_log_cb_t)(const char* message);

#ifndef _WIN32
/*
 * @brief Returns the platform/architecture-specific path to the EUD shared
 *        library (libeud.so).
 *
 *        Returns the arm64 path when running on an aarch64 host, otherwise
 *        returns the x64 path.
 */
const char* GetEudLibPath(void);
#endif /* !_WIN32 */

/**
 * @brief Convert an EUD device_id to the SerNumMsm string.
 *
 * EUD.dll/so stores device_id in little-endian byte order.  Reversing the
 * four bytes produces the same hex string that the host OS reports as the
 * device serial number (QCDeviceMsmSerialNumber on Windows, _SN: field on
 * Linux).  The result is written as an 8-digit uppercase hex string into
 * serNumMsm (caller must supply at least 9 bytes).
 */
void ConvertDeviceIdToSerNumMsm(uint32_t device_id, char* serNumMsm, size_t bufSize);


typedef int32_t EUD_ERR_t;
#define EUD_SUCCESS          0
#define EUD_GENERIC_FAILURE  (-1)

/*
 * QcEud_LoadLib
 *   Loads EUD.dll (Windows) / EUD.so (Linux) and resolves all required
 *   exports.  Call this once at startup (e.g. after initialize_libusb).
 *   Returns TRUE on success, FALSE if the library cannot be found/loaded.
 */
BOOL      QcEud_LoadLib(void);

/*
 * QcEud_Initialize
 *   Retrieves the device ID, initialises the EUD trace peripheral, and
 *   calls eud_trace_device_ready to signal readiness.
 *   QcEud_LoadLib() must have been called successfully before this.
 *   Returns TRUE on success, FALSE if init fails.
 */
BOOL      QcEud_Initialize(void);

/*
 * QcEud_TraceDeviceReady
 *   Calls eud_trace_device_ready directly (outside of QcEud_Initialize).
 *   Returns EUD_SUCCESS (0) on success, non-zero error code on failure.
 */
EUD_ERR_t QcEud_TraceDeviceReady(void);

/*
 * QcEud_CloseDevice
 *   Closes the EUD trace peripheral.
 */
VOID      QcEud_CloseDevice(void);

/*
 * QcEud_ReadTrace
 *   Reads trace data from the EUD trace peripheral into RxBuffer.
 *   Returns TRUE on success.
 */
BOOL      QcEud_ReadTrace(PVOID RxBuffer, DWORD NumBytesToRead, LPDWORD NumBytesReturned);

/*
 * QcEud_SpoofUsbAttach
 *   Signals a USB VBUS attach event to the target device.
 */
EUD_ERR_t QcEud_SpoofUsbAttach(void);

/*
 * QcEud_SpoofUsbDetach
 *   Signals a USB VBUS detach event to the target device.
 */
EUD_ERR_t QcEud_SpoofUsbDetach(void);

/*
 * QcEud_OpenDevice
 *   Associates a device name with an open device handle in the internal map.
 *   Returns the same Handle passed in.
 */
HANDLE    QcEud_OpenDevice(PVOID DeviceName, HANDLE Handle);

/*
 * QcEud_FindHandle
 *   Returns TRUE if the given Handle exists in the internal device map.
 */
BOOL      QcEud_FindHandle(HANDLE Handle);

/*
 * QcEud_UnregisterHandle
 *   Removes the given Handle from the internal device map.
 */
VOID      QcEud_UnregisterHandle(HANDLE Handle);

/*
 * QcEud_RemoveByName
 *   Removes the map entry whose key matches DeviceName.
 *   Use this instead of directly accessing g_EudHandleMap on device departure.
 */
VOID      QcEud_RemoveByName(PVOID DeviceName);

/*
 * QcEud_ResetDeviceId
 *   Resets the cached EUD device ID to 0 so that the next QcEud_Initialize()
 *   call re-queries get_device_id_array.  Call this from the EUD CTL departure
 *   path so that a newly connected device is correctly identified even if the
 *   previous device was never opened by the application.
 */
VOID      QcEud_ResetDeviceId(void);

/*
 * QcEud_Cleanup
 *   Clears the internal device handle map.  Call this from StopDeviceMonitor
 *   so that stale handles do not persist across monitor restarts.
 */
VOID      QcEud_Cleanup(void);

/*
 * QcEud_StartMonitor
 *   Calls usb_monitor_init(cb, 0) from EUD.dll to start the background
 *   polling thread.  The callback is invoked (from EUD's thread) whenever
 *   an EUD peripheral (CTL / TRC / SWD / JTG / COM) arrives or departs.
 *   QcEud_LoadLib() must have been called successfully first.
 *   Returns TRUE on success, FALSE if the symbol is unavailable or init fails.
 */
BOOL      QcEud_StartMonitor(usb_monitor_cb_t cb);

/*
 * QcEud_StopMonitor
 *   Calls usb_monitor_deinit() from EUD.dll.
 *   Signals the polling thread to stop and waits for it to join before
 *   returning — no further callbacks will fire after this returns.
 *   Must be called before StopDeviceMonitor() tears down the queue/lock.
 */
VOID      QcEud_StopMonitor(void);

#endif /* QDSEUD_H */
