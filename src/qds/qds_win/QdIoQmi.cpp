// Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
// SPDX-License-Identifier: BSD 3-Clause Clear License
// #include "stdafx.h"
#include "qdpublic.h"
#include "QdIoQmi.h"
#include "scandev.h"

using namespace QcDevice;
using namespace QdIoQmi;
using namespace Utils;

namespace QdIoQmi
{
   namespace
   {
      static CRITICAL_SECTION TidLock;

      VOID QCWWAN_InitializeResources(VOID)
      {
         InitializeCriticalSection(&TidLock);
      }

      VOID QCWWAN_ReleaseResources(VOID)
      {
         DeleteCriticalSection(&TidLock);
      }

      BOOL WINAPI GetServiceFile
      (
         HANDLE hd,
         PCHAR  ServiceFileName,
         UCHAR  ServiceType
      )
      {
         DWORD bytesReturned = 0;

         if (DeviceIoControl(
                               hd,
                               IOCTL_QCDEV_GET_SERVICE_FILE,
                               (LPVOID)&ServiceType,
                               (DWORD)sizeof(UCHAR),
                               (LPVOID)ServiceFileName,
                               (DWORD)SERVICE_FILE_BUF_LEN,
                               &bytesReturned,
                               NULL
                            )
            )
         {
            QCD_Printf(Info, "Got service file <%s>\n", ServiceFileName);

            return TRUE;
         }

         QCD_Printf(Error, "GetServiceFile failure (error code 0x%x)\n", GetLastError());

         return FALSE;
      }  // GetServiceFile

   }
}  // QdIoQmi

// ----------- PUBLIC APIs -------------
QCDEVLIB_API HANDLE QdIoQmi::QCWWAN_OpenService(IN PCHAR QmiDeviceName, IN UCHAR ServiceType)
{
   HANDLE hServiceDevice, hDevice;
   char serviceFileName[SERVICE_FILE_BUF_LEN];
   char tmpName[SERVICE_FILE_BUF_LEN];
   BOOL bResult;

   // Init service file name buffer
   ZeroMemory(serviceFileName, SERVICE_FILE_BUF_LEN);
   ZeroMemory(tmpName, SERVICE_FILE_BUF_LEN);

   hDevice = CreateFileA
             (
                QmiDeviceName,
                GENERIC_WRITE | GENERIC_READ,
                FILE_SHARE_READ | FILE_SHARE_WRITE,
                NULL,
                OPEN_EXISTING,
                FILE_ATTRIBUTE_NORMAL,
                NULL
             );

   if (hDevice == INVALID_HANDLE_VALUE)
   {
      QCD_Printf(Error, "ERROR opening control file <%s>: INVALID_HANDLE_VALUE (error code %u)\n",
             QmiDeviceName, GetLastError());
      return INVALID_HANDLE_VALUE;
   }

   // Get QMUX service file
   ZeroMemory(tmpName, SERVICE_FILE_BUF_LEN);
   bResult = GetServiceFile(hDevice, tmpName, ServiceType);
   CloseHandle(hDevice);  // close control file handle

   if (bResult == FALSE)
   {
      QCD_Printf(Error, "ERROR: cannot get service file.\n");
      return INVALID_HANDLE_VALUE;
   }

   _snprintf_s((PCHAR)serviceFileName, SERVICE_FILE_BUF_LEN, _TRUNCATE, "\\\\.\\%s", tmpName);

   hServiceDevice = CreateFileA
                    (
                       serviceFileName,
                       GENERIC_WRITE | GENERIC_READ,
                       FILE_SHARE_READ | FILE_SHARE_WRITE,
                       NULL,
                       OPEN_EXISTING,
                       FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OVERLAPPED,
                       NULL
                    );
   if (hServiceDevice == INVALID_HANDLE_VALUE)
   {
      QCD_Printf(Error, "ERROR opening service <%s>: INVALID_HANDLE_VALUE (error code 0x%x)\n",
                  serviceFileName, GetLastError());
      return INVALID_HANDLE_VALUE;
   }

   return hServiceDevice;
}  // QCWWAN_OpenService


