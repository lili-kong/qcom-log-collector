// Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
// SPDX-License-Identifier: BSD 3-Clause Clear License

#include "QdsEud.h"
#include <iostream>
#include <unordered_map>
#include <string>

#ifdef _WIN32
#  include <windows.h>
#else
#  include <dlfcn.h>
#  include <sys/utsname.h>  /* uname() for architecture detection */
#  include <string.h>       /* strncmp */
#  include "qdutils.h"      /* QCDEV_LOG_* macros for Linux */
#endif

#include "../common/utils.h"

using namespace std;
using namespace Utils;

//===----------------------------------------------------------------------===//
// Platform-specific library handle type, helpers, and logging macros
//===----------------------------------------------------------------------===//

#ifdef _WIN32
typedef HMODULE  EudLibHandle;
#  define EUD_LOAD_LIB(path)        LoadLibraryExA(path, NULL, LOAD_WITH_ALTERED_SEARCH_PATH)
#  define EUD_GET_SYM(h, name)      GetProcAddress(h, name)
#  define EUD_FREE_LIB(h)           FreeLibrary(h)
#  define EUD_LIB_NULL              NULL
/* GetEudLibPath() detects the native machine architecture at runtime
 * and returns the matching path. */
static const char* k_EudLibFullPath_x64   =
    "C:\\Program Files (x86)\\Qualcomm\\Qualcomm_EUD_Trace\\x64\\EUD.dll";
static const char* k_EudLibFullPath_arm64 =
    "C:\\Program Files (x86)\\Qualcomm\\Qualcomm_EUD_Trace\\arm64\\EUD.dll";

const char* GetEudLibPath()
{
    SYSTEM_INFO si;
    GetNativeSystemInfo(&si);
    return (si.wProcessorArchitecture == PROCESSOR_ARCHITECTURE_ARM64)
           ? k_EudLibFullPath_arm64
           : k_EudLibFullPath_x64;
}
#  define k_EudLibFullPath GetEudLibPath()
/* Logging: use QCD_Printf on Windows */
#  define EUD_LOG_DBG(...)   QCD_Printf(Debug, __VA_ARGS__)
#  define EUD_LOG_INFO(...)  QCD_Printf(Info,  __VA_ARGS__)
#  define EUD_LOG_ERR(...)   QCD_Printf(Error, __VA_ARGS__)
#else
typedef void*    EudLibHandle;
/* RTLD_NOW   – resolve all symbols at load time (fail fast if any are missing).
 * RTLD_LOCAL – keep EUD symbols out of the global namespace.
 * RTLD_DEEPBIND – EUD.so resolves its own internal symbols before looking in
 *                 the global namespace, preventing conflicts with other libraries
 *                 already loaded by the host process (e.g. QUTS / libqcdev.so). */
#  define EUD_LOAD_LIB(path)        dlopen(path, RTLD_NOW | RTLD_LOCAL | RTLD_DEEPBIND)
#  define EUD_GET_SYM(h, name)      dlsym(h, name)
#  define EUD_FREE_LIB(h)           dlclose(h)
#  define EUD_LIB_NULL              NULL


static const char* k_EudLibFullPath_x64   = "/opt/qcom/EUD/x64/libeud.so";
static const char* k_EudLibFullPath_arm64 = "/opt/qcom/EUD/arm64/libeud.so";

static const char* GetEudLibPath()
{
    struct utsname uts;
    if (uname(&uts) == 0 && strncmp(uts.machine, "aarch64", 7) == 0)
        return k_EudLibFullPath_arm64;
    return k_EudLibFullPath_x64;
}
#  define k_EudLibFullPath GetEudLibPath()
/* Logging: use QCDEV_LOG_* macros on Linux */
#  define EUD_LOG_DBG   QCDEV_LOG_DBG
#  define EUD_LOG_INFO  QCDEV_LOG_INFO
#  define EUD_LOG_ERR   QCDEV_LOG_ERR
#endif

//===----------------------------------------------------------------------===//
// ConvertDeviceIdToSerNumMsm implementation
//===----------------------------------------------------------------------===//

