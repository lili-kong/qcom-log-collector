// Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
// SPDX-License-Identifier: BSD 3-Clause Clear License
#ifndef QCOMLIBUSB_H
#define QCOMLIBUSB_H

#pragma once

enum interface_protocols {
    INTERFACE_DIAG_PROTOCOL = 0x30,
    INTERFACE_MODEM_PROTOCOL = 0x40,
    INTERFACE_DPL_PROTOCOL = 0x80,
    INTERFACE_QDSS_PROTOCOL = 0x70,
    INTERFACE_SAHARA_PROTOCOL = 0x10,
    INTERFACE_EDL_PROTOCOL = 0x11,
    INTERFACE_FB_PROTOCOL = 0x12,
    INTERFACE_RAMDUMP_PROTOCOL = 0x13,
    INTERFACE_EFS_PROTOCOL = 0x14,
    INTERFACE_FIREHOSE_PROTOCOL = 0x20,
    INTERFACE_RMNET_PROTOCOL = 0x50
};

#if defined(_WIN32) || defined(_WIN64)
#define strtok_r strtok_s
#define QCOM_MAX_STRING_SIZE			1024
#endif

#include "libusb-dynamic-loader.h"

#if (defined(__GNUC__) && defined(__unix__))
#include <sys/stat.h>
#define ERROR_INVALID_PARAMETER       EINVAL
#define QCOM_MAX_STRING_SIZE			256
#define QCOM_DIAG_DEV_NAME			"QCOM_LIBUSB_Diagnostics"
#define QCOM_MODEM_DEV_NAME			"QCOM_LIBUSB_Modem"
#define QCOM_DPL_DEV_NAME			"QCOM_LIBUSB_DPL_Data"
#define QCOM_QDSS_DEV_NAME			"QCOM_LIBUSB_QDSS_Data"
#define QCOM_EDL_DEV_NAME			"QCOM_LIBUSB_QDLoader"
#endif

//#ifdef __cplusplus
//extern "C" {
//#endif

#include <atomic>
#include <unordered_map>
#include <string>

#ifdef QCOM_WIN_ENV
#include <windows.h>
#include <conio.h>
#include <sstream>
#include <cstdio>
#include <strsafe.h>
#include <vector>
//#include <shlwapi.h>
 #define QC_REG_SW_KEY_LIBUSB "SYSTEM\\CurrentControlSet\\Control\\Class\\{eb781aaf-9c70-4523-a5df-642a87eca567}"
 #define QC_SPEC_SERNUM      "QCDeviceSerialNumber"
 #define QC_SPEC_SERNUM_MSM  "QCDeviceMsmSerialNumber"
 #define QC_SPEC_DEV_NAME    "QCDeviceDescription"
 #define QC_SPEC_PROTOC      "QCDeviceProtocol"
 #define QC_REG_SW_KEY       "SYSTEM\\CurrentControlSet\\Control\\Class\\"
 #define QC_REG_SW_KEY_USER   "Software\\Qualcomm\\QDS\\USB\\"
#endif

#ifdef QCOM_LNX_ENV
#include <stdio.h>
#include <stdlib.h>
#include <fcntl.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <errno.h>
#endif

#include "../../common/utils.h"
#ifdef QCOM_WIN_ENV
//#include "../../qds_win/scandev.h"
#endif
#ifdef QCOM_LNX_ENV
#include "../../qds_lnx/inc/windows_compatible.h"
#define LIBUSB_CONFIG_FILE "/etc/qcom_libusb.conf"
#endif

#define QCOM_VENDOR_ID              0x05c6 // device's Vendor ID
#define QCOM_EDL_PID                "9008" // EDL mode PID
using namespace std;
struct _QcdevCtx;

#include <mutex>
#include <condition_variable>

typedef struct qcom_libusb_device
{
    struct libusb_device_handle*    dev_handle; //single dev handle per device
    struct libusb_device*           dev;
    BOOL                            num_devices;
    INT                             ref_count;
    atomic<int>                     active_transfer;
    mutex                           dev_handle_mutex; /* Per-device synchronization primitives */
    condition_variable              active_transfer_cv;
    CHAR                            SerNumMSM[QCOM_MAX_STRING_SIZE];
    CHAR                            qcomDevKey[64];
    LIST_ENTRY                      deviceListHead;
    LIST_ENTRY                      ArrivalListLibusb;
}QcomLibUSBdevCtx, *PQcomLibUSBdevCtx;