QCDEVLIB_API BOOL QdIoQmi::QCWWAN_CloseService(HANDLE ServiceHandle)
{
   return CloseHandle(ServiceHandle);
}

QCDEVLIB_API BOOL QdIoQmi::QCWWAN_Cleanup(HANDLE ServiceHandle)
{
   DWORD bytesReturned = 0;

   if (DeviceIoControl(
                         ServiceHandle,
                         IOCTL_QCDEV_CLEANUP,
                         NULL,
                         0,
                         NULL,
                         0,
                         &bytesReturned,
                         NULL
                      )
      )
   {
      QCD_Printf(Info, "QCWWAN_Cleanup for handle 0x%x\n", ServiceHandle);

      return TRUE;
   }

   QCD_Printf(Error, "QCWWAN_Cleanup for handle failed (error code 0x%x)\n", GetLastError());

   return FALSE;
}  // QCWWAN_Cleanup

QCDEVLIB_API BOOL QdIoQmi::QCWWAN_ReadRaw
(
   HANDLE ServiceHandle,
   PCHAR  ReceiveBuffer,
   ULONG  ReceiveBufferSize,
   PULONG BytesRead
)
{
   OVERLAPPED ov;
   BOOL       bResult = FALSE;
   DWORD      dwStatus = NO_ERROR;

   ZeroMemory(&ov, sizeof(ov));
   ov.Offset = 0;
   ov.OffsetHigh = 0;
   ov.hEvent = CreateEvent(NULL, TRUE, FALSE, NULL);
   if (ov.hEvent == NULL)
   {
      QCD_Printf(Error, "Read failure, event error %u\n", GetLastError());
      return bResult;
   }

   *BytesRead = 0;

   bResult = ReadFile
             (
                ServiceHandle,
                ReceiveBuffer,
                ReceiveBufferSize,
                BytesRead,
                &ov
             );

   if (bResult == TRUE)
   {
      if (ov.hEvent != NULL)
      {
         CloseHandle(ov.hEvent);
      }
      return bResult;
   }
   else
   {
      dwStatus = GetLastError();

      if (ERROR_IO_PENDING != dwStatus)
      {
         QCD_Printf(Error, "Read failure, error %u\n", dwStatus);
      }
      else
      {
         bResult = GetOverlappedResult
                   (
                      ServiceHandle,
                      &ov,
                      BytesRead,
                      TRUE  // no return until operaqtion completes
                   );

         if (bResult == FALSE)
         {
            dwStatus = GetLastError();
            QCD_Printf(Error, "Read/GetOverlappedResult failure, error %u\n", dwStatus);
         }
      }
   }

   if (ov.hEvent != NULL)
   {
      CloseHandle(ov.hEvent);
   }

   return bResult;

}  // QCWWAN_ReadRaw

QCDEVLIB_API BOOL QdIoQmi::QCWWAN_SendRaw
(
   HANDLE ServiceHandle,
   PCHAR  SendBuffer,
   ULONG  SendLength,
   PULONG BytesWritten
)
{
   BOOL       bResult = FALSE;
   OVERLAPPED ov;
   DWORD      dwStatus = NO_ERROR;

   ZeroMemory(&ov, sizeof(ov));
   ov.Offset = 0;
   ov.OffsetHigh = 0;
   ov.hEvent = CreateEvent(NULL, TRUE, FALSE, NULL);
   if (ov.hEvent == NULL)
   {
      QCD_Printf(Error, "Read error, event error %u\n", GetLastError());
      return bResult;
   }

   *BytesWritten = 0;

   // send message
   if ((SendBuffer != NULL) && (SendLength != 0))
   {
      #ifdef MY_UNIT_TEST
      return 0;
      #endif // MY_UNIT_TEST

      bResult = WriteFile
                (
                   ServiceHandle,
                   SendBuffer,
                   SendLength,
                   BytesWritten,
                   &ov
                );

      if (bResult == FALSE)
      {
         dwStatus = GetLastError();

         if (ERROR_IO_PENDING != dwStatus)
         {
            QCD_Printf(Error, "write error, error %u\n", dwStatus);
         }
         else
         {
            bResult = GetOverlappedResult
                      (
                         ServiceHandle,
                         &ov,
                         BytesWritten,
                         TRUE  // no return until operaqtion completes
                      );

            if (bResult == FALSE)
            {
               dwStatus = GetLastError();
               QCD_Printf(Error, "Write error, error %u\n", dwStatus);
            }
         }
      }
   }
   else
   {
      QCD_Printf(Error, "Write error: cannot get message to send.\n");
   }

   if (ov.hEvent != NULL)
   {
      CloseHandle(ov.hEvent);
   }

   return bResult;
}  // QCWWAN_SendRaw