void ConvertDeviceIdToSerNumMsm(uint32_t device_id, char* serNumMsm, size_t bufSize)
{
    uint32_t correctedId =
        ((device_id & 0x000000FFu) << 24) |
        ((device_id & 0x0000FF00u) <<  8) |
        ((device_id & 0x00FF0000u) >>  8) |
        ((device_id & 0xFF000000u) >> 24);
    snprintf(serNumMsm, bufSize, "%08X", correctedId);
}

//===----------------------------------------------------------------------===//
// Library handle and function pointer types
//===----------------------------------------------------------------------===//

static EudLibHandle g_hEudLib = EUD_LIB_NULL;

typedef EUD_ERR_t (*pfn_get_device_id_array)   (uint32_t*, uint32_t*);
typedef EUD_ERR_t (*pfn_eud_trace_device_init) (uint32_t, uint32_t, uint32_t);
typedef EUD_ERR_t (*pfn_eud_spoof_attach)      (uint32_t);
typedef EUD_ERR_t (*pfn_eud_spoof_detach)      (uint32_t);
typedef EUD_ERR_t (*pfn_eud_trace_device_ready)(void);
typedef EUD_ERR_t (*pfn_eud_trace_device_close)(void);
typedef EUD_ERR_t (*pfn_eud_trace_device_read) (uint8_t*, uint32_t, uint32_t*);
/* USB monitor – matches EUDDevMonitorInit(cb, use_hotplug) / EUDDevMonitorDeInit() */
typedef EUD_ERR_t (*pfn_usb_monitor_init)      (usb_monitor_cb_t, int);
typedef EUD_ERR_t (*pfn_usb_monitor_deinit)    (void);
typedef void      (*pfn_eud_setlogging_callback)  (eud_log_cb_t cb);

static pfn_get_device_id_array      fp_get_device_id_array      = NULL;
static pfn_eud_trace_device_init    fp_eud_trace_device_init    = NULL;
static pfn_eud_spoof_attach         fp_eud_spoof_attach         = NULL;
static pfn_eud_spoof_detach         fp_eud_spoof_detach         = NULL;
static pfn_eud_trace_device_ready   fp_eud_trace_device_ready   = NULL;
static pfn_eud_trace_device_close   fp_eud_trace_device_close   = NULL;
static pfn_eud_trace_device_read    fp_eud_trace_device_read    = NULL;
static pfn_usb_monitor_init         fp_usb_monitor_init         = NULL;
static pfn_usb_monitor_deinit       fp_usb_monitor_deinit       = NULL;
static pfn_eud_setlogging_callback  fp_eud_setlogging_callback  = NULL;

/* Forwards EUD log messages into QDS */
static void EudLogForwarder(const char* msg)
{
    //QCD_Printf(Utils::Info, "EUD: %s", msg);
}

/* Device handle map and lock */
static unordered_map<string, HANDLE> g_EudHandleMap;
#ifdef _WIN32
static CRITICAL_SECTION g_EudMapLock;
static BOOL             s_mapLockInitialized = FALSE;
#  define EUD_MAP_LOCK()    EnterCriticalSection(&g_EudMapLock)
#  define EUD_MAP_UNLOCK()  LeaveCriticalSection(&g_EudMapLock)
#else
static pthread_mutex_t  g_EudMapLock = PTHREAD_MUTEX_INITIALIZER;
#  define EUD_MAP_LOCK()    pthread_mutex_lock(&g_EudMapLock)
#  define EUD_MAP_UNLOCK()  pthread_mutex_unlock(&g_EudMapLock)
#endif
static uint32_t g_DeviceId = 0U;
#ifndef _WIN32
/* True after EudTraceDeviceInit() succeeds for the current device.
 * Prevents re-initialization when initializeEudTrace() is called
 * multiple times for the same device (e.g. QUTS retry logic).
 * Protected by g_EudMapLock. Reset by QcEud_ResetDeviceId() on
 * device departure and by QcEud_CloseDevice() on explicit close. */
static bool     g_TraceInitialized = false;
#endif
#ifndef _WIN32
/* Handle for the pre-loaded libusb — stored so it can be closed on cleanup. */
static void* g_hLibusb = NULL;
#endif