typedef struct device_info
{
    LIST_ENTRY                      List;
    uint64_t                        virtual_handle; //unique virtual dev handle for each interface
    CHAR                            DevDesc[QCOM_MAX_STRING_SIZE];
    CHAR                            DevName[QCOM_MAX_STRING_SIZE];
    CHAR                            Loc[QCOM_MAX_STRING_SIZE];
    CHAR                            DevPath[QCOM_MAX_STRING_SIZE];
    CHAR                            SerNum[QCOM_MAX_STRING_SIZE];
    CHAR                            SerNumMSM[QCOM_MAX_STRING_SIZE];
    ULONG                           Flag;
    ULONG                           protocol;
    CHAR                            HwId[QCOM_MAX_STRING_SIZE];
    CHAR                            ParentDev[QCOM_MAX_STRING_SIZE];
    CHAR                            SocVer[QCOM_MAX_STRING_SIZE];
    CHAR                            ParentLocationInformation[QCOM_MAX_STRING_SIZE];
    CHAR                            DevDetails[QCOM_MAX_STRING_SIZE];
    UINT                            interfaceNumber;
    uint8_t                         bulk_in_endpointAddr;
    uint8_t                         bulk_out_endpointAddr;
    UINT                            bulk_in_size;
    UINT                            bulk_out_size;
    std::unordered_map<std::string, std::string>* DevDetailsMP; // pointer-based: contains fields obtained from parsed device description string
    PQcomLibUSBdevCtx               devCtx;
}QcomDeviceInfo, * PQcomDeviceInfo;

BOOL initialize_libusb();
VOID deinitialize_libusb();
BOOL enumerate_devices();
BOOL process_device(libusb_device* dev, void *QcdevCtx);
int qcom_libusb_open_internal(QcomDeviceInfo* qcomDevInfo, void** devHandle);
VOID display_devices(QcomLibUSBdevCtx* qcom_dev);
VOID update_arrival_list_libusb(QcomLibUSBdevCtx* qcom_dev);

#ifdef QCOM_WIN_ENV
BOOLEAN set_serial_num_in_registry(LPCSTR SW_KEY_PATH, const char* SerNum);
BOOLEAN set_serial_num_msm_in_registry(LPCSTR SW_KEY_PATH, const char* SerNumMsm);
BOOLEAN set_protocol_in_registry(LPCSTR SW_KEY_PATH, DWORD protocol);
// VOID SetParamsInRegistry(QcomLibUSBdevCtx* qcom_dev, LPCSTR driverKeyPath, PTSTR InterfaceName)
VOID set_param_in_registry(LPCSTR driverKeyPath, const char * InterfaceName, string);
BOOLEAN set_dev_details_in_registry(LPCSTR SW_KEY_PATH, const char* DevDetails);
PQcomDeviceInfo find_device(PQcomLibUSBdevCtx devContext, const char* SernumMsm, const char* devDesc);
#endif

#ifdef QCOM_LNX_ENV
VOID register_libusb_hotplug_callback(void *device_ctx);
BOOL libusb_device_change_callback(libusb_context *ctx,
                                  libusb_device *dev,
                                  libusb_hotplug_event event,
                                  void *user_data);
void PushLibUSBToApplication(PQcomDeviceInfo devinfo);
std::unordered_map<std::string, std::string> ParseDevDesc(const char* input, std::unordered_map<std::string, std::string>& deviceMap);
BOOL IsQcomLibusbEnable();
PQcomDeviceInfo find_device(PQcomLibUSBdevCtx devContext, const char* devDesc);
#endif

namespace QcomLibusbDevice {

    BOOL qcom_libusb_open(char* DeviceName, char *SernumMsm, void** devHandle);
    BOOL qcom_libusb_open(char *SernumMsm, unsigned int bInterfaceNumber, void** devHandle);
    VOID qcom_libusb_close(void** devHandle);
    BOOL qcom_libusb_read(void **devHandle, void* buffer, int bufferLen, int* bytesRead, unsigned timeout);
    BOOL qcom_libusb_write(void** devHandle, void* buffer, int bufferLen, int* bytesSent, unsigned timeout);

} //namespace QcomLibusbDevice

//#ifdef __cplusplus
//}
//#endif


#endif
