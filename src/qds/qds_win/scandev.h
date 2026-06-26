// Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
// SPDX-License-Identifier: BSD 3-Clause Clear License
#ifndef SCANDEV_H
#define SCANDEV_H

#include <stdlib.h>
#include <stdio.h>
#include <windows.h>
#include <tchar.h>
#include <wchar.h>
#include <conio.h>
#include <string.h>
#include <winioctl.h>
#include <setupapi.h>
#include <Strsafe.h>
#include <Shlwapi.h>
#include <Cfgmgr32.h>
#include <atomic>
#include <initguid.h>
//#include <devpropdef.h>
#include <devpkey.h>
#include <sstream>

#include "..\libusb\source\qcom-libusb.h"
#include "qdpublic.h"

typedef enum _QC_REG_LOC
{
   QC_REG_DEVMAP = 0,
   QC_REG_USB    = 1,
   QC_REG_MDM    = 2,
   QC_REG_NET    = 3,
   QC_REG_PORTS  = 4,
   QC_REG_ADB    = 5,
   QC_REG_LIBUSB = 6,
   QC_REG_MAX
} QC_REG_LOC;

#define QC_REG_KEY_DEVMAP   "HARDWARE\\DEVICEMAP\\SERIALCOMM"
#define QC_REG_SW_KEY       "SYSTEM\\CurrentControlSet\\Control\\Class\\"
#define QC_REG_SW_KEY_ADB   "SYSTEM\\CurrentControlSet\\Control\\Class\\{3f966bd9-fa04-4ec5-991c-d326973b5128}"
#define QC_REG_SW_KEY_USB   "SYSTEM\\CurrentControlSet\\Control\\Class\\{88BAE032-5A81-49f0-BC3D-A4FF138216D6}"
#define QC_REG_SW_KEY_MDM   "SYSTEM\\CurrentControlSet\\Control\\Class\\{4D36E96D-E325-11CE-BFC1-08002BE10318}"
#define QC_REG_SW_KEY_NET   "SYSTEM\\CurrentControlSet\\Control\\Class\\{4D36E972-E325-11CE-BFC1-08002BE10318}"
#define QC_REG_SW_KEY_PORTS "SYSTEM\\CurrentControlSet\\Control\\Class\\{4D36E978-E325-11CE-BFC1-08002BE10318}"
#define QC_REG_SW_KEY_LIBUSB "SYSTEM\\CurrentControlSet\\Control\\Class\\{eb781aaf-9c70-4523-a5df-642a87eca567}"
#define QC_REG_HW_KEY_USB   "SYSTEM\\CurrentControlSet\\Enum\\USB\\"
#define QC_REG_HW_KEY_PREF  "SYSTEM\\CurrentControlSet\\Enum\\"
#define QC_REG_SW_KEY_USER   "Software\\Qualcomm\\QDS\\USB\\"
#define QC_REG_HW_KEY_PARAM "\\Device Parameters"
#define QC_NET_CONNECTION_REG_KEY "SYSTEM\\CurrentControlSet\\Control\\Network\\{4D36E972-E325-11CE-BFC1-08002BE10318}\\"

#define QC_DEV_PREFIX       "\\\\.\\"
#define DEV_PORT_NAME       "PortName"
#define QC_SPEC             "AssignedPortForQCDevice"
#define QC_SPEC_NET         "QCDeviceControlFile"
#define QC_SPEC_STAMP       "QCDeviceStamp"
#define QC_SPEC_MTU         "QCAdvertisedMtu"
#define QC_SPEC_SERNUM      "QCDeviceSerialNumber"
#define QC_SPEC_SOCVER      "QCDeviceSOCVER"
#define QC_SPEC_SERNUM_MSM  "QCDeviceMsmSerialNumber"
#define QC_SPEC_CID         "QCDeviceCID"
#define QC_SPEC_PROTOC      "QCDeviceProtocol"
#define QC_SPEC_PARENT_DEV  "QCDeviceParent"
#define QC_SPEC_SSR         "QCDeviceSSR"
#define QC_SPEC_DEV_NAME    "QCDeviceDescription"
#define QC_IFACENUM_PREFIX  L"MI_"
#define QC_IFACENUM_LENGTH  2
#define QC_MAX_VALUE_NAME 1024
#define REG_HW_ID_SIZE    2048

#define D_CLASS_MODEM  "MODEM"
#define D_CLASS_PORTS  "PORTS"
#define D_CLASS_NET    "NET"
#define D_CLASS_USB    "USB"
#define D_CLASS_LIBUSB "LIBUSB-WIN32 DEVICES"
#define D_CLASS_USBDEV "USBDEVICE"
#define D_CLASS_ADB    "ANDROIDUSBDEVICECLASS"
#define D_CLASS_CPU    "PROCESSOR"

#define BUS_TEST_USB  "USB\\"
#define BUS_TEST_PCI  "PCI\\"
#define BUS_TEST_ACPI "ACPI\\"
#define BUS_TEST_PCIE "QC_BUS\\"

#define MAX_NUM_DEV         512
#define DEV_FLAG_NONE         0
#define DEV_FLAG_DEPARTURE    1
#define DEV_FLAG_ARRIVAL      2

typedef struct _INTERNAL_DEV_FEATURE_SETTING
{
   DEV_FEATURE_SETTING User;
   DWORD TimerInterval;          // [20..5000]
   CHAR VID[QC_MAX_VALUE_NAME];  // VID
} INTERNAL_DEV_FEATURE_SETTING, *PINTERNAL_DEV_FEATURE_SETTING;

typedef struct _QC_DEV_ITEM
{
   LIST_ENTRY List;
   struct
   {
      UCHAR Type;
      UCHAR Flag;
      UCHAR IsQCDriver;
      UCHAR Reserved;
   } Info;
   PVOID Context;
   PVOID UserContext;
   CB_PARAMS CbParams;
   CHAR DevDesc[QC_MAX_VALUE_NAME];  // name for display
   CHAR DevDescA[QC_MAX_VALUE_NAME]; // name for display
   CHAR DevNameW[QC_MAX_VALUE_NAME]; // device name for communication (open/close)
   CHAR DevNameA[QC_MAX_VALUE_NAME]; // device name for communication (open/close)
   CHAR HwId[QC_MAX_VALUE_NAME];     // HW IDs
   CHAR ParentDev[QC_MAX_VALUE_NAME];// parent device name
   CHAR ParentLocationInfomation[QC_MAX_VALUE_NAME];// parent location information
   CHAR Location[QC_MAX_VALUE_NAME]; // Location on the bus
   CHAR DevPath[QC_MAX_VALUE_NAME];  // device connection path
   CHAR InterfaceName[QC_MAX_VALUE_NAME]; // Network interface name of a NIC
   UINT InterfaceNumber;
   CHAR SerNum[256];
   CHAR SocVer[256];
   CHAR SerNumMsm[256];
   CHAR PortNos[256];
   UCHAR BusType;
   std::unordered_map<std::string, std::string>* DevDetails; // pointer-based: contains fields obtained from parsed device description string
} QC_DEV_ITEM, *PQC_DEV_ITEM;

typedef struct _QC_NOTIFICATION_STORE
{
   LIST_ENTRY List;
   LIST_ENTRY DevItemChain;
} QC_NOTIFICATION_STORE, *PQC_NOTIFICATION_STORE;
#endif // SCANDEV_H