//===----------------------------------------------------------------------===//
// Internal: load EUD library and resolve all required exports
//===----------------------------------------------------------------------===//

static BOOL LoadEudLib()
{
    if (g_hEudLib != EUD_LIB_NULL)
        return TRUE;  /* Already loaded */

#ifndef _WIN32
    /* Pre-load libusb with RTLD_GLOBAL so libeud.so can resolve libusb symbols
     * even if it lacks a DT_NEEDED entry for libusb-1.0.so.0. Lock held during
     * dlopen() to prevent concurrent threads from getting duplicate handles. */
    EUD_MAP_LOCK();
    if (g_hLibusb == NULL) {
        void* h = dlopen("libusb-1.0.so.0", RTLD_LAZY | RTLD_GLOBAL);
        if (h != NULL) {
            g_hLibusb = h;  /* store for cleanup */
        } else {
            const char* lErr = dlerror();
            EUD_LOG_DBG("QcEud: libusb pre-load: %s (continuing)\n",
                        lErr ? lErr : "unknown error");
        }
    }
    EUD_MAP_UNLOCK();
#endif

    g_hEudLib = EUD_LOAD_LIB(k_EudLibFullPath);
    if (g_hEudLib == EUD_LIB_NULL) {
#ifdef _WIN32
        EUD_LOG_ERR("QcEud: Failed to load EUD library from '%s' (err=%d)",
                    k_EudLibFullPath, GetLastError());
#else
        EUD_LOG_ERR("QcEud: Failed to load EUD library from '%s' (err=%s)\n",
                    k_EudLibFullPath, dlerror());
#endif
        return FALSE;
    }

    fp_get_device_id_array    = (pfn_get_device_id_array)   EUD_GET_SYM(g_hEudLib, "get_device_id_array");
    fp_eud_trace_device_init  = (pfn_eud_trace_device_init) EUD_GET_SYM(g_hEudLib, "EudTraceDeviceInit");
    fp_eud_spoof_attach       = (pfn_eud_spoof_attach)      EUD_GET_SYM(g_hEudLib, "eud_spoof_attach");
    fp_eud_spoof_detach       = (pfn_eud_spoof_detach)      EUD_GET_SYM(g_hEudLib, "eud_spoof_detach");
    fp_eud_trace_device_ready = (pfn_eud_trace_device_ready) EUD_GET_SYM(g_hEudLib, "EudTraceDeviceReady");
    fp_eud_trace_device_close = (pfn_eud_trace_device_close)EUD_GET_SYM(g_hEudLib, "EudTraceDeviceClose");
    fp_eud_trace_device_read  = (pfn_eud_trace_device_read) EUD_GET_SYM(g_hEudLib, "EudTraceDeviceRead");
    /* USB monitor exports – required for device-change notifications */
    fp_usb_monitor_init       = (pfn_usb_monitor_init)      EUD_GET_SYM(g_hEudLib, "EUDDevMonitorInit");
    fp_usb_monitor_deinit     = (pfn_usb_monitor_deinit)    EUD_GET_SYM(g_hEudLib, "EUDDevMonitorDeInit");
    /* eud_setlogging_callback is not available in the current EUD library version.
     * Kept for future use — do not remove. */
    // fp_eud_setlogging_callback = (pfn_eud_setlogging_callback) EUD_GET_SYM(g_hEudLib, "eud_setlogging_callback");

    if (!fp_get_device_id_array    || !fp_eud_trace_device_init  ||
        !fp_eud_spoof_attach       || !fp_eud_spoof_detach       ||
        !fp_eud_trace_device_close || !fp_eud_trace_device_read  ||
        !fp_eud_trace_device_ready || !fp_usb_monitor_init       ||
        !fp_usb_monitor_deinit) {
        EUD_LOG_ERR("QcEud: Failed to resolve one or more EUD library exports\n");
        EUD_FREE_LIB(g_hEudLib);
        g_hEudLib = EUD_LIB_NULL;
        return FALSE;
    }

#ifdef _WIN32
    /* Initialize critical section only after successful library load */
    if (!s_mapLockInitialized) {
        InitializeCriticalSection(&g_EudMapLock);
        s_mapLockInitialized = TRUE;
    }
#endif

    /* Register QDS's logger as the EUD log callback so that all QCEUD_Print
     * messages are forwarded into QDS.  Disabled until eud_setlogging_callback
     * is available in the EUD library. */
    // if (fp_eud_setlogging_callback) {
    //     fp_eud_setlogging_callback(EudLogForwarder);
    //     EUD_LOG_DBG("QcEud: EUD log callback registered — EUD logs will be forwarded\n");
    // } else {
    //     EUD_LOG_ERR("QcEud: eud_setlogging_callback not found in EUD library — EUD logs will NOT be forwarded\n");
    // }

    EUD_LOG_INFO("QcEud: EUD library loaded and all exports resolved\n");
    return TRUE;
}