QCDEVLIB_API USHORT QdIoQmi::QCWWAN_GenerateTransactionId(VOID)
{
   static USHORT tid = 0;
   USHORT retVal;

   EnterCriticalSection(&TidLock);

   if (++tid == 0)
   {
      tid = 1;
   }
   retVal = tid;

   LeaveCriticalSection(&TidLock);

   return retVal;
}  // QCWWAN_GenerateTransactionId

QCDEVLIB_API BOOL WINAPI QdIoQmi::QCWWAN_GetClientId
(
   HANDLE ServiceHandle,
   PUCHAR ClientId
)
{
   DWORD bytesReturned = 0;
   UCHAR cid;

   if (DeviceIoControl(
                         ServiceHandle,
                         IOCTL_QCDEV_QMI_GET_CLIENT_ID,
                         NULL,
                         0,
                         (LPVOID)&cid,
                         sizeof(UCHAR),
                         &bytesReturned,
                         NULL
                      )
      )
   {
      *ClientId = cid;
      return TRUE;
   }

   return FALSE;
}  // QCWWAN_GetClientId

QCDEVLIB_API BOOL WINAPI QdIoQmi::QCWWAN_GetQmiServiceVersion
(
   HANDLE ServiceHandle,
   UCHAR  ServiceType,
   PULONG Version
)
{
   DWORD bytesReturned = 0;
   ULONG version;

   if (DeviceIoControl(
                         ServiceHandle,
                         IOCTL_QCDEV_QMI_GET_SVC_VER,
                         &ServiceType,
                         sizeof(UCHAR),
                         (LPVOID)&version,
                         sizeof(ULONG),
                         &bytesReturned,
                         NULL
                      )
      )
   {
      *Version = version;
      return TRUE;
   }

   return FALSE;
}  // QCWWAN_GetQmiServiceVersion

QCDEVLIB_API BOOL WINAPI QdIoQmi::QCWWAN_GetQmiServiceVersionEx
(
   HANDLE ServiceHandle,
   UCHAR  ServiceType,
   PCHAR  AddendumVersionLabel,
   PQMI_SERVICE_VERSION Version
)
{
   DWORD bytesReturned = 0;
   PQMI_SERVICE_VERSION pVersion;
   PCHAR p;
   PCHAR ioBuffer;

   ioBuffer = (PCHAR)malloc(sizeof(QMI_SERVICE_VERSION)+256);
   if (ioBuffer == NULL)
   {
      return FALSE;
   }
   ZeroMemory(ioBuffer, sizeof(QMI_SERVICE_VERSION)+256);

   if (DeviceIoControl(
                         ServiceHandle,
                         IOCTL_QCDEV_QMI_GET_SVC_VER_EX,
                         &ServiceType,
                         sizeof(UCHAR),
                         (LPVOID)ioBuffer,
                         (sizeof(QMI_SERVICE_VERSION)+256),
                         &bytesReturned,
                         NULL
                      )
      )
   {
      pVersion = (PQMI_SERVICE_VERSION)ioBuffer;
      Version->BaseMajor = pVersion->BaseMajor;
      Version->BaseMinor = pVersion->BaseMinor;
      Version->AddendumMajor = pVersion->AddendumMajor;
      Version->AddendumMinor = pVersion->AddendumMinor;

      if (AddendumVersionLabel != NULL)
      {
         p = ioBuffer + sizeof(QMI_SERVICE_VERSION);
         // strcpy(AddendumVersionLabel, p);
         strncpy_s(AddendumVersionLabel, sizeof(QMI_SERVICE_VERSION)+256, p, _TRUNCATE);
      }
      free(ioBuffer);

      return TRUE;
   }

   free(ioBuffer);
   return FALSE;
}  // QCWWAN_GetQmiServiceVersionEx

