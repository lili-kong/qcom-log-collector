// Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
// SPDX-License-Identifier: BSD 3-Clause Clear License
#ifndef QDIOQMI_H
#define QDIOQMI_H

#include <stdlib.h>
#include <stdio.h>
#include <windows.h>
#include <conio.h>
#include <string.h>
#include <winioctl.h>

#define SERVICE_FILE_BUF_LEN      256
#define QMUX_NUM_THREADS  3
#define QMUX_MAX_DATA_LEN 2048
#define QMUX_MAX_CMD_LEN  2048

// User-defined IOCTL code range: 2048-4095
#define QCDEV_IOCTL_INDEX                   2048
#define QCDEV_DUPLICATED_NOTIFICATION_REQ 0x00000002L

#define IOCTL_QCDEV_WAIT_NOTIFY CTL_CODE(FILE_DEVICE_UNKNOWN, \
                                  QCDEV_IOCTL_INDEX+1, \
                                  METHOD_BUFFERED, \
                                  FILE_ANY_ACCESS)

/* Make the following code as 3338 - USB debug mask */
#define IOCTL_QCDEV_SET_DBG_UMSK CTL_CODE(FILE_DEVICE_UNKNOWN, \
                                  QCDEV_IOCTL_INDEX+1290, \
                                  METHOD_BUFFERED, \
                                  FILE_ANY_ACCESS)

/* Make the following code as 3339 - MP debug mask */
#define IOCTL_QCDEV_SET_DBG_MMSK CTL_CODE(FILE_DEVICE_UNKNOWN, \
                                  QCDEV_IOCTL_INDEX+1291, \
                                  METHOD_BUFFERED, \
                                  FILE_ANY_ACCESS)

/* Make the following code as 3340 - MP debug mask */
#define IOCTL_QCDEV_GET_SERVICE_FILE CTL_CODE(FILE_DEVICE_UNKNOWN, \
                                  QCDEV_IOCTL_INDEX+1292, \
                                  METHOD_BUFFERED, \
                                  FILE_ANY_ACCESS)

/* Make the following code as 3343 - QMI Client Id  */
#define IOCTL_QCDEV_QMI_GET_CLIENT_ID CTL_CODE(FILE_DEVICE_UNKNOWN, \
                                              QCDEV_IOCTL_INDEX+1295, \
                                              METHOD_BUFFERED, \
                                              FILE_ANY_ACCESS)

/* Make the following code as 3344 - versions of QMI services  */
#define IOCTL_QCDEV_QMI_GET_SVC_VER CTL_CODE(FILE_DEVICE_UNKNOWN, \
                                              QCDEV_IOCTL_INDEX+1296, \
                                              METHOD_BUFFERED, \
                                              FILE_ANY_ACCESS)

#define IOCTL_QCDEV_QMI_GET_SVC_VER_EX CTL_CODE(FILE_DEVICE_UNKNOWN, \
                                              QCDEV_IOCTL_INDEX+23, \
                                              METHOD_BUFFERED, \
                                              FILE_ANY_ACCESS)

#define IOCTL_QCDEV_CLEANUP CTL_CODE(FILE_DEVICE_UNKNOWN, \
                                     QCDEV_IOCTL_INDEX+31, \
                                     METHOD_BUFFERED, \
                                     FILE_ANY_ACCESS)

#define IOCTL_QCDEV_GET_PEER_DEV_NAME CTL_CODE(FILE_DEVICE_UNKNOWN, \
                                       QCDEV_IOCTL_INDEX+32, \
                                       METHOD_BUFFERED, \
                                       FILE_ANY_ACCESS)

#define IOCTL_QCDEV_GET_PRIMARY_ADAPTER_NAME CTL_CODE(FILE_DEVICE_UNKNOWN, \
                                              QCDEV_IOCTL_INDEX+35, \
                                              METHOD_BUFFERED, \
                                              FILE_ANY_ACCESS)

#define IOCTL_QCDEV_QMI_READY CTL_CODE(FILE_DEVICE_UNKNOWN, \
                                       QCDEV_IOCTL_INDEX+52, \
                                       METHOD_BUFFERED, \
                                       FILE_ANY_ACCESS)

#define IOCTL_QCDEV_DEVICE_GROUP_INDETIFIER CTL_CODE(FILE_DEVICE_UNKNOWN, \
                                                 QCDEV_IOCTL_INDEX+1305, \
                                                 METHOD_BUFFERED, \
                                                 FILE_ANY_ACCESS)