//===----------------------------------------------------------------------===//
// Public QcEud wrapper functions
//===----------------------------------------------------------------------===//

BOOL QcEud_LoadLib(void)
{
    return LoadEudLib();
}

BOOL QcEud_Initialize(void)
{
    if (g_hEudLib == EUD_LIB_NULL) {
        EUD_LOG_ERR("QcEud_Initialize: EUD library not loaded\n");
        return FALSE;
    }

    uint32_t trns_len   = 126;
    uint32_t trns_tmout = 0x00EE12EA;
    EUD_ERR_t err       = EUD_SUCCESS;
    uint32_t cachedId   = 0U;

    /* Hold lock for entire critical section to prevent TOCTOU.
     * The lock is temporarily released around the blocking I/O call
     * (fp_get_device_id_array) and re-acquired immediately after. */
    EUD_MAP_LOCK();
    cachedId = g_DeviceId;

    if (cachedId == 0U)
    {
        uint32_t arr[100] = { 0 };
        uint32_t len      = 0U;

        /* Temporarily release lock for potentially blocking I/O */
        EUD_MAP_UNLOCK();
        err = fp_get_device_id_array(arr, &len);
        EUD_MAP_LOCK();

        if (err != EUD_SUCCESS || len == 0) {
            EUD_MAP_UNLOCK();
            EUD_LOG_ERR("QcEud_Initialize: get_device_id_array failed (err=%d, len=%u)\n", err, len);
            return FALSE;
        }
        if (arr[0] == 0) {
            EUD_MAP_UNLOCK();
            EUD_LOG_ERR("QcEud_Initialize: Get Device ID failed\n");
            return FALSE;
        }
        /* Re-check: only cache if not already reset by a departure callback
         * while the lock was released (compare-and-set pattern). */
        if (g_DeviceId == 0U) {
            g_DeviceId = arr[0];
        }
        cachedId = g_DeviceId;
        EUD_LOG_DBG("QcEud_Initialize: DEVICE ID = 0x%08x\n", cachedId);
    }
    else
    {
        EUD_LOG_DBG("QcEud_Initialize: reusing cached DEVICE ID = 0x%08x\n", cachedId);
    }
    EUD_MAP_UNLOCK();

#ifndef _WIN32
    /* Skip re-initialization if trace is already initialized for this device.
     * Calling EudTraceDeviceInit() a second time without closing first causes
     * LIBUSB_ERROR_BUSY (-16) because the TRC USB interface is still claimed. */
    EUD_MAP_LOCK();
    bool already_init = g_TraceInitialized;
    EUD_MAP_UNLOCK();
    if (already_init) {
        EUD_LOG_INFO("QcEud_Initialize: trace already initialized for device 0x%08x — skipping\n", cachedId);
        return TRUE;
    }
#endif

    err = fp_eud_trace_device_init(cachedId, trns_len, trns_tmout);
    if (EUD_SUCCESS != err) {
#ifndef _WIN32
        if (err == -1) {
            EUD_LOG_ERR("QcEud_Initialize: libusb error %d — continuing\n", err);
            EUD_MAP_LOCK();
            g_TraceInitialized = true;
            EUD_MAP_UNLOCK();
            return TRUE;
        }
#endif
        EUD_LOG_ERR("QcEud_Initialize: eud_trace_device_init failed with err: %x\n", err);
        return FALSE;
    }
    EUD_LOG_INFO("QcEud_Initialize: initialized for device 0x%08x\n", cachedId);
#ifndef _WIN32
    EUD_MAP_LOCK();
    g_TraceInitialized = true;
    EUD_MAP_UNLOCK();
#endif
    return TRUE;
}