QCDEVLIB_API BOOL WINAPI QdIoQmi::QCWWAN_GetParentIdByHandle
(
   HANDLE ServiceHandle,
   PULONGLONG ParentId
)
{
   ULONGLONG parentId = 0;
   DWORD bytesReturned = 0;

   if (DeviceIoControl(
                         ServiceHandle,
                         IOCTL_QCDEV_DEVICE_GROUP_INDETIFIER,
                         NULL,
                         0,
                         (LPVOID)&parentId,
                         sizeof(ULONGLONG),
                         &bytesReturned,
                         NULL
                      )
      )
   {
      *ParentId = parentId;
      return TRUE;
   }

   return FALSE;
}  // QCWWAN_GetParentIdByHandle

QCDEVLIB_API BOOL WINAPI QdIoQmi::QCWWAN_GetParentIdByName
(
   PCHAR      DeviceName,
   PULONGLONG ParentId
)
{
   BOOL bResult;
   HANDLE hDevice;

   hDevice = CreateFileA
             (
                DeviceName,
                GENERIC_WRITE | GENERIC_READ,
                FILE_SHARE_READ | FILE_SHARE_WRITE,
                NULL,
                OPEN_EXISTING,
                FILE_ATTRIBUTE_NORMAL,
                NULL
             );

   if (hDevice == INVALID_HANDLE_VALUE)
   {
      QCD_Printf(Error, "GetParentId: ERROR opening control file <%s>: INVALID_HANDLE_VALUE\n",
                  DeviceName);
      return FALSE;
   }

   bResult = QCWWAN_GetParentIdByHandle(hDevice, ParentId);

   CloseHandle(hDevice);  // close control file handle

   return bResult;

}  // QCWWAN_GetParentIdByName

QCDEVLIB_API BOOL WINAPI QdIoQmi::QCWWAN_IsQmiReady(PCHAR DeviceName)
{
   BOOL bResult;
   HANDLE hDevice;
   DWORD bytesReturned = 0;

   hDevice = CreateFileA
             (
                DeviceName,
                GENERIC_WRITE | GENERIC_READ,
                FILE_SHARE_READ | FILE_SHARE_WRITE,
                NULL,
                OPEN_EXISTING,
                FILE_ATTRIBUTE_NORMAL,
                NULL
             );
   if (hDevice == INVALID_HANDLE_VALUE)
   {
      QCD_Printf(Error, "IsQmiReady: ERROR opening control file <%s>: INVALID_HANDLE_VALUE\n",
                  DeviceName);
      return FALSE;
   }

   bResult = DeviceIoControl
             (
                hDevice,
                IOCTL_QCDEV_QMI_READY,
                NULL,
                0,
                NULL,
                0,
                &bytesReturned,
                NULL
             );

   CloseHandle(hDevice);  // close control file handle

   return bResult;
}  // QCWWAN_IsQmiReady

QCDEVLIB_API PPEER_DEV_INFO_HDR WINAPI QdIoQmi::QCWWAN_GetPeerDevice
(
   PCHAR DeviceName,
   PVOID PeerDevName,
   ULONG Length
)
{
   BOOL bResult;
   HANDLE hDevice;
   DWORD bytesReturned = 0;
   PPEER_DEV_INFO_HDR pPeerDev = NULL;

   ZeroMemory(PeerDevName, Length);

   hDevice = CreateFileA
             (
                DeviceName,
                GENERIC_WRITE | GENERIC_READ,
                FILE_SHARE_READ | FILE_SHARE_WRITE,
                NULL,
                OPEN_EXISTING,
                FILE_ATTRIBUTE_NORMAL,
                NULL
             );
   if (hDevice == INVALID_HANDLE_VALUE)
   {
      QCD_Printf(Error, "GetPeerDevice: ERROR opening control file <%s>: INVALID_HANDLE_VALUE\n",
                  DeviceName);
      return FALSE;
   }

   bResult = DeviceIoControl
             (
                hDevice,
                IOCTL_QCDEV_GET_PEER_DEV_NAME,
                NULL,
                0,
                PeerDevName,
                Length,
                &bytesReturned,
                NULL
             );

   if ((bResult == TRUE) && (bytesReturned >= sizeof(PEER_DEV_INFO_HDR)))
   {
      pPeerDev = (PPEER_DEV_INFO_HDR)PeerDevName;
      if (bytesReturned > Length)
      {
         pPeerDev = NULL;
      }
   }

   CloseHandle(hDevice);  // close control file handle

   return pPeerDev;
}  // QCWWAN_GetPeerDevice