#define IOCTL_QCDEV_GET_QMI_QUEUE_SIZE CTL_CODE(FILE_DEVICE_UNKNOWN, \
                                       QCDEV_IOCTL_INDEX+53, \
                                       METHOD_BUFFERED, \
                                       FILE_ANY_ACCESS)

#define IOCTL_QCDEV_PURGE_QMI_QUEUE CTL_CODE(FILE_DEVICE_UNKNOWN, \
                                       QCDEV_IOCTL_INDEX+54, \
                                       METHOD_BUFFERED, \
                                       FILE_ANY_ACCESS)

typedef struct _QMI_SERVICE_VERSION
{
   USHORT BaseMajor;
   USHORT BaseMinor;
   USHORT AddendumMajor;
   USHORT AddendumMinor;
} QMI_SERVICE_VERSION, *PQMI_SERVICE_VERSION;

// Peer Device Info
#pragma pack(push, 1)

typedef struct _PEER_DEV_INFO_HDR
{
   UCHAR Version;
   USHORT DeviceNameLength;
   USHORT SymLinkNameLength;
} PEER_DEV_INFO_HDR, *PPEER_DEV_INFO_HDR;

#pragma pack(pop)

namespace QdIoQmi
{
   QCDEVLIB_API HANDLE QCWWAN_OpenService(IN PCHAR QmiDeviceName, IN UCHAR ServiceType);

   QCDEVLIB_API BOOL QCWWAN_CloseService(IN HANDLE ServiceHandle);

   QCDEVLIB_API BOOL QCWWAN_Cleanup(IN HANDLE ServiceHandle);

   QCDEVLIB_API BOOL QCWWAN_ReadRaw
   (
      IN  HANDLE ServiceHandle,
      OUT PCHAR  ReceiveBuffer,
      IN  ULONG  ReceiveBufferSize,
      OUT PULONG BytesRead
   );

   QCDEVLIB_API BOOL QCWWAN_SendRaw
   (
      IN  HANDLE ServiceHandle,
      IN  PCHAR  SendBuffer,
      IN  ULONG  SendLength,
      OUT PULONG BytesWritten
   );

   QCDEVLIB_API USHORT QCWWAN_GenerateTransactionId(VOID);

   QCDEVLIB_API BOOL WINAPI QCWWAN_GetClientId(IN  HANDLE ServiceHandle, OUT PUCHAR ClientId);

   QCDEVLIB_API BOOL WINAPI QCWWAN_GetQmiServiceVersion
   (
      HANDLE ServiceHandle,
      UCHAR  ServiceType,
      PULONG Version
   );

   QCDEVLIB_API BOOL WINAPI QCWWAN_GetQmiServiceVersionEx
   (
      HANDLE ServiceHandle,
      UCHAR  ServiceType,
      PCHAR  AddendumVersionLabel,
      PQMI_SERVICE_VERSION Version
   );

   QCDEVLIB_API BOOL WINAPI QCWWAN_GetParentIdByHandle(HANDLE ServiceHandle, PULONGLONG ParentId);

   QCDEVLIB_API BOOL WINAPI QCWWAN_GetParentIdByName(PCHAR DeviceFriendlyName, PULONGLONG ParentId);

   QCDEVLIB_API BOOL WINAPI QCWWAN_IsQmiReady(PCHAR DeviceFriendlyName);

   QCDEVLIB_API PPEER_DEV_INFO_HDR WINAPI QCWWAN_GetPeerDevice
   (
      PCHAR DeviceFriendlyName,
      PVOID Buffer,
      ULONG Length
   );

   QCDEVLIB_API PVOID QCWWAN_GetPeerDiagName(PPEER_DEV_INFO_HDR PeerDev);

   QCDEVLIB_API PVOID QCWWAN_GetPeerDiagPort(PPEER_DEV_INFO_HDR PeerDev);

   QCDEVLIB_API BOOL QCWWAN_GetPrimaryAdapterName
   (
      PCHAR DeviceFriendlyName,
      PVOID NameBuffer,
      ULONG BufferLength
   );

   QCDEVLIB_API BOOL QCWWAN_GetQmiQueueSize(HANDLE ServiceHandle, PULONG QueueSize);

   QCDEVLIB_API BOOL QCWWAN_PurgeQmiQueue(HANDLE ServiceHandle);
}  // QdIoQmi

#endif // QDIOQMI_H