EUD_ERR_t QcEud_TraceDeviceReady(void)
{
    if (g_hEudLib == EUD_LIB_NULL) {
        EUD_LOG_ERR("QcEud_TraceDeviceReady: EUD library not loaded\n");
        return EUD_GENERIC_FAILURE;
    }
    EUD_LOG_DBG("QcEud_TraceDeviceReady: calling eud_trace_device_ready\n");
    EUD_ERR_t err = fp_eud_trace_device_ready();
    if (err != EUD_SUCCESS) {
#ifndef _WIN32
        if (err == -1) {
            EUD_LOG_ERR("QcEud_TraceDeviceReady: libusb error %d — continuing\n", err);
            return EUD_SUCCESS;
        }
#endif
        EUD_LOG_ERR("QcEud_TraceDeviceReady: failed with err: %x\n", err);
    }
    return err;
}

VOID QcEud_CloseDevice(void)
{
    if (g_hEudLib == EUD_LIB_NULL) {
        EUD_LOG_ERR("QcEud_CloseDevice: EUD library not loaded\n");
        return;
    }
    EUD_LOG_DBG("QcEud_CloseDevice: calling eud_trace_device_close\n");
    fp_eud_trace_device_close();
#ifndef _WIN32
    /* Allow re-initialization after explicit close */
    EUD_MAP_LOCK();
    g_TraceInitialized = false;
    EUD_MAP_UNLOCK();
#endif
}

BOOL QcEud_ReadTrace(PVOID RxBuffer, DWORD NumBytesToRead, LPDWORD NumBytesReturned)
{
    if (!RxBuffer || !NumBytesReturned) {
        EUD_LOG_ERR("QcEud_ReadTrace: invalid parameter (RxBuffer or NumBytesReturned is NULL)\n");
        return FALSE;
    }
    if (g_hEudLib == EUD_LIB_NULL) {
        EUD_LOG_ERR("QcEud_ReadTrace: EUD library not loaded\n");
        return FALSE;
    }
    EUD_LOG_DBG("QcEud_ReadTrace: calling eud_trace_device_read\n");
    uint32_t bytesRead = 0;
    EUD_ERR_t err = fp_eud_trace_device_read(
        (uint8_t*)RxBuffer,
        (uint32_t)NumBytesToRead,
        &bytesRead);
    *NumBytesReturned = (DWORD)bytesRead;
    EUD_LOG_DBG("QcEud_ReadTrace: err=%08x bytesRead=%u\n", err, bytesRead);
    return (err == EUD_SUCCESS) ? TRUE : FALSE;
}

EUD_ERR_t QcEud_SpoofUsbAttach(void)
{
    if (g_hEudLib == EUD_LIB_NULL) {
        EUD_LOG_ERR("QcEud_SpoofUsbAttach: EUD library not loaded\n");
        return EUD_GENERIC_FAILURE;
    }
    EUD_ERR_t ret = fp_eud_spoof_attach(g_DeviceId);
#ifndef _WIN32
    if (ret == -1) {
        EUD_LOG_ERR("QcEud_SpoofUsbAttach: libusb error %d — continuing\n", ret);
        return EUD_SUCCESS;
    }
#endif
    EUD_LOG_DBG("QcEud_SpoofUsbAttach: returned %d\n", ret);
    return ret;
}

