// Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
// SPDX-License-Identifier: BSD 3-Clause Clear License
#include <fcntl.h>  /* File Control Definitions          */
#include <unistd.h> /* UNIX Standard Definitions         */
#include <sys/ioctl.h>
#include "QdIoQmi.h"
#include "qdaio.h"

QCDEVLIB_API HANDLE QdIoQmi::QCWWAN_OpenService(IN PCHAR QmiDeviceName, IN UCHAR ServiceType)
{
    QDAIO::Initialize();
    return QDAIO::QCWWAN_OpenService(QmiDeviceName, ServiceType);
}

QCDEVLIB_API BOOL QdIoQmi::QCWWAN_CloseService(IN HANDLE ServiceHandle)
{
    return QDAIO::QCWWAN_CloseService(ServiceHandle);
}

QCDEVLIB_API BOOL QdIoQmi::QCWWAN_Cleanup(IN HANDLE ServiceHandle)
{
    return FALSE;
}

QCDEVLIB_API BOOL QdIoQmi::QCWWAN_ReadRaw
(
      IN  HANDLE ServiceHandle,
      OUT PCHAR  ReceiveBuffer,
      IN  ULONG  ReceiveBufferSize,
      OUT PULONG BytesRead
)
{
    return QDAIO::ReadFromDevice(ServiceHandle, ReceiveBuffer, ReceiveBufferSize, BytesRead);
}

QCDEVLIB_API BOOL QdIoQmi::QCWWAN_SendRaw
(
   IN  HANDLE ServiceHandle,
   IN  PCHAR  SendBuffer,
   IN  ULONG  SendLength,
   OUT PULONG BytesWritten
)
{
    return QDAIO::SendToDevice(ServiceHandle, SendBuffer, SendLength, BytesWritten);
}

QCDEVLIB_API BOOL WINAPI QdIoQmi::QCWWAN_GetQmiServiceVersion
(
   HANDLE ServiceHandle,
   UCHAR  ServiceType,
   PULONG Version
)
{
   *Version = 1;
   return TRUE;
}

QCDEVLIB_API BOOL WINAPI QdIoQmi::QCWWAN_IsQmiReady(PCHAR DeviceFriendlyName)
{
   BOOL bResult;
   HANDLE hDevice;
   DWORD bytesReturned = 0;

   hDevice = open(DeviceFriendlyName, O_RDWR | O_NOCTTY);             // Open the device with read/write access   

   if (hDevice < 0){
      perror("Failed to open the device...");
      return FALSE;
   }

   bResult = ioctl(hDevice, IOCTL_QMIREADY, 0);

   close(hDevice);  // close control file handle

   return bResult;
}

QCDEVLIB_API USHORT WINAPI QdIoQmi::QCWWAN_GenerateTransactionId(VOID)
{
    return TRUE;
}

QCDEVLIB_API BOOL WINAPI QdIoQmi::QCWWAN_GetClientId(IN  HANDLE ServiceHandle, OUT PUCHAR ClientId)
{
    return TRUE;
}

QCDEVLIB_API BOOL WINAPI QdIoQmi::QCWWAN_GetQmiServiceVersionEx
(
      HANDLE ServiceHandle, 
      UCHAR  ServiceType,
      PCHAR  AddendumVersionLabel,
      PQMI_SERVICE_VERSION Version
)
{
    return TRUE;
}

QCDEVLIB_API BOOL WINAPI QdIoQmi::QCWWAN_GetParentIdByHandle(HANDLE ServiceHandle, PULONGLONG ParentId)
{
    return TRUE;
}

QCDEVLIB_API BOOL WINAPI QdIoQmi::QCWWAN_GetParentIdByName(PCHAR DeviceFriendlyName, PULONGLONG ParentId)
{
    return TRUE;
}

QCDEVLIB_API PPEER_DEV_INFO_HDR WINAPI QdIoQmi::QCWWAN_GetPeerDevice
(
   PCHAR DeviceFriendlyName,
   PVOID Buffer, 
   ULONG Length 
)
{
    return (PPEER_DEV_INFO_HDR)NULL;
}

QCDEVLIB_API PVOID QdIoQmi::QCWWAN_GetPeerDiagName(PPEER_DEV_INFO_HDR PeerDev)
{
    return NULL;
}
 
QCDEVLIB_API PVOID QdIoQmi::QCWWAN_GetPeerDiagPort(PPEER_DEV_INFO_HDR PeerDev)
{
    return NULL;
}
 
QCDEVLIB_API BOOL QdIoQmi::QCWWAN_GetPrimaryAdapterName
(
   PCHAR DeviceFriendlyName,
   PVOID NameBuffer,
   ULONG BufferLength
)
{
    return TRUE;
}

QCDEVLIB_API BOOL QdIoQmi::QCWWAN_GetQmiQueueSize(HANDLE ServiceHandle, PULONG QueueSize)
{
    return TRUE;
}

QCDEVLIB_API BOOL QdIoQmi::QCWWAN_PurgeQmiQueue(HANDLE ServiceHandle)
{
    return QDAIO::QCWWAN_PurgeQmiQueue(ServiceHandle);
}