QCDEVLIB_API PVOID QdIoQmi::QCWWAN_GetPeerDiagName(PPEER_DEV_INFO_HDR PeerDev)
{
   PCHAR pLocation;

   pLocation = (PCHAR)PeerDev;
   pLocation += sizeof(PEER_DEV_INFO_HDR);

   return pLocation;

}  // QCWWAN_GetPeerDiagName

QCDEVLIB_API PVOID QdIoQmi::QCWWAN_GetPeerDiagPort(PPEER_DEV_INFO_HDR PeerDev)
{
   PCHAR pLocation;

   pLocation = (PCHAR)PeerDev;
   pLocation += sizeof(PEER_DEV_INFO_HDR);
   pLocation += PeerDev->DeviceNameLength;

   return pLocation;

}  // QCWWAN_GetPeerDiagName

QCDEVLIB_API BOOL QdIoQmi::QCWWAN_GetPrimaryAdapterName
(
   PCHAR DeviceName,
   PVOID NameBuffer,
   ULONG BufferLength
)
{
   BOOL bResult;
   HANDLE hDevice;
   DWORD bytesReturned = 0;

   // Init service file name buffer
   ZeroMemory(NameBuffer, BufferLength);

   hDevice = CreateFileA
             (
                DeviceName,
                GENERIC_WRITE | GENERIC_READ,
                FILE_SHARE_READ | FILE_SHARE_WRITE,
                NULL,
                OPEN_EXISTING,
                FILE_ATTRIBUTE_NORMAL,
                NULL
             );
   if (hDevice == INVALID_HANDLE_VALUE)
   {
      QCD_Printf(Error, "GetPrimaryAdapterName: ERROR opening control file <%s>: INVALID_HANDLE_VALUE\n",
                  DeviceName);
      return FALSE;
   }

   bResult = DeviceIoControl
             (
                hDevice,
                IOCTL_QCDEV_GET_PRIMARY_ADAPTER_NAME,
                NULL,
                0,
                NameBuffer,
                BufferLength,
                &bytesReturned,
                NULL
             );
   CloseHandle(hDevice);  // close control file handle

   return bResult;

}  // QCWWAN_GetPrimaryAdapterName

QCDEVLIB_API BOOL QdIoQmi::QCWWAN_GetQmiQueueSize
(
   HANDLE ServiceHandle,
   PULONG QueueSize
)
{
   BOOL  bResult;
   ULONG value;
   DWORD bytesReturned = 0;

   bResult = DeviceIoControl
             (
                ServiceHandle,
                IOCTL_QCDEV_GET_QMI_QUEUE_SIZE,
                NULL,
                0,
                (PVOID)&value,
                sizeof(ULONG),
                &bytesReturned,
                NULL
             );

   if (bResult == TRUE)
   {
      *QueueSize = value;
   }


   return bResult;
}  // QCWWAN_GetQmiQueueSize

QCDEVLIB_API BOOL QdIoQmi::QCWWAN_PurgeQmiQueue(HANDLE ServiceHandle)
{
   BOOL bResult;
   DWORD bytesReturned = 0;

   bResult = DeviceIoControl
             (
                ServiceHandle,
                IOCTL_QCDEV_PURGE_QMI_QUEUE,
                NULL,
                0,
                NULL,
                0,
                &bytesReturned,
                NULL
             );

   return bResult;
}  // QCWWAN_PurgeQmiQueue