EUD_ERR_t QcEud_SpoofUsbDetach(void)
{
    if (g_hEudLib == EUD_LIB_NULL) {
        EUD_LOG_ERR("QcEud_SpoofUsbDetach: EUD library not loaded\n");
        return EUD_GENERIC_FAILURE;
    }
    EUD_LOG_DBG("QcEud_SpoofUsbDetach: calling eud_spoof_detach\n");
    EUD_ERR_t ret = fp_eud_spoof_detach(g_DeviceId);
#ifndef _WIN32
    if (ret == -1) {
        EUD_LOG_ERR("QcEud_SpoofUsbDetach: libusb error %d — continuing\n", ret);
        return EUD_SUCCESS;
    }
#endif
    return ret;
}

HANDLE QcEud_OpenDevice(PVOID DeviceName, HANDLE Handle)
{
    /* EUD device is present but DLL is not loaded — cannot register handle. */
    if (g_hEudLib == EUD_LIB_NULL) {
        EUD_LOG_ERR("QcEud_OpenDevice: EUD library not loaded — cannot register handle\n");
        return (HANDLE)(intptr_t)INVALID_HANDLE_VALUE;
    }
    EUD_LOG_DBG("QcEud_OpenDevice: registering device handle\n");
#ifdef _WIN32
    std::wstring wName(static_cast<PWCHAR>(DeviceName));
    std::string key(wName.begin(), wName.end());
#else
    std::string key(static_cast<const char*>(DeviceName));
#endif
    EUD_MAP_LOCK();
    g_EudHandleMap[key] = Handle;
    EUD_MAP_UNLOCK();

    /* Signal trace-device readiness when the TRC channel is opened.
     * QcEud_TraceDeviceReady() is called here instead of in QcEud_Initialize()
     * so that readiness is tied to the actual open of the trace channel. */
    if (Handle == QC_EUD_TRC_HANDLE)
    {
        EUD_ERR_t err = QcEud_TraceDeviceReady();
        if (err != EUD_SUCCESS)
        {
#ifndef _WIN32
            if (err == -1) {
                EUD_LOG_ERR("QcEud_OpenDevice: eud_trace_device_ready returned libusb error %d — continuing\n", err);
            } else {
#endif
            EUD_LOG_ERR("QcEud_OpenDevice: eud_trace_device_ready failed (err=%d)\n", err);
            /* Remove the handle from the map and return failure. */
            EUD_MAP_LOCK();
            g_EudHandleMap.erase(key);
            EUD_MAP_UNLOCK();
            return (HANDLE)(intptr_t)INVALID_HANDLE_VALUE;
#ifndef _WIN32
            }
#endif
        }
        EUD_LOG_DBG("QcEud_OpenDevice: trace device ready\n");
    }

    return Handle;
}

BOOL QcEud_FindHandle(HANDLE Handle)
{
    /* If the EUD library is not loaded there are no EUD handles in the map
     * and the map lock may not be initialized — return FALSE immediately. */
    if (g_hEudLib == EUD_LIB_NULL)
        return FALSE;

    EUD_MAP_LOCK();
    for (const auto& entry : g_EudHandleMap) {
        if (entry.second == Handle) {
            EUD_MAP_UNLOCK();
            return TRUE;
        }
    }
    EUD_MAP_UNLOCK();
    EUD_LOG_DBG("QcEud_FindHandle: Handle not found : 0x%x\n", Handle);
    return FALSE;
}

VOID QcEud_UnregisterHandle(HANDLE Handle)
{
    if (g_hEudLib == EUD_LIB_NULL)
        return;

    EUD_MAP_LOCK();
    for (auto it = g_EudHandleMap.begin(); it != g_EudHandleMap.end();) {
        if (it->second == Handle) {
            EUD_LOG_DBG("QcEud_UnregisterHandle: removing %s\n", it->first.c_str());
            it = g_EudHandleMap.erase(it);
        } else {
            ++it;
        }
    }
    EUD_MAP_UNLOCK();
    EUD_LOG_DBG("QcEud_UnregisterHandle: done\n");
}

VOID QcEud_RemoveByName(PVOID DeviceName)
{
    if (!DeviceName || g_hEudLib == EUD_LIB_NULL)
        return;

#ifdef _WIN32
    std::wstring wName(static_cast<PWCHAR>(DeviceName));
    std::string key(wName.begin(), wName.end());
#else
    std::string key(static_cast<const char*>(DeviceName));
#endif
    EUD_MAP_LOCK();
    auto it = g_EudHandleMap.find(key);
    if (it != g_EudHandleMap.end()) {
        EUD_LOG_DBG("QcEud_RemoveByName: removing '%s'\n", key.c_str());
        g_EudHandleMap.erase(it);
    }
    EUD_MAP_UNLOCK();
}

VOID QcEud_ResetDeviceId(void)
{
    /* Guard: on Windows g_EudMapLock is uninitialized if EUD.dll never loaded. */
    if (g_hEudLib == EUD_LIB_NULL)
        return;
    EUD_MAP_LOCK();
    g_DeviceId = 0U;
#ifndef _WIN32
    /* Device departed — allow re-initialization on next arrival */
    g_TraceInitialized = false;
#endif
    EUD_MAP_UNLOCK();
    EUD_LOG_DBG("QcEud_ResetDeviceId: device ID cache cleared\n");
}

VOID QcEud_Cleanup(void)
{
    /* Map lock is only initialized on Windows after a successful library load. */
    if (g_hEudLib != EUD_LIB_NULL)
    {
        EUD_MAP_LOCK();
        EUD_LOG_DBG("QcEud_Cleanup: clearing %zu handle(s) from map\n", g_EudHandleMap.size());
        g_EudHandleMap.clear();
        EUD_MAP_UNLOCK();
    }

#ifndef _WIN32
    /* Release pre-loaded libusb — runs even if libeud.so failed to load. */
    if (g_hLibusb != NULL) {
        if (dlclose(g_hLibusb) != 0) {
            const char* lErr = dlerror();
            EUD_LOG_ERR("QcEud_Cleanup: dlclose(libusb) failed: %s\n",
                        lErr ? lErr : "unknown error");
        }
        g_hLibusb = NULL;
    }
#endif
}

//===----------------------------------------------------------------------===//
// EUD USB monitor – start / stop
//===----------------------------------------------------------------------===//

/*
 * QcEud_StartMonitor
 *   Calls usb_monitor_init(cb, 0) in EUD.dll.
 *   use_hotplug=0 → polling mode (recommended; hotplug requires libusb support).
 *   The supplied callback will be invoked from EUD's background thread whenever
 *   an EUD peripheral (CTL / TRC / SWD / JTG / COM) arrives or departs.
 */
BOOL QcEud_StartMonitor(usb_monitor_cb_t cb)
{
    if (g_hEudLib == EUD_LIB_NULL || !fp_usb_monitor_init) {
        EUD_LOG_ERR("QcEud_StartMonitor: EUD library not loaded or usb_monitor_init unavailable\n");
        return FALSE;
    }
    if (cb == NULL) {
        EUD_LOG_ERR("QcEud_StartMonitor: callback is NULL\n");
        return FALSE;
    }
    EUD_ERR_t err = fp_usb_monitor_init(cb, 0 /* use_hotplug=0 → polling */);
    if (err != EUD_SUCCESS) {
        EUD_LOG_ERR("QcEud_StartMonitor: usb_monitor_init failed (err=%d)\n", err);
        return FALSE;
    }
    EUD_LOG_INFO("QcEud_StartMonitor: EUD USB monitor started \n");
    return TRUE;
}

/*
 * QcEud_StopMonitor
 *   Calls usb_monitor_deinit() in EUD.dll.
 *   Signals the polling thread to stop and waits for it to join before
 *   returning, so no further callbacks will fire after this returns.
 *   Must be called before StopDeviceMonitor() tears down the queue/lock.
 */
VOID QcEud_StopMonitor(void)
{
    if (g_hEudLib == EUD_LIB_NULL || !fp_usb_monitor_deinit) {
        EUD_LOG_ERR("QcEud_StopMonitor: EUD library not loaded or usb_monitor_deinit unavailable\n");
        return;
    }
    EUD_LOG_DBG("QcEud_StopMonitor: stopping EUD USB monitor\n");
    fp_usb_monitor_deinit();
    EUD_LOG_INFO("QcEud_StopMonitor: EUD USB monitor stopped\n");
}
