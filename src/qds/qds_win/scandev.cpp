// Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
// SPDX-License-Identifier: BSD 3-Clause Clear License
// #include "stdafx.h"
#include "../common/qdver.h"
#include "scandev.h"
#include <devpropdef.h>
#include <queue>

static const DEVPROPKEY QDS_DEVPKEY_BusReportedDeviceDesc =
{
    { 0x540b947e, 0x8b40, 0x45bc, { 0xa8, 0xa2, 0x6a, 0x0b, 0x89, 0x4c, 0xbd, 0xa2 } },
    4
};
#include <string>
#include <set>
#include "../common/Utils.h"


using namespace QcDevice;
using namespace Utils;
std::atomic<BOOL> _IsQcomLibusbEnable = FALSE;

namespace QcDevice
{
   namespace
   {
      PVOID CallerContext = NULL;
      INTERNAL_DEV_FEATURE_SETTING FeatureSetting;
      BOOL                  bFeatureSet = FALSE;
      DEVICECHANGE_CALLBACK fNotifyCb = NULL;
      DEVICECHANGE_CALLBACK_N fNotifyCb_N = NULL;
      BOOL                  bMonitorRunning = FALSE;
      HANDLE hStopMonitorEvt, hMonitorStartedEvt;
      HANDLE hAnnouncementEvt, hAnnouncementExitEvt;
      HANDLE hMonitorThread, hAnnouncementThread = 0;
      HANDLE hRegChangeEvt[QC_REG_MAX];
      HKEY hSysKey[QC_REG_MAX];
      LONG lInOperation = 0;
      LONG lInitialized = 0;
      CRITICAL_SECTION opLock, notifyLock;
      LIST_ENTRY ArrivalList, FreeList, NewArrivalList;
      LIST_ENTRY NotifyFreeList, AnnouncementList;
      PQC_NOTIFICATION_STORE NotifyStore;
      std::atomic<BOOL> IsQcomLibusbEnable = FALSE;

      VOID InitializeLists(VOID)
      {
         int i;
         PQC_DEV_ITEM pItem;

         InitializeListHead(&ArrivalList);
         InitializeListHead(&FreeList);
         InitializeListHead(&NewArrivalList);
         InitializeListHead(&NotifyFreeList);
         InitializeListHead(&AnnouncementList);
         for (i = 0; i < MAX_NUM_DEV; i++)
         {
            pItem = (PQC_DEV_ITEM)malloc(sizeof(QC_DEV_ITEM));
            if (pItem == NULL)
            {
               break;
            }
            pItem->Info.Type = QC_DEV_TYPE_NONE;
            pItem->Info.Flag = DEV_FLAG_NONE;
            pItem->Info.IsQCDriver = 0;
            pItem->UserContext = NULL;
            pItem->InterfaceNumber = 0xFF;
            ZeroMemory(pItem->DevDesc, QC_MAX_VALUE_NAME);
            ZeroMemory(pItem->DevDescA, QC_MAX_VALUE_NAME);
            ZeroMemory(pItem->DevNameW, QC_MAX_VALUE_NAME);
            ZeroMemory(pItem->DevNameA, QC_MAX_VALUE_NAME);
            ZeroMemory(pItem->HwId, QC_MAX_VALUE_NAME);
            ZeroMemory(pItem->ParentDev, QC_MAX_VALUE_NAME);
            ZeroMemory(pItem->InterfaceName, QC_MAX_VALUE_NAME);
            ZeroMemory(pItem->SerNum, 256);
            ZeroMemory(pItem->SocVer, 256);
            ZeroMemory(pItem->SerNumMsm, 256);
            ZeroMemory(pItem->PortNos, 256);
            pItem->DevDetails = new std::unordered_map<std::string, std::string>();
            InsertTailList(&FreeList, &(pItem->List));
         }
         NotifyStore = (PQC_NOTIFICATION_STORE)malloc(sizeof(QC_NOTIFICATION_STORE) * 512);
         if (NotifyStore == NULL)
         {
            QCD_Printf(Error, "Failed to alloc notification store\n");
         }
         else
         {
            for (i = 0; i < 512; i++)
            {
               InitializeListHead(&(NotifyStore[i].DevItemChain));
               InsertTailList(&NotifyFreeList, &(NotifyStore[i].List));
            }
         }
      }  // InitializeLists

      VOID CleanupList(PLIST_ENTRY ItemList)
      {
         PLIST_ENTRY pEntry;
         PQC_DEV_ITEM pItem;

         EnterCriticalSection(&opLock);
         while (!IsListEmpty(ItemList))
         {
            pEntry = RemoveHeadList(ItemList);
            pItem = CONTAINING_RECORD(pEntry, QC_DEV_ITEM, List);
            if (pItem->DevDetails) 
            { 
               delete pItem->DevDetails; 
               pItem->DevDetails = nullptr; 
            }
            free(pItem);
         }
         LeaveCriticalSection(&opLock);
      }  // CleanupList

      VOID GetDeviceInterfaceNumberFromHwId(PTSTR HwId, PUINT outInterfaceNumber)
      {
          QCD_Printf(Debug, "GetDeviceInterfaceNumberFromHwId hardware id: %ws\n", HwId);
          wstring hwIdString(HwId);
          size_t position = hwIdString.find(QC_IFACENUM_PREFIX);
          if (position != wstring::npos)
          {
              wstring interfaceString = hwIdString.substr(position + wcslen(QC_IFACENUM_PREFIX), QC_IFACENUM_LENGTH);
              *outInterfaceNumber = stoi(interfaceString);
              QCD_Printf(Debug, "GetDeviceInterfaceNumberFromHwId hardware id: %ws, interface number fetched: %u\n", HwId, *outInterfaceNumber);
          }
      }

      VOID CheckDeviceActiveState(PWCHAR DevName)
      {
          HANDLE hHandle = NULL;
          int retryCount = 5;

          if (StrStrI(DevName, L"\\\\.\\COM") == NULL) return;

          while (retryCount--)
          {
              hHandle = CreateFile(DevName,
                  GENERIC_READ | GENERIC_WRITE,
                  0,
                  0,
                  OPEN_EXISTING,
                  FILE_ATTRIBUTE_NORMAL,
                  0);
              if (hHandle == INVALID_HANDLE_VALUE)
              {
                  DWORD err = GetLastError();
                  QCD_Printf(Info, "CheckDeviceActiveState : Handle creation for %ws error code: %x\n", DevName, err);
                  Sleep(100);
              }
              else
              {
                  break;
              }
          }
          CloseHandle(hHandle);
      }

      VOID TryToAnnounce(PLIST_ENTRY ActiveList, PLIST_ENTRY NewArrival)
      {
         PQC_DEV_ITEM pActive, pArrival;
         PLIST_ENTRY headOfActive, peekActive;
         PLIST_ENTRY headOfArrival, peekArrival;
         PLIST_ENTRY headOfStore;
         PQC_NOTIFICATION_STORE storeBranch = NULL;

         // NOTE: ActiveList is the ArrivalList

         EnterCriticalSection(&opLock);

         if ((!IsListEmpty(ActiveList)) && (!IsListEmpty(NewArrival)))
         {
            headOfActive = ActiveList;
            peekActive = headOfActive->Flink;
            while (peekActive != headOfActive)
            {
               pActive = CONTAINING_RECORD(peekActive, QC_DEV_ITEM, List);
               peekActive = peekActive->Flink;

               headOfArrival = NewArrival;
               peekArrival = headOfArrival->Flink;
               while (peekArrival != headOfArrival)
               {
                  pArrival = CONTAINING_RECORD(peekArrival, QC_DEV_ITEM, List);
                  peekArrival = peekArrival->Flink;
                  if ((StrCmp((LPCWSTR)pActive->DevDesc, (LPCWSTR)pArrival->DevDesc) == 0) &&
                      (StrCmp((LPCWSTR)pActive->DevNameW, (LPCWSTR)pArrival->DevNameW) == 0))
                  {
                     if (StrCmp((LPCWSTR)pActive->SerNum, (LPCWSTR)pArrival->SerNum) == 0)
                     {
                        pActive->Info.Flag = pArrival->Info.Flag = DEV_FLAG_ARRIVAL;
                     }
                  }
               }  // while
            }  // while
         }  // if

         // Get a storeBranch for devices to be announced
         {
            EnterCriticalSection(&notifyLock);
            if (!IsListEmpty(&NotifyFreeList))
            {
               headOfStore = RemoveHeadList(&NotifyFreeList);
               storeBranch = CONTAINING_RECORD(headOfStore, QC_NOTIFICATION_STORE, List);
            }
            LeaveCriticalSection(&notifyLock);
         }

         // New arrivals are Flag==0 on NewArrival
         while (!IsListEmpty(NewArrival))
         {
            headOfArrival = RemoveHeadList(NewArrival);
            pArrival = CONTAINING_RECORD(headOfArrival, QC_DEV_ITEM, List);
            if (pArrival->Info.Flag == DEV_FLAG_NONE)
            {
               PQC_DEV_ITEM pDevice;

               pDevice = (PQC_DEV_ITEM)malloc(sizeof(QC_DEV_ITEM));
               pArrival->Info.Flag = DEV_FLAG_ARRIVAL;
               if ((storeBranch == NULL) || (hAnnouncementThread == 0) || (pDevice == NULL))
               {
                  if (fNotifyCb != NULL)
                  {
                     pArrival->CbParams.DevDesc = (PWCHAR)pArrival->DevDesc;
                     pArrival->CbParams.DevName = (PWCHAR)pArrival->DevNameW;
                     pArrival->CbParams.IfName = (PWCHAR)pArrival->InterfaceName;
                     pArrival->CbParams.Loc = (PWCHAR)pArrival->Location;
                     pArrival->CbParams.ParentLocationInfomation = (PWCHAR)pArrival->ParentLocationInfomation;
                     pArrival->CbParams.DevPath = (PWCHAR)pArrival->DevPath;
                     pArrival->CbParams.SerNum = (PWCHAR)pArrival->SerNum;
                     pArrival->CbParams.SocVer = (PWCHAR)pArrival->SocVer;
                     pArrival->CbParams.SerNumMsm = (PWCHAR)pArrival->SerNumMsm;
                     pArrival->CbParams.HwId = (PWCHAR)pArrival->HwId;
                     pArrival->CbParams.ParentDev = (PWCHAR)pArrival->ParentDev;
                     pArrival->CbParams.Flag = ((ULONG)pArrival->Info.Type << 8 ) |
                                               ((ULONG)1 << 4)                    |
                                               ((ULONG)pArrival->BusType << 16)   |
                                               (ULONG)pArrival->Info.IsQCDriver;
                     pArrival->CbParams.DevDetails = new std::unordered_map<std::string, std::string>();
                     if (pArrival->DevDetails) 
                     { 
                        *(pArrival->CbParams.DevDetails) = *(pArrival->DevDetails); 
                     }
                     if (CallerContext != NULL)
                     {
                        pArrival->UserContext = CallerContext;
                     }
                     fNotifyCb(&(pArrival->CbParams), &pArrival->UserContext);
                     if (pArrival->CbParams.DevDetails) 
                     { 
                        delete pArrival->CbParams.DevDetails; 
                        pArrival->CbParams.DevDetails = nullptr; 
                     }
                     QCD_Printf(Debug, "<%ws> New arrival HWID [%ws]\n", pArrival->DevDesc, pArrival->HwId);
                  }
               }
               else  // send to announcement thread
               {
                  CopyMemory((PVOID)pDevice, (PVOID)pArrival, sizeof(QC_DEV_ITEM));
                  pDevice->Context = (PVOID)pArrival;
                  pDevice->DevDetails = new std::unordered_map<std::string, std::string>();
                  if (pArrival->DevDetails) 
                  { 
                     *(pDevice->DevDetails) = *(pArrival->DevDetails); 
                  }
                  InsertTailList(&(storeBranch->DevItemChain), &pDevice->List);
                  QCD_Printf(Debug, "<%ws> storing arrival HWID in DevItemChain [%ws]\n", pArrival->DevDesc, pArrival->HwId);
               }
               InsertTailList(&ArrivalList, &pArrival->List); // ArrivalList is ActiveList
            }
            else
            {
               if (pArrival->DevDetails) 
               { 
                  delete pArrival->DevDetails; 
                  pArrival->DevDetails = nullptr;
               }
               ZeroMemory(pArrival, sizeof(QC_DEV_ITEM));
               pArrival->DevDetails = new std::unordered_map<std::string, std::string>();
               InsertTailList(&FreeList, &pArrival->List);
            }
         }  // while

         // Those departed are Intersection==0 on ActiveList
         // Now, this list also has new arrivals marked with DEV_FLAG_ARRIVAL
         if (!IsListEmpty(ActiveList))
         {
            headOfActive = ActiveList;
            peekActive = headOfActive->Flink;
            while (peekActive != headOfActive)
            {
               pActive = CONTAINING_RECORD(peekActive, QC_DEV_ITEM, List);
               peekActive = peekActive->Flink;
               if (pActive->Info.Flag == DEV_FLAG_NONE)
               {
                  PQC_DEV_ITEM pDevice;

                  pDevice = (PQC_DEV_ITEM)malloc(sizeof(QC_DEV_ITEM));
                  RemoveEntryList(&pActive->List);
                  if ((storeBranch == NULL) || (hAnnouncementThread == 0) || (pDevice == NULL))
                  {
                     if (fNotifyCb != NULL)
                     {
                        pActive->CbParams.DevDesc = (PWCHAR)pActive->DevDesc;
                        pActive->CbParams.DevName = (PWCHAR)pActive->DevNameW;
                        pActive->CbParams.IfName = (PWCHAR)pActive->InterfaceName;
                        pActive->CbParams.Loc = (PWCHAR)pActive->Location;
                        pActive->CbParams.ParentLocationInfomation = (PWCHAR)pActive->ParentLocationInfomation;
                        pActive->CbParams.DevPath = (PWCHAR)pActive->DevPath;
                        pActive->CbParams.SerNum = (PWCHAR)pActive->SerNum;
                        pActive->CbParams.SocVer = (PWCHAR)pActive->SocVer;
                        pActive->CbParams.SerNumMsm = (PWCHAR)pActive->SerNumMsm;
                        pActive->CbParams.HwId = (PWCHAR)pActive->HwId;
                        pActive->CbParams.ParentDev = (PWCHAR)pActive->ParentDev;
                        pActive->CbParams.Flag = ((ULONG)pActive->Info.Type << 8 ) |
                                                 ((ULONG)pActive->BusType << 16)  |
                                                 pActive->Info.IsQCDriver;
                        pActive->CbParams.DevDetails = new std::unordered_map<std::string, std::string>();
                        if (pActive->DevDetails) 
                        { 
                           *(pActive->CbParams.DevDetails) = *(pActive->DevDetails); 
                        }

                        if (CallerContext != NULL)
                        {
                           pActive->UserContext = CallerContext;
                        }
                        fNotifyCb
                        (
                           &(pActive->CbParams),
                           &pActive->UserContext
                        );
                        if (pActive->CbParams.DevDetails) 
                        { 
                           delete pActive->CbParams.DevDetails; 
                           pActive->CbParams.DevDetails = nullptr; 
                        }
                        QCD_Printf(Debug, "<%ws> ActiveList  HWID [%ws]\n", pArrival->DevDesc, pActive->HwId);
                     }
                  }
                  else
                  {
                     pActive->Info.Flag = DEV_FLAG_DEPARTURE;
                     CopyMemory((PVOID)pDevice, (PVOID)pActive, sizeof(QC_DEV_ITEM));
                     pDevice->DevDetails = new std::unordered_map<std::string, std::string>();
                     if (pActive->DevDetails) 
                     { 
                        *(pDevice->DevDetails) = *(pActive->DevDetails); 
                     }
                     InsertTailList(&(storeBranch->DevItemChain), &pDevice->List);
					 QCD_Printf(Debug, "<%ws> storing departure in DevItemChain HWID [%ws]\n", pArrival->DevDesc, pArrival->HwId);
                  }
                  if (pActive->DevDetails) 
                  { 
                     delete pActive->DevDetails; 
                  }
                  ZeroMemory(pActive, sizeof(QC_DEV_ITEM));
                  pActive->DevDetails = new std::unordered_map<std::string, std::string>();
                  InsertTailList(&FreeList, &pActive->List);
               }
               else
               {
                  pActive->Info.Flag = DEV_FLAG_NONE;  // reset
               }
            }  // while
         }  // if

         LeaveCriticalSection(&opLock);

         // put records to NotifyStore
         if (storeBranch != NULL)
         {
            EnterCriticalSection(&notifyLock);
            if (!IsListEmpty(&(storeBranch->DevItemChain)))
            {
               QCD_Printf(Debug, "update AnnouncementList with storeBranch DevItemChain. Set AnnouncementEvent\n");
               InsertTailList(&AnnouncementList, &(storeBranch->List));
               SetEvent(hAnnouncementEvt);
            }
            else
            {
               InsertHeadList(&NotifyFreeList, &(storeBranch->List));
            }
            LeaveCriticalSection(&notifyLock);
         }
      }  // TryToAnnounce

      VOID MakeAnnouncement(VOID)
      {
         PQC_NOTIFICATION_STORE storeBranch;
         PQC_DEV_ITEM devItem;
         PLIST_ENTRY headOfList;

         QCD_Printf(Info, "Announcing device I:\n");
         EnterCriticalSection(&notifyLock);
         while (!IsListEmpty(&AnnouncementList))
         {
            headOfList = RemoveHeadList(&AnnouncementList);
            storeBranch = CONTAINING_RECORD(headOfList, QC_NOTIFICATION_STORE, List);
            LeaveCriticalSection(&notifyLock);

            while (!IsListEmpty(&(storeBranch->DevItemChain)))
            {
               headOfList = RemoveHeadList(&(storeBranch->DevItemChain));
               devItem = CONTAINING_RECORD(headOfList, QC_DEV_ITEM, List);
               if (fNotifyCb != NULL)
               {
                  if (devItem->Info.Flag == DEV_FLAG_ARRIVAL)
                  {

                     PQC_DEV_ITEM context = (PQC_DEV_ITEM)(devItem->Context);
                     devItem->CbParams.DevDesc = (PWCHAR)devItem->DevDesc;
                     devItem->CbParams.DevName = (PWCHAR)devItem->DevNameW;
                     devItem->CbParams.IfName = (PWCHAR)devItem->InterfaceName;
                     devItem->CbParams.Loc = (PWCHAR)devItem->Location;
                     devItem->CbParams.ParentLocationInfomation = (PWCHAR)devItem->ParentLocationInfomation;
                     devItem->CbParams.DevPath = (PWCHAR)devItem->DevPath;
                     devItem->CbParams.SerNum = (PWCHAR)devItem->SerNum;
                     devItem->CbParams.SocVer = (PWCHAR)devItem->SocVer;
                     devItem->CbParams.SerNumMsm = (PWCHAR)devItem->SerNumMsm;
                     devItem->CbParams.HwId = (PWCHAR)devItem->HwId;
                     devItem->CbParams.ParentDev = (PWCHAR)devItem->ParentDev;
                     devItem->CbParams.Flag = ((ULONG)devItem->Info.Type << 8) |
                                              ((ULONG)devItem->BusType << 16)  |
                                              ((ULONG)1 << 4)                  |
                                              (ULONG)devItem->Info.IsQCDriver;
                     devItem->CbParams.DevDetails = new std::unordered_map<std::string, std::string>();
                     if (devItem->DevDetails) 
					 { 
					    *(devItem->CbParams.DevDetails) = *(devItem->DevDetails); 
					 }
					 CheckDeviceActiveState(devItem->CbParams.DevName);
                     QCD_Printf(Info, "=> ARRIVAL: <%ws> pItem 0x%p - 0x%p - 0x%p\n", (PWCHAR)devItem->DevDesc,
                                 devItem, devItem->Context, &(devItem->CbParams));
                     if (CallerContext != NULL)
                     {
                        devItem->UserContext = CallerContext;
                     }
                     fNotifyCb
                     (
                        &(devItem->CbParams),
                        &devItem->UserContext
                     );
                     context->UserContext = devItem->UserContext; // save user context ID
                     QCD_Printf(Debug, "==>>   HWID [%ws]\n", devItem->HwId);
                  }
                  else  // DEV_FLAG_DEPARTURE
                  {
                     devItem->CbParams.DevDesc = (PWCHAR)devItem->DevDesc;
                     devItem->CbParams.DevName = (PWCHAR)devItem->DevNameW;
                     devItem->CbParams.IfName = (PWCHAR)devItem->InterfaceName;
                     devItem->CbParams.Loc = (PWCHAR)devItem->Location;
                     devItem->CbParams.ParentLocationInfomation = (PWCHAR)devItem->ParentLocationInfomation;
                     devItem->CbParams.DevPath = (PWCHAR)devItem->DevPath;
                     devItem->CbParams.SerNum = (PWCHAR)devItem->SerNum;
                     devItem->CbParams.SocVer = (PWCHAR)devItem->SocVer;
                     devItem->CbParams.SerNumMsm = (PWCHAR)devItem->SerNumMsm;
                     devItem->CbParams.HwId = (PWCHAR)devItem->HwId;
                     devItem->CbParams.ParentDev = (PWCHAR)devItem->ParentDev;
                     devItem->CbParams.Flag = ((ULONG)devItem->Info.Type << 8) |
                                              ((ULONG)devItem->BusType << 16)  |
                                              (ULONG)devItem->Info.IsQCDriver;
                     devItem->CbParams.DevDetails = new std::unordered_map<std::string, std::string>();
                     if (devItem->DevDetails) 
                     { 
                        *(devItem->CbParams.DevDetails) = *(devItem->DevDetails); 
                     }
                     QCD_Printf(Info, "DEPARTURE: <%ws>\n", (PWCHAR)devItem->DevDesc);

                     if (CallerContext != NULL)
                     {
                        devItem->UserContext = CallerContext;
                     }
                     fNotifyCb
                     (
                        &(devItem->CbParams),
                        &devItem->UserContext
                     );
                     QCD_Printf(Debug, "===>>>   HWID [%ws]\n", devItem->HwId);
                  }
               }
               if (devItem->CbParams.DevDetails) 
               { 
                  delete devItem->CbParams.DevDetails; 
                  devItem->CbParams.DevDetails = nullptr; 
               }
               if (devItem->DevDetails) 
               { 
                  delete devItem->DevDetails; 
                  devItem->DevDetails = nullptr; 
               }
               free(devItem);
            }
            EnterCriticalSection(&notifyLock);
            InsertTailList(&NotifyFreeList, &(storeBranch->List)); // recycle
         }
         LeaveCriticalSection(&notifyLock);
      }  // MakeAnnouncement

      VOID MakeAnnouncement_N(VOID)
      {
         PQC_NOTIFICATION_STORE storeBranch;
         PQC_DEV_ITEM devItem;
         PLIST_ENTRY headOfList;

         QCD_Printf(Info, "Announcing device II:\n");
         EnterCriticalSection(&notifyLock);
         while (!IsListEmpty(&AnnouncementList))
         {
            headOfList = RemoveHeadList(&AnnouncementList);
            storeBranch = CONTAINING_RECORD(headOfList, QC_NOTIFICATION_STORE, List);
            LeaveCriticalSection(&notifyLock);

            while (!IsListEmpty(&(storeBranch->DevItemChain)))
            {
               headOfList = RemoveHeadList(&(storeBranch->DevItemChain));
               devItem = CONTAINING_RECORD(headOfList, QC_DEV_ITEM, List);
               if (fNotifyCb != NULL)
               {
                  if (devItem->Info.Flag == DEV_FLAG_ARRIVAL)
                  {
                     PQC_DEV_ITEM context = (PQC_DEV_ITEM)(devItem->Context);

                     devItem->CbParams.DevDesc = (PWCHAR)devItem->DevDesc;
                     devItem->CbParams.DevName = (PWCHAR)devItem->DevNameW;
                     devItem->CbParams.IfName = (PWCHAR)devItem->InterfaceName;
                     devItem->CbParams.Loc = (PWCHAR)devItem->Location;
                     devItem->CbParams.ParentLocationInfomation = (PWCHAR)devItem->ParentLocationInfomation;
                     devItem->CbParams.DevPath = (PWCHAR)devItem->DevPath;
                     devItem->CbParams.SerNum = (PWCHAR)devItem->SerNum;
                     devItem->CbParams.SocVer = (PWCHAR)devItem->SocVer;
                     devItem->CbParams.SerNumMsm = (PWCHAR)devItem->SerNumMsm;
                     devItem->CbParams.HwId = (PWCHAR)devItem->HwId;
                     devItem->CbParams.ParentDev = (PWCHAR)devItem->ParentDev;
                     devItem->CbParams.Flag = (((ULONG)devItem->Info.Type << 8) |
                                               ((ULONG)1 << 4) |
                                               (ULONG)devItem->Info.IsQCDriver);
                     devItem->CbParams.DevDetails = new std::unordered_map<std::string, std::string>();
                     if (devItem->DevDetails) 
					 { 
					    *(devItem->CbParams.DevDetails) = *(devItem->DevDetails); 
					 }
					 CheckDeviceActiveState(devItem->CbParams.DevName);
                     QCD_Printf(Info, "==>> ARRIVAL: <%ws>\n", (PWCHAR)devItem->DevDesc);
                     if (CallerContext != NULL)
                     {
                        devItem->UserContext = CallerContext;
                     }
                     fNotifyCb
                     (
                        &(devItem->CbParams),
                        &devItem->UserContext
                     );
                     context->UserContext = devItem->UserContext; // save user context ID
                     QCD_Printf(Debug, "====>>>>   HWID [%ws]\n", devItem->HwId);
                  }
                  else  // DEV_FLAG_DEPARTURE
                  {
                     devItem->CbParams.DevDesc = (PWCHAR)devItem->DevDesc;
                     devItem->CbParams.DevName = (PWCHAR)devItem->DevNameW;
                     devItem->CbParams.IfName = (PWCHAR)devItem->InterfaceName;
                     devItem->CbParams.Loc = (PWCHAR)devItem->Location;
                     devItem->CbParams.ParentLocationInfomation = (PWCHAR)devItem->ParentLocationInfomation;
                     devItem->CbParams.DevPath = (PWCHAR)devItem->DevPath;
                     devItem->CbParams.SerNum = (PWCHAR)devItem->SerNum;
                     devItem->CbParams.SocVer = (PWCHAR)devItem->SocVer;
                     devItem->CbParams.SerNumMsm = (PWCHAR)devItem->SerNumMsm;
                     devItem->CbParams.HwId = (PWCHAR)devItem->HwId;
                     devItem->CbParams.ParentDev = (PWCHAR)devItem->ParentDev;
                     devItem->CbParams.Flag = (((ULONG)devItem->Info.Type << 8) | (ULONG)devItem->Info.IsQCDriver);
                     devItem->CbParams.DevDetails = new std::unordered_map<std::string, std::string>();
                     if (devItem->DevDetails) 
					 { 
					    *(devItem->CbParams.DevDetails) = *(devItem->DevDetails); 
					 }
                     QCD_Printf(Info, "DEPARTURE: <%ws>\n", (PWCHAR)devItem->DevDesc);
                     if (CallerContext != NULL)
                     {
                        devItem->UserContext = CallerContext;
                     }
                     fNotifyCb
                     (
                        &(devItem->CbParams),
                        &devItem->UserContext
                     );
                     QCD_Printf(Debug, "=====>>>>>   HWID [%ws]\n", devItem->HwId);
                  }
               }
               if (devItem->CbParams.DevDetails) 
               { 
                  delete devItem->CbParams.DevDetails; 
                  devItem->CbParams.DevDetails = nullptr; 
               }
               if (devItem->DevDetails) 
               { 
                  delete devItem->DevDetails; 
                  devItem->DevDetails = nullptr; 
               }
               free(devItem);
            }
            EnterCriticalSection(&notifyLock);
            InsertTailList(&NotifyFreeList, &(storeBranch->List)); // recycle
         }
         LeaveCriticalSection(&notifyLock);
      }  // MakeAnnouncement_N

      DWORD WINAPI AnnouncementThread(PVOID Context)
      {
         DWORD status = WAIT_OBJECT_0;

         while (bMonitorRunning == TRUE)  // set in StartDeviceMonitor()
         {
            status = WaitForSingleObject(hAnnouncementEvt, 500);
            if (status == WAIT_OBJECT_0)
            {
               if (fNotifyCb_N != NULL)
               {
                  fNotifyCb_N();
               }
               else
               {
                  MakeAnnouncement();
               }
            }
         }
         SetEvent(hAnnouncementExitEvt);
         return 0;
      }  // AnnouncementThread

      BOOL MonitorDeviceChange(VOID)
      {
         static WCHAR keyPath[REG_HW_ID_SIZE];
         LONG entries, result;
         static DWORD status = WAIT_OBJECT_0;
         DWORD retCode;

         // QCD_Printf("-->MonitorDeviceChange\n");

         // Make sure events are created only once
         if ((entries = InterlockedIncrement(&lInitialized)) == 1)
         {
            int i;

            for (i = QC_REG_DEVMAP; i < QC_REG_MAX; i++)
            {
               hRegChangeEvt[i] = CreateEvent(NULL, FALSE, FALSE, NULL);
               if (hRegChangeEvt[i] == NULL)
               {
                  QCD_Printf(Error, "CreateEvent failed: 0x%x\n", GetLastError());
               }
               else if (i == QC_REG_DEVMAP)
               {
                  // trigger an immediate 1st-time scan later in this function
                  SetEvent(hRegChangeEvt[i]);
               }
            }

            // not useful because ADB driver does not make reg change on arrival/departure
            StringCchCopy(keyPath, MAX_PATH, TEXT(QC_REG_SW_KEY_ADB));
            retCode = RegOpenKeyEx
                      (
                         HKEY_LOCAL_MACHINE,
                         keyPath,
                         0,
                         KEY_READ,
                         &hSysKey[QC_REG_ADB]
                      );
            if (retCode != ERROR_SUCCESS)
            {
               hSysKey[QC_REG_ADB] = 0;
               QCD_Printf(Error, "Error: failed to open reg for SW_ADB\n");
            }

            StringCchCopy(keyPath, MAX_PATH, TEXT(QC_REG_SW_KEY_USB));
            retCode = RegOpenKeyEx
                      (
                         HKEY_LOCAL_MACHINE,
                         keyPath,
                         0,
                         KEY_READ,
                         &hSysKey[QC_REG_USB]
                      );
            if (retCode != ERROR_SUCCESS)
            {
               hSysKey[QC_REG_USB] = 0;
               QCD_Printf(Error, "Error: failed to open reg for SW_USB\n");
            }

            StringCchCopy(keyPath, MAX_PATH, TEXT(QC_REG_KEY_DEVMAP));
            retCode = RegOpenKeyEx
                      (
                         HKEY_LOCAL_MACHINE,
                         keyPath,
                         0,
                         KEY_READ,
                         &hSysKey[QC_REG_DEVMAP]
                      );
            if (retCode != ERROR_SUCCESS)
            {
               hSysKey[QC_REG_DEVMAP] = 0;
               QCD_Printf(Error, "Error: failed to open reg for DEVMAP\n");
            }

            StringCchCopy(keyPath, MAX_PATH, TEXT(QC_REG_SW_KEY_MDM));
            retCode = RegOpenKeyEx
                      (
                         HKEY_LOCAL_MACHINE,
                         keyPath,
                         0,
                         KEY_READ,
                         &hSysKey[QC_REG_MDM]
                      );
            if (retCode != ERROR_SUCCESS)
            {
               hSysKey[QC_REG_MDM] = 0;
               QCD_Printf(Error, "Error: failed to open reg for MDM\n");
            }

            StringCchCopy(keyPath, MAX_PATH, TEXT(QC_REG_SW_KEY_NET));
            retCode = RegOpenKeyEx
                      (
                         HKEY_LOCAL_MACHINE,
                         keyPath,
                         0,
                         KEY_READ,
                         &hSysKey[QC_REG_NET]
                      );
            if (retCode != ERROR_SUCCESS)
            {
               hSysKey[QC_REG_NET] = 0;
               QCD_Printf(Error, "Error: failed to open reg for NET\n");
            }

            StringCchCopy(keyPath, MAX_PATH, TEXT(QC_REG_SW_KEY_PORTS));
            retCode = RegOpenKeyEx
                      (
                         HKEY_LOCAL_MACHINE,
                         keyPath,
                         0,
                         KEY_READ,
                         &hSysKey[QC_REG_PORTS]
                      );
            if (retCode != ERROR_SUCCESS)
            {
               hSysKey[QC_REG_PORTS] = 0;
               QCD_Printf(Error, "Error: failed to open reg for PORTS\n");
            }

            StringCchCopy(keyPath, MAX_PATH, TEXT(QC_REG_SW_KEY_LIBUSB));
            retCode = RegOpenKeyEx
                      (
                         HKEY_LOCAL_MACHINE,
                         keyPath,
                         0,
                         KEY_READ,
                         &hSysKey[QC_REG_LIBUSB]
                      );
            if (retCode != ERROR_SUCCESS)
            {
               hSysKey[QC_REG_LIBUSB] = 0;
               QCD_Printf(Error, "Error: failed to open reg for LIBUSB\n");
            }

         }
         else if (entries > 1)
         {
            InterlockedDecrement(&lInitialized);
         }

         if (status != WAIT_TIMEOUT)
         {
            int i;

            for (i = QC_REG_DEVMAP; i < QC_REG_MAX; i++)
            {
               if (hSysKey[i] != 0)
               {
                  result = RegNotifyChangeKeyValue
                           (
                              hSysKey[i],
                              TRUE,
                              REG_NOTIFY_CHANGE_LAST_SET,
                              hRegChangeEvt[i],
                              TRUE
                           );
                  if (result != ERROR_SUCCESS)
                  {
                     QCD_Printf(Error, "Failed to monitor reg location %d - 0x%x\n", i, GetLastError());
                  }
               }
               else
               {
                  QCD_Printf(Debug, "Skip to monitor reg location %d\n", i);
               }
            }
            status = WaitForMultipleObjects(QC_REG_MAX, hRegChangeEvt, FALSE, FeatureSetting.TimerInterval);
            // QCD_Printf("Reg change detected: 0x%x\n", status);
         }
         else
         {
            SetEvent(hRegChangeEvt[0]);  // scan on timer
            status = WaitForMultipleObjects(QC_REG_MAX, hRegChangeEvt, FALSE, FeatureSetting.TimerInterval);
            // QCD_Printf("timer off: 0x%x\n", status);
         }

         // QCD_Printf("<--MonitorDeviceChange: ST 0x%x\n", status);

         return status; // WAIT_OBJECT_0);

      }  // MonitorDeviceChange

      PBYTE GetPortNameFromHwKey(PTSTR LocationPath)
      {
         WCHAR keyPath[REG_HW_ID_SIZE];
         static BYTE  regValue[QC_MAX_VALUE_NAME];
         DWORD regLen = QC_MAX_VALUE_NAME;
         HKEY  hHwKey;
         //BOOL bResult = FALSE;
         DWORD retCode;

         StringCchCopy(keyPath, MAX_PATH, TEXT(QC_REG_HW_KEY_PREF));
         StringCchCat(keyPath, MAX_PATH, LocationPath);
         StringCchCat(keyPath, MAX_PATH, TEXT(QC_REG_HW_KEY_PARAM));

         retCode = RegOpenKeyEx
                   (
                      HKEY_LOCAL_MACHINE,
                      keyPath,
                      0,
                      KEY_READ,
                      &hHwKey
                   );

         if (retCode == ERROR_SUCCESS)
         {
            retCode = RegQueryValueEx
                      (
                         hHwKey,
                         TEXT(DEV_PORT_NAME),
                         NULL,         // reserved
                         NULL,         // returned type
                         (LPBYTE)regValue,
                         &regLen
                      );
            RegCloseKey(hHwKey);

            if (retCode == ERROR_SUCCESS)
            {
               return (PBYTE)regValue;
            }
         }
         return NULL;
      }  // GetPortNameFromHwKey

      VOID GetNetInterfaceName(PTSTR NetCfgInstanceId, PTSTR IfName)
      {
         HKEY hTestKey;
         CHAR fullKeyName[QC_MAX_VALUE_NAME];
         //BOOL ret = FALSE;

         StringCchCopy((PTSTR)fullKeyName, QC_MAX_VALUE_NAME/2, TEXT(QC_NET_CONNECTION_REG_KEY));
         StringCchCat((PTSTR)fullKeyName, QC_MAX_VALUE_NAME/2, NetCfgInstanceId);
         StringCchCat((PTSTR)fullKeyName, QC_MAX_VALUE_NAME/2, TEXT("\\"));
         StringCchCat((PTSTR)fullKeyName, QC_MAX_VALUE_NAME/2, TEXT("Connection"));

         if (RegOpenKeyEx(HKEY_LOCAL_MACHINE, (PTSTR)fullKeyName, 0, KEY_READ, &hTestKey) == ERROR_SUCCESS)
         {
            DWORD retCode;
            DWORD valueNameLen = QC_MAX_VALUE_NAME/2;

            retCode = RegQueryValueEx
                      (
                         hTestKey,
                         TEXT("Name"),
                         NULL,         // reserved
                         NULL,         // returned type
                         (LPBYTE)IfName,
                         &valueNameLen
                      );

            if (retCode == ERROR_SUCCESS )
            {
               // QCD_Printf("GetNetInterfaceNames: InterfaceName: [%ws]\n", IfName);
            }
            else
            {
               QCD_Printf(Error, "GetNetInterfaceNames: error 0x%x\n", GetLastError());
            }

            RegCloseKey(hTestKey);
         }
         else
         {
            QCD_Printf(Error, "GetNetInterfaceName: faild to open registry 0x%x\n", GetLastError());
         }
      }  // GetNetInterfaceName

      void MatchCaseSerNum(PTSTR InstanceId, PTSTR pSerNumPos, PVOID SerNum)
      {
         //Store serial number in instance id as serial number
         StringCchCopy((PTSTR)SerNum, 128, pSerNumPos);

         HKEY  hSwKey;
         WCHAR keyPath[MAX_PATH];
         CHAR matchingDeviceId[QC_MAX_VALUE_NAME];

         StringCchCopy(keyPath, MAX_PATH, TEXT(QC_REG_HW_KEY_PREF));
         ZeroMemory(matchingDeviceId, QC_MAX_VALUE_NAME);
         StringCchCopyN((PTSTR)matchingDeviceId, QC_MAX_VALUE_NAME / 2, InstanceId, lstrlen(InstanceId) - lstrlen(pSerNumPos));
         StringCchCat(keyPath, MAX_PATH, (PTSTR)matchingDeviceId);

         DWORD retCode = RegOpenKeyEx
            (
            HKEY_LOCAL_MACHINE,
            keyPath,
            0,
            KEY_READ,
            &hSwKey
            );
         if (retCode == ERROR_SUCCESS)
         {
            WCHAR keyName[256];//Max key name is 256 WORD
            DWORD keyLength = 512;//Max key name is 512 CHAR
            WCHAR matchingSerNum[128];//Serial Number is 128 WORD
            WCHAR orgSerNumUpper[128];//Serial Number is 128 WORD
            StringCchCopy(orgSerNumUpper, 128, pSerNumPos);
            _wcsupr_s(orgSerNumUpper, 128);
            DWORD index = 0;
            //Go through the enum key
            while (RegEnumKeyEx(hSwKey, index, keyName, &keyLength, NULL, NULL, NULL, NULL) == ERROR_SUCCESS)
            {
               //Compare and see if the subkey match the instand id in Capital
               StringCchCopy(matchingSerNum, 128, keyName);
               _wcsupr_s(matchingSerNum, 128);
               if (0 == StrCmp(matchingSerNum, orgSerNumUpper))
               {
                  //Store key name as serial number
                  StringCchCopy((PTSTR)SerNum, 128, keyName);
                  break;
               }
               index++;
               keyLength = 512;
               ZeroMemory(keyName, keyLength);
            }
            RegCloseKey(hSwKey);
         }
      }

      PBYTE ValidateDevice
      (
         PTSTR DriverKey,
         PVOID InstanceId,
         PVOID InterfaceName,  // optional, interface name of a NIC
         PVOID SerNum,
         PVOID SocVer,
         PVOID DevDetails,
         PVOID SerNumMsm,
         PVOID DevCID,
         PVOID ParentDev,
         PULONG Protocol,
         PLONG Mtu,
         int DevType,
         BOOL *IsActive,
         BOOL *IsQCDriver,
         ULONG DevStatus
      )
      {
         WCHAR sysKeyPath[REG_HW_ID_SIZE];
		 WCHAR userKeyPath[REG_HW_ID_SIZE];
         static BYTE  regValue[QC_MAX_VALUE_NAME];
         DWORD regLen = QC_MAX_VALUE_NAME;
         HKEY  hSwSysKey, hSwUserKey;
         BOOL bResult = FALSE;
         DWORD retCode, retCodeUser;

         StringCchCopy(sysKeyPath, MAX_PATH, TEXT(QC_REG_SW_KEY));
         StringCchCat(sysKeyPath, MAX_PATH, DriverKey);
         StringCchCopy(userKeyPath, MAX_PATH, TEXT(QC_REG_SW_KEY_USER));
         StringCchCat(userKeyPath, MAX_PATH, DriverKey);

		// Open device software key
         retCode = RegOpenKeyEx
                   (
                      HKEY_LOCAL_MACHINE,
                      sysKeyPath,
                      0,
                      KEY_READ,
                      &hSwSysKey
                   );
         if (retCode != ERROR_SUCCESS)
         {
            QCD_Printf(Debug, "%s: Failed to open/create registry key SysKey I: %x\n", __func__, retCode);
         }
         else {
            QCD_Printf(Debug, "ValidateDevice: system DriverKey <%ws>\n", sysKeyPath);
         }

         retCodeUser = RegOpenKeyEx
                   (
                       HKEY_CURRENT_USER,
                       userKeyPath,
                       0,
                       KEY_READ,
                       &hSwUserKey
                   );
         if (retCodeUser != ERROR_SUCCESS) {
             QCD_Printf(Debug, "%s: Failed to open/create registry key UserKey II: %x\n", __func__, retCodeUser);
         }
         else {
             QCD_Printf(Debug, "%s: Validate New registry UserKey path: %ws\n", __func__, userKeyPath);
         }

         if (retCode == ERROR_SUCCESS)
         {
            retCode = RegQueryValueEx
                      (
                         hSwSysKey,
                         TEXT(QC_SPEC_STAMP),
                         NULL,         // reserved
                         NULL,         // returned type
                         (LPBYTE)regValue,
                         &regLen
                      );
            if (retCode == ERROR_SUCCESS)
            {
               DWORD v = *((DWORD *)regValue);
               // QCD_Printf("   QC_SPEC_STAMP: %u for <%ws>\n", v, sysKeyPath);

               if (v == 1)
               {
                  *IsActive = TRUE;
                  FeatureSetting.TimerInterval = 1000; // INFINITE; // ADB device needs timer
               }
               else
               {
                  *IsActive = FALSE;
               }

               if ((DevStatus & DN_STARTED) != 0)
               {
                  *IsActive = TRUE;  // Best Effort -- let system devnode status make final call but may not be reliable
               }
               *IsQCDriver = TRUE;
            }
            else
            {
               // not a QC driver
               *IsActive = TRUE;
               *IsQCDriver = FALSE;
            }

            regLen = 256;
            retCode = RegQueryValueEx
                      (
                         hSwSysKey,
                         TEXT(QC_SPEC_SERNUM),
                         NULL,         // reserved
                         NULL,         // returned type
                         (LPBYTE)SerNum,
                         &regLen
                      );
            if (retCode != ERROR_SUCCESS)
            {
                regLen = 256;
                retCode = RegQueryValueEx
                (
                    hSwUserKey,
                    TEXT(QC_SPEC_SERNUM),
                    NULL,         // reserved
                    NULL,         // returned type
                    (LPBYTE)SerNum,
                    &regLen
                );
                if (retCode != ERROR_SUCCESS) {
                    // or we use InstanceId instead for USB single function device (for Fastboot)
                    if ((InstanceId != NULL) &&
                        (StrStrW((PTSTR)InstanceId, TEXT(BUS_TEST_USB)) != NULL) &&  // is a USB device
                        (StrStrW((PTSTR)InstanceId, TEXT("&MI_")) == NULL))          // is not a composite device
                    {
                        PTSTR p = (PTSTR)InstanceId, p1;

                        while (p1 = StrStrW(p, TEXT("\\")))
                        {
                            p = ++p1;
                        }

                        if (StrStrW(p, TEXT("&")) != NULL)
                        {
                            ZeroMemory(SerNum, 256);
                            QCD_Printf(Debug, "ValidateDevice: no SerNum retrieved from <%ws>\n", p);
                        }
                        else
                        {
                            MatchCaseSerNum((PTSTR)InstanceId, p, SerNum);
                            QCD_Printf(Debug, "ValidateDevice: use <%ws> as SerNum\n", (PTSTR)SerNum);

                            // revert above changes to unblock Axiom due to unreliability of serial number population
                            // by Windows system in single-function mode
                            // ZeroMemory(SerNum, 256);
                            // QCD_Printf("ValidateDevice: no SerNum retrieved from <%ws>\n", p);
                        }
                    }
                    else
                    {
                        ZeroMemory(SerNum, 256);
                    }
                }
            }

            regLen = 256;
            retCode = RegQueryValueEx
                      (
                         hSwSysKey,
                         TEXT(QC_SPEC_SERNUM_MSM),
                         NULL,         // reserved
                         NULL,         // returned type
                         (LPBYTE)SerNumMsm,
                         &regLen
                      );
            if (retCode != ERROR_SUCCESS)
            {
               QCD_Printf(Debug, "Syskey registry Status: %d, SerNumMsm <%ws> and sernum: %ws\n", retCode, (PTSTR)SerNumMsm, (PTSTR)SerNum);
               ZeroMemory(SerNumMsm, 256);
               regLen = 256;
               retCode = RegQueryValueEx
               (
                   hSwUserKey,
                   TEXT(QC_SPEC_SERNUM_MSM),
                   NULL,         // reserved
                   NULL,         // returned type
                   (LPBYTE)SerNumMsm,
                   &regLen
               );
               if (retCode != ERROR_SUCCESS)
               {
                   ZeroMemory(SerNumMsm, 256);
               }
               else {
                   QCD_Printf(Debug, "UserKey registry Status: %d, fetch SerNum:Msm  <%ws>:<%ws>\n", retCode, (PTSTR)SerNum, (PTSTR)SerNumMsm);
               }
            }
            else
            {
                PTSTR p = StrStrW((PTSTR)SerNumMsm, TEXT("_JID:"));
                if (p != NULL)
                {
                    *p = L'\0';
                }
            }
            // retrieve CID
            regLen = 256;
            retCode = RegQueryValueEx
                      (
                         hSwSysKey,
                         TEXT(QC_SPEC_CID),
                         NULL,         // reserved
                         NULL,         // returned type
                         (LPBYTE)DevCID,
                         &regLen
                      );
            if (retCode != ERROR_SUCCESS)
            {
               ZeroMemory(DevCID, 256);
            }

            // retrieve SOCVER
            regLen = 256;
            retCode = RegQueryValueEx
            (
                hSwSysKey,
                TEXT(QC_SPEC_SOCVER),
                NULL,         // reserved
                NULL,         // returned type
                (LPBYTE)SocVer,
                &regLen
            );
            if (retCode != ERROR_SUCCESS)
            {
                ZeroMemory(SocVer, 256);
            }

            // retrieve DEV_NAME
            regLen = 256;
            retCode = RegQueryValueEx
            (
                hSwSysKey,
                TEXT(QC_SPEC_DEV_NAME),
                NULL,         // reserved
                NULL,         // returned type
                (LPBYTE)DevDetails,
                &regLen
            );
            if (retCode != ERROR_SUCCESS)
            {
                ZeroMemory(DevDetails, 256);
            }

            // retrieve protocol code
            regLen = sizeof(ULONG);
            retCode = RegQueryValueEx
                      (
                         hSwSysKey,
                         TEXT(QC_SPEC_PROTOC),
                         NULL,         // reserved
                         NULL,         // returned type
                         (LPBYTE)Protocol,
                         &regLen
                      );
            if (retCode != ERROR_SUCCESS)
            {
                regLen = sizeof(ULONG);
                retCode = RegQueryValueEx
                (
                    hSwUserKey,
                    TEXT(QC_SPEC_PROTOC),
                    NULL,         // reserved
                    NULL,         // returned type
                    (LPBYTE)Protocol,
                    &regLen
                );
                if (retCode != ERROR_SUCCESS) {
                    *Protocol = 0;
                }
            }

            // retrieve parent device name
            regLen = 256;
            retCode = RegQueryValueEx
                      (
                         hSwSysKey,
                         TEXT(QC_SPEC_PARENT_DEV),
                         NULL,         // reserved
                         NULL,         // returned type
                         (LPBYTE)ParentDev,
                         &regLen
                      );
            if (retCode != ERROR_SUCCESS)
            {
               ZeroMemory(ParentDev, 256);
            }

            // Retrieve the symbolic link name
            //sysKeyPath[0] = 0; // ??? why are wedoing zero here?
            regLen = QC_MAX_VALUE_NAME;
            if ((DevType == QC_DEV_TYPE_PORTS) || (DevType == QC_DEV_TYPE_MDM))
            {
               if (*IsQCDriver == TRUE)
               {
                  retCode = RegQueryValueEx
                            (
                               hSwSysKey,
                               TEXT(QC_SPEC),
                               NULL,         // reserved
                               NULL,         // returned type
                               (LPBYTE)regValue,
                               &regLen
                            );
               }
               else if ((InstanceId != NULL) && ((FeatureSetting.User.Settings & DEV_FEATURE_INCLUDE_NONE_QC_PORTS) != 0))
               {
                  LPBYTE p;

                  p = GetPortNameFromHwKey((PTSTR)InstanceId);
                  if (p != NULL)
                  {
                     StringCchCopy((PTSTR)regValue, QC_MAX_VALUE_NAME/2, (PTSTR)p);
                     retCode = ERROR_SUCCESS;
                  }
                  else
                  {
                     retCode = ERROR_FILE_NOT_FOUND;
                  }
               }
               else
               {
                  retCode = ERROR_FILE_NOT_FOUND;
               }
            }
            else if (DevType == QC_DEV_TYPE_NET)
            {
               retCode = RegQueryValueEx
                         (
                            hSwSysKey,
                            TEXT("NetCfgInstanceId"),
                            NULL,         // reserved
                            NULL,         // returned type
                            (LPBYTE)regValue,
                            &regLen
                         );
               if (InterfaceName != NULL)
               {
                  GetNetInterfaceName((PTSTR)regValue, (PTSTR)InterfaceName);
               }

               regLen = sizeof(LONG);
               retCode = RegQueryValueEx
                         (
                            hSwSysKey,
                            TEXT(QC_SPEC_MTU),
                            NULL,         // reserved
                            NULL,         // returned type
                            (LPBYTE)Mtu,
                            &regLen
                         );
               if (retCode != ERROR_SUCCESS)
               {
                  *Mtu = 0;
               }
               regLen = QC_MAX_VALUE_NAME;
               retCode = RegQueryValueEx
                         (
                            hSwSysKey,
                            TEXT(QC_SPEC_NET),
                            NULL,         // reserved
                            NULL,         // returned type
                            (LPBYTE)regValue,
                            &regLen
                         );
               if (retCode == ERROR_SUCCESS)
               {
                  *IsActive = TRUE;
               }
               else
               {
                  if ((InstanceId != NULL) && 
                      (StrStrW((PTSTR)InstanceId, TEXT("VID_05C6")) != NULL) &&    // is a QC USB device
                      (StrStrW((PTSTR)InstanceId, TEXT("&MI_")) != NULL))          // is a composite device
                  {
                     *IsActive = TRUE;
                  }
                  else
                  {
                     *IsActive = FALSE;
                     // QCD_Printf("ValidateDevice: RemoveDevice <%ws>\n", InstanceId);
                  }
               }

               // Inspect modem SSR
               if (*IsQCDriver == TRUE)
               {
                  DWORD ssrRetCode;
                  BYTE  ssrValue[QC_MAX_VALUE_NAME];

                  DWORD ssrLen = QC_MAX_VALUE_NAME;
                  ssrRetCode = RegQueryValueEx
                               (
                                  hSwSysKey,
                                  TEXT(QC_SPEC_SSR),
                                  NULL,         // reserved
                                  NULL,         // returned type
                                  (LPBYTE)ssrValue,
                                  &ssrLen
                               );
                  if (ssrRetCode == ERROR_SUCCESS)
                  {
                     DWORD v = *((DWORD *)ssrValue);

                     if (v == 1)
                     {
                        *IsActive = FALSE;
                        QCD_Printf(Info, "ValidateDevice: SSR detected for <%ws>\n", InstanceId);
                     }
                     else
                     {
                        // *isActive should remain what has been set so far
                        QCD_Printf(Debug, "ValidateDevice: SSR changed for <%ws> IsActive %d/%d\n", InstanceId, *IsActive, v);
                     }
                  }
                  else
                  {
                     // *isActive should remain what has been set so far
                     // QCD_Printf("ValidateDevice: SSR does not exist for <%ws> IsActive %d\n", InstanceId, *IsActive);
                  }
               }
            } 
            else if (DevType == QC_DEV_TYPE_USB)
            {
               // for QDSS/DPL online, use FriendlyName, so return NULL 
               retCode = ERROR_FILE_NOT_FOUND; // just assign an error code
            }
            else
            {
               retCode = ERROR_FILE_NOT_FOUND; // just assign an error code
            }

            if (retCode == ERROR_SUCCESS )
            {
               bResult = TRUE;
            }
         }
         QCD_Printf(Debug, "registry: [%ws] == Status: %d, SerNumMsm <%ws> and sernum: %ws, protocol: %ld, IsActive: %d\n", sysKeyPath, retCode, (PTSTR)SerNumMsm, (PTSTR)SerNum, (ULONG)Protocol, *IsActive);
         RegCloseKey(hSwSysKey);
		 RegCloseKey(hSwUserKey);

         if (bResult == TRUE)
         {
            PBYTE pDevNameString = (PBYTE)regValue;

            if (DevType == QC_DEV_TYPE_NET)
            {
               PCHAR pStart, pEnd;

               pStart = (PCHAR)regValue;
               pEnd = pStart + regLen;
               while (pEnd > pStart)
               {
                  if (*pEnd != 0x5C)  // look for '\'
                  {
                     pEnd--;
                  }
                  else
                  {
                     pEnd += 2;
                     pDevNameString = (PBYTE)pEnd;
                     break;
                  }
               }
            }
            return pDevNameString;
         }

         return NULL;
      }  // ValidateDevice

      BOOL IsHexNumber(PVOID NumBuffer)
      {
         PCHAR p = (PCHAR)NumBuffer;
         BOOL retVal = TRUE;

         while ((*p != 0) && (*(p+1) == 0))
         {
            if ((*p >= '0' && *p <= '9') ||
                (*p >= 'A' && *p <= 'F') ||
                (*p >= 'a' && *p <= 'f'))
            {
               // QCD_Printf("examine: %02X/%c\n", *p, *p);
            }
            else
            {
               retVal = FALSE;
               break;
            }
            p += 2;
         }
         return (p == NumBuffer ? FALSE : retVal);
      }  // IsHexNumber

      BOOL FindParent(PVOID HwInstanceId, PVOID ParentDev, PVOID LocationInformation, PVOID PotentialSerNum)
      {
         WCHAR keyPath[REG_HW_ID_SIZE];
         CHAR  container[256], parentContainer[256];
         DWORD containerLen = 256;
         HKEY  hHwKey;
         DWORD retVal;
         BOOL  result = FALSE;

         StringCchCopy(keyPath, MAX_PATH, TEXT(QC_REG_HW_KEY_PREF));
         StringCchCat(keyPath, MAX_PATH, (PTSTR)HwInstanceId);

         // QCD_Printf("FindParent: path <%ws>\n", keyPath);

         // Open instance and retrieve ContainerID
         if (RegOpenKeyExW
             (
                HKEY_LOCAL_MACHINE,
                keyPath,
                0,
                KEY_READ,
                &hHwKey
             ) == ERROR_SUCCESS
            )
         {
            ZeroMemory(container, containerLen);
            retVal = RegQueryValueExW
                     (
                        hHwKey,
                        TEXT("ContainerID"),
                        NULL,
                        NULL,
                        (LPBYTE)container,
                        &containerLen
                     );
            RegCloseKey(hHwKey);
 
            if (retVal == ERROR_SUCCESS )
            {
               PWSTR pEnd;
               LSTATUS openSt;

               // QCD_Printf("FindParent: Container <%ws>\n", (PTSTR)container);

               // revise keyPath
               if ((pEnd = StrStrW((PTSTR)keyPath, TEXT("Enum\\USB\\VID_"))) != NULL)
               {
                  pEnd += 26;
                  *pEnd = 0;
                  // QCD_Printf("FindParent: Rev-path <%ws>\n", keyPath);
                  openSt = RegOpenKeyExW(HKEY_LOCAL_MACHINE, keyPath, KEY_ENUMERATE_SUB_KEYS, KEY_READ, &hHwKey);
                  if (openSt == ERROR_SUCCESS)
                  {
                     DWORD idx = 0;
                     CHAR  subKey[512]; 
                     while (TRUE)
                     {
                        DWORD subKeyLen = 512;
                        ZeroMemory(subKey, subKeyLen);
                        subKeyLen /= 2;
                        retVal = RegEnumKeyEx
                                 (
                                    hHwKey, idx,
                                    (LPWSTR)subKey, &subKeyLen,
                                    NULL,
                                    NULL,
                                    NULL,
                                    NULL
                                 );
                        if (retVal == ERROR_SUCCESS)
                        {
                           HKEY hSubKey;
 
                           // open the subkey
                           // QCD_Printf("FindParent: iteration %d <%ws> ret %d\n", idx, (LPWSTR)subKey, retVal);
                           openSt = RegOpenKeyExW(hHwKey, (LPWSTR)subKey, 0, KEY_READ, &hSubKey);
                           if (openSt == ERROR_SUCCESS)
                           {
                              containerLen = 256;
                              ZeroMemory(parentContainer, containerLen);
                              retVal = RegQueryValueExW
                                       (
                                          hSubKey, TEXT("ContainerID"), NULL, NULL,
                                          (LPBYTE)parentContainer, &containerLen
                                       );
                              if (retVal == ERROR_SUCCESS )
                              {
                                 // QCD_Printf("FindParent: ParentContainer <%ws>\n", parentContainer);
                                 if (StrCmpW((PCWSTR)parentContainer, (PCWSTR)container) == 0)
                                 {
                                    DWORD devLen = 256;
 
                                    retVal = RegQueryValueExW
                                             (
                                                hSubKey, TEXT("FriendlyName"), NULL, NULL,
                                                (LPBYTE)ParentDev, &devLen
                                             );
                                    if (retVal == ERROR_SUCCESS)
                                    {
                                       QCD_Printf(Debug, "FindParent: Parent found <%ws> SubKey <%ws>\n", (PTSTR)ParentDev, subKey);
                                       if (IsHexNumber((PVOID)subKey) == TRUE)
                                       {
                                          StringCchCopy((PTSTR)PotentialSerNum, 128, (PTSTR)subKey);
                                       }
                                       DWORD locationLen = 256;
                                       retVal = RegQueryValueExW
                                                (
                                                    hSubKey, TEXT("LocationInformation"), NULL, NULL,
                                                    (LPBYTE)LocationInformation, &locationLen
                                                );
                                       if (retVal == ERROR_SUCCESS)
                                       {
                                           QCD_Printf(Debug, "FindParent: LocationInformation <%ws>\n", (PTSTR)LocationInformation);
                                       }
                                       else
                                       {
                                          if (GetLastError() != 0)
                                          {
                                             QCD_Printf(Error, "FindParent: LocationInformation failure 0x%x\n", GetLastError());
                                          }
                                       }
                                       result = TRUE;
                                    }
                                    else
                                    {
                                       if (GetLastError() != 0)
                                       {
	                                       QCD_Printf(Error, "FindParent: FriendlyName failure 0x%x\n", GetLastError());
                                       }
                                    }
                                 }
                              }
                              RegCloseKey(hSubKey);
                           }
                        }
                        else
                        {
                           // until retVal == ERROR_NO_MORE_ITEMS (259)
                           if (retVal != ERROR_NO_MORE_ITEMS)
                           {
	                           QCD_Printf(Error, "FindParent: Rev-path <%ws> iteration %d error 0x%x Ret %d\n", keyPath, idx, GetLastError(), retVal);
                           }
                           break;  // out of the loop
                        }
                        idx++;
                     } // while
 
                     RegCloseKey(hHwKey);
                  }
               }
            }
         }

         return result;
      }  // FindParent

      void ParseDevDesc(PWSTR input, std::unordered_map<std::string, std::string>& deviceMap)
      {
          QCD_Printf(Debug, "String:  %ws\n", input);
          if (!input || wcslen(input) == 0)
          {
              QCD_Printf(Error, "ParseDevDesc: Invalid or empty input\n");
              return ;
          }

          // Convert wide string to UTF-8 narrow string for parsing
          int sizeNeeded = WideCharToMultiByte(CP_UTF8, 0, input, -1, nullptr, 0, nullptr, nullptr);
          if (sizeNeeded <= 0)
          {
              QCD_Printf(Error, "ParseDevDesc: WideCharToMultiByte size calc failed\n");
              return ;
          }
          std::string s;
          s.resize(sizeNeeded - 1);
          int conv = WideCharToMultiByte(CP_UTF8, 0, input, -1, &s[0], sizeNeeded, nullptr, nullptr);
          if (conv <= 0)
          {
              QCD_Printf(Error, "ParseDevDesc: WideCharToMultiByte conversion failed\n");
              return ;
          }

          size_t start = 0, n = s.size();
          while (start < n) {
              size_t sep = s.find('_', start);
              size_t end = (sep == std::string::npos) ? n : sep;
              size_t colon = s.find(':', start);

              if (colon != std::string::npos && colon < end) {
                  std::string key = s.substr(start, colon - start);
                  std::string val = s.substr(colon + 1, end - (colon + 1));
                  QCD_Printf(Debug, "DevDetails: %s=%s\n", key.c_str(), val.c_str());
                  deviceMap.emplace(std::move(key), std::move(val));
              }
              start = (sep == std::string::npos) ? n : sep + 1;
          }

          return;
      }

      static std::string GetUsbPortChainFromCmTree(DEVINST devInst)
      {
          std::vector<unsigned> ports; // leaf-to-root; reversed at end
          DEVINST current = devInst;

          // USB spec ≤7 tiers; allow extra iterations to skip ACPI nodes.
          for (int depth = 0; depth < 14; ++depth)
          {
              WCHAR hwIds[512] = {};
              ULONG hwSize = sizeof(hwIds);
              ULONG hwType = 0;
              CONFIGRET cr = CM_Get_DevNode_Registry_Property(
                  current,
                  CM_DRP_HARDWAREID,
                  &hwType,
                  hwIds,
                  &hwSize,
                  0);

              if (cr == CR_SUCCESS)
              {
                  bool isRootHub  = false;
                  bool isUsbNode  = false;

                  // REG_MULTI_SZ: iterate NUL-separated strings
                  for (const WCHAR* s = hwIds; *s; s += wcslen(s) + 1)
                  {
                      if (StrStrIW(s, L"ROOT_HUB") != nullptr)
                      {
                          isRootHub = true;
                          break;
                      }
                      if (_wcsnicmp(s, L"USB\\", 4) == 0)
                          isUsbNode = true;
                  }

                  if (isRootHub)
                      break; // reached root hub — stop without collecting

                  if (isUsbNode)
                  {
                      // Skip USB interface nodes (USB\VID_xxxx&PID_xxxx&MI_xx).
                      // we donot need to count interface sub-nodes.
                      // Interface nodes have "&MI_" in every hardware ID.
                      bool isInterface = false;
                      for (const WCHAR* s = hwIds; *s; s += wcslen(s) + 1)
                      {
                          if (StrStrIW(s, L"&MI_") != nullptr)
                          {
                              isInterface = true;
                              break;
                          }
                      }

                      if (!isInterface)
                      {
                          // Genuine USB composite/function/hub node — collect port
                          ULONG portNum  = 0;
                          ULONG propSize = sizeof(portNum);
                          ULONG propType = 0;
                          cr = CM_Get_DevNode_Registry_Property(
                              current,
                              CM_DRP_ADDRESS,
                              &propType,
                              &portNum,
                              &propSize,
                              0);
                          if (cr == CR_SUCCESS && propType == REG_DWORD)
                              ports.push_back(static_cast<unsigned>(portNum));
                      }
                      // else: interface node — skip address, keep climbing to composite device
                  }
                  // else: ACPI/other node — skip (don't collect address, keep climbing)
              }
              else
              {
                  // No hardware IDs retrievable — probably at host controller; stop
                  break;
              }

              DEVINST parent = 0;
              if (CM_Get_Parent(&parent, current, 0) != CR_SUCCESS)
                  break;
              current = parent;
          }

          if (ports.empty())
              return "";

          std::reverse(ports.begin(), ports.end());

          std::string chain;
          for (size_t i = 0; i < ports.size(); ++i)
          {
              if (i) chain += "-";
              chain += std::to_string(ports[i]);
          }
          return chain;
      }

      VOID ScanDevices(VOID)
      {
         HDEVINFO        devInfoHandle = INVALID_HANDLE_VALUE;
         SP_DEVINFO_DATA devInfoData;
         DWORD           memberIdx = 0;
         CHAR            devClass[REG_HW_ID_SIZE];
         CHAR            driverKey[REG_HW_ID_SIZE];
         CHAR            hwId[QC_MAX_VALUE_NAME];
         CHAR            comptId[QC_MAX_VALUE_NAME];
         CHAR            instanceId[512];
         CHAR            potentialSerNum[256];
         CHAR            serNum[256];
         CHAR            socVer[256];
         CHAR            devDetails[256];
         CHAR            serNumMsm[256];
         CHAR            devCID[256];
         CHAR            parentDev[256];
         CHAR            parentLocationInformation[QC_MAX_VALUE_NAME];
         CHAR            ifName[QC_MAX_VALUE_NAME];
         CHAR            friendlyName[QC_MAX_VALUE_NAME];
         CHAR            busReportedDevDesc[QC_MAX_VALUE_NAME];
         CHAR            location[QC_MAX_VALUE_NAME];
         CHAR            devPath[QC_MAX_VALUE_NAME];
         LONG            mtu;
         ULONG           funcProtocol;
         DWORD           requiredSize;
         BOOL            bResult, bHwIdOK;
         BOOL            bMatch, bExclude;
         UCHAR           devType = QC_DEV_TYPE_NONE, busType = QC_DEV_BUS_TYPE_NONE;
         PBYTE           pDevName = NULL;
         BOOL            bActive = FALSE, bQCDriver = FALSE, bInstanceIdOK = FALSE;
         PQC_DEV_ITEM    pItem = NULL;
         PTSTR           pSwIdx = NULL;
         BOOL            bAdbDetected;
         CONFIGRET       cmRet;
         ULONG           devStatus, problemNum;
         BOOL            bProcessorAdded = FALSE;
         WCHAR           driverKeyPath[REG_HW_ID_SIZE];
         CHAR            driverKeyPathNew[REG_HW_ID_SIZE];
         WCHAR           LocationPath[REG_HW_ID_SIZE];
         DEVPROPTYPE type = DEVPROP_TYPE_EMPTY;
         WCHAR           busRepDescW[QC_MAX_VALUE_NAME];
         std::unordered_map<std::string, std::string> devDetailsMP;


         devInfoHandle = SetupDiGetClassDevsEx
                         (
                            NULL,
                            NULL,  // TEXT("USB")
                            NULL,
                            (DIGCF_PRESENT | DIGCF_ALLCLASSES),
                            NULL,
                            NULL,  // Machine,
                            NULL
                         );
          if (devInfoHandle == INVALID_HANDLE_VALUE)
          {
              QCD_Printf(Error, "SetupDiGetClassDevsEx returned no handle\n");
              return;
          }
          devInfoData.cbSize = sizeof(SP_DEVINFO_DATA);
          while (SetupDiEnumDeviceInfo(devInfoHandle, memberIdx, &devInfoData) == TRUE)
          {
             busType = QC_DEV_BUS_TYPE_NONE;
             pDevName = NULL;
             bInstanceIdOK = FALSE;
             bHwIdOK = FALSE;
             bMatch = bExclude = FALSE;
             ZeroMemory(devClass, REG_HW_ID_SIZE);
             ZeroMemory(driverKey, REG_HW_ID_SIZE);
             ZeroMemory(hwId, QC_MAX_VALUE_NAME);
             ZeroMemory(instanceId, 512);
             ZeroMemory(ifName, QC_MAX_VALUE_NAME);
             ZeroMemory(friendlyName, QC_MAX_VALUE_NAME);
             ZeroMemory(busReportedDevDesc, QC_MAX_VALUE_NAME);
             ZeroMemory(location, QC_MAX_VALUE_NAME);
             ZeroMemory(devPath, QC_MAX_VALUE_NAME);
             ZeroMemory(potentialSerNum, 256);
             ZeroMemory(serNum, 256);
             ZeroMemory(socVer, 256);
             ZeroMemory(serNumMsm, 256);
             ZeroMemory(devCID, 256);
             ZeroMemory(parentDev, 256);
             ZeroMemory(parentLocationInformation, QC_MAX_VALUE_NAME);
             ZeroMemory(driverKeyPath, REG_HW_ID_SIZE);
             ZeroMemory(driverKeyPathNew, REG_HW_ID_SIZE);
             ZeroMemory(LocationPath, REG_HW_ID_SIZE);
             devDetailsMP.clear();
             bAdbDetected = FALSE;
             devType = QC_DEV_TYPE_NONE;
             pSwIdx = NULL;
             devStatus = problemNum = 0;

             bResult = SetupDiGetDeviceRegistryProperty
                       (
                          devInfoHandle,
                          &devInfoData,
                          SPDRP_CLASS,
                          NULL,
                          (LPBYTE)devClass,
                          REG_HW_ID_SIZE,
                          &requiredSize
                       );
             if (bResult == FALSE)
             {
                // QCD_Printf("No class info returned\n");
                memberIdx++;
                continue;
             }
             else
             {
                CharUpper((PTSTR)devClass);
                if (StrStrW((PTSTR)devClass, TEXT(D_CLASS_CPU)) != NULL)
                {
                    // for snapdragon processor support
                    bMatch = !bProcessorAdded;
                }
                else if (((FeatureSetting.User.DeviceClass & DEV_CLASS_NET) != 0)   ||
                    ((FeatureSetting.User.DeviceClass & DEV_CLASS_MDM) != 0)        ||
                    ((FeatureSetting.User.DeviceClass & DEV_CLASS_PORTS) != 0)      ||
                    ((FeatureSetting.User.DeviceClass & DEV_CLASS_LIBUSB) != 0)     ||
                    ((FeatureSetting.User.DeviceClass & DEV_CLASS_USB) != 0)        ||
                    ((FeatureSetting.User.DeviceClass & DEV_CLASS_ADB) != 0))
                {
                   // QCD_Printf("Inspecting DevClass <%ws>\n", (PTSTR)devClass);
                   if (StrStrW((PTSTR)devClass, TEXT(D_CLASS_NET)) != NULL)
                   {
                      // QCD_Printf("NET class <%ws>\n", (PTSTR)devClass);
                      devType = QC_DEV_TYPE_NET;
                      bMatch = TRUE;
                   }
                   else if (StrStrW((PTSTR)devClass, TEXT(D_CLASS_PORTS)) != NULL)
                   {
                      // QCD_Printf("PORTS class <%ws>\n", (PTSTR)devClass);
                      devType = QC_DEV_TYPE_PORTS;
                      bMatch = TRUE;
                   }
                   else if (StrStrW((PTSTR)devClass, TEXT(D_CLASS_LIBUSB)) != NULL)
                   {
                      // QCD_Printf("LIBUSB class <%ws>\n", (PTSTR)devClass);
                      devType = QC_DEV_TYPE_LIBUSB;
                      bMatch = TRUE;
                   }
                   else if (StrStrW((PTSTR)devClass, TEXT(D_CLASS_MODEM)) != NULL)
                   {
                      // QCD_Printf("MDM class <%ws>\n", (PTSTR)devClass);
                      devType = QC_DEV_TYPE_MDM;
                      bMatch = TRUE;
                   }
                   else if (StrStrW((PTSTR)devClass, TEXT(D_CLASS_ADB)) != NULL)
                   {
                      QCD_Printf(Debug, "ADB class <%ws>\n", (PTSTR)devClass);
                      devType = QC_DEV_TYPE_ADB;
                      bAdbDetected = TRUE;
                      bMatch = TRUE;
                   }
                   else if (StrStrW((PTSTR)devClass, TEXT(D_CLASS_USBDEV)) != NULL)
                   {
                      QCD_Printf(Debug, "USBDEV class <%ws>\n", (PTSTR)devClass);
                      devType = QC_DEV_TYPE_USB;
                      bMatch = TRUE;
                   }
                }

                bResult = SetupDiGetDeviceRegistryProperty
                          (
                             devInfoHandle,
                             &devInfoData,
                             SPDRP_HARDWAREID,
                             NULL,
                             (LPBYTE)hwId,
                             QC_MAX_VALUE_NAME,
                             &requiredSize
                          );
                if (bResult == TRUE)
                {
                   bHwIdOK = TRUE;
                }

                bResult = SetupDiGetDeviceRegistryProperty
                          (
                             devInfoHandle,
                             &devInfoData,
                             SPDRP_COMPATIBLEIDS,
                             NULL,
                             (LPBYTE)comptId,
                             QC_MAX_VALUE_NAME,
                             &requiredSize
                          );

                if (bResult == TRUE)
                {
                   // MBIM
                   if (StrStrW((PTSTR)comptId, TEXT("USB\\Class_02&SubClass_0e")) != NULL)
                   {
                      devType = QC_DEV_TYPE_MBIM;
                   }
                   // RNDIS
                   else if (StrStrW((PTSTR)comptId, TEXT("USB\\MS_COMP_RNDIS")) != NULL)
                   {
                      devType = QC_DEV_TYPE_RNDIS;
                   }
                   else if (StrStrW((PTSTR)comptId, TEXT("USB\\Class_EF&SubClass_04&Prot_01")) != NULL)
                   {
                      devType = QC_DEV_TYPE_RNDIS;
                   }
                }

                // filter by USB VID
                if (bMatch == TRUE)
                {
                   if ((FeatureSetting.User.Settings & DEV_FEATURE_SCAN_USB_WITH_VID) != 0)
                   {
                      if (StrStrW((PTSTR)hwId, (PTSTR)TEXT(BUS_TEST_USB)) != NULL)
                      {
                         if (StrStrW((PTSTR)hwId, (PTSTR)FeatureSetting.VID) != NULL)
                         {
                            // match VID
                            QCD_Printf(Debug, "USB VID matched <%ws>\n", (PTSTR)hwId);
                         }
                         else
                         {
                            bMatch = FALSE;
                         }
                      }
                      else
                      {
                         // For QIL we just need EDL devices which is always USB, others are not required
                         bMatch = FALSE;
                      }
                   }
                   // QCD_Printf("Matched class <%ws>... devType %d\n", (PTSTR)devClass, devType);
                }

                if (bMatch == TRUE)
                {
                   if (StrStrW((PTSTR)devClass, TEXT(D_CLASS_LIBUSB)) != NULL)
                   {
                      if (StrStrW((PTSTR)hwId, TEXT("VID_05C6")) == NULL)
                      {
                         QCD_Printf(Info, "Skip non Qualcomm libusb device <%ws>\n", (PTSTR)hwId);
                         bMatch = FALSE;
                      }
                   }
                }
             }

             if (bMatch == FALSE)
             {
                memberIdx++;
                continue;
             }
             else
             {
                cmRet = CM_Get_DevNode_Status(&devStatus, &problemNum, devInfoData.DevInst, 0);
                if (cmRet == CR_SUCCESS)
                {
                   // if (StrStrW((PTSTR)hwId, TEXT("VID_05C6")) != NULL)
                   // {
                   //    QCD_Printf("\tCM_Get_DevNode_Status 0x%x HWID <%ws>\n", devStatus, (PTSTR)hwId);
                   // }
                   if ((devStatus & DN_STARTED) == 0)
                   {
                      QCD_Printf(Debug, "\tDevice NOT started.\n");
                      memberIdx++;
                      continue;
                   }
                   else
                   {
                      // QCD_Printf("\tDevice started.\n");

                      // if (StrStrW((PTSTR)hwId, TEXT("VID_05C6")) != NULL)  // limit to QC device for debugging
                      if (StrStrW((PTSTR)hwId, TEXT("USB")) != NULL)  // only re-test USB device
                      {
                         // test again after 100ms
                         Sleep(100);
                         devStatus = 0;
                         cmRet = CM_Get_DevNode_Status(&devStatus, &problemNum, devInfoData.DevInst, 0);
                         QCD_Printf(Debug, "\t2ND: CM_Get_DevNode_Status 0x%x HWID <%ws>\n", devStatus, (PTSTR)hwId);
                         if ((devStatus & DN_STARTED) == 0)
                         {
                            QCD_Printf(Debug, "\t2ND: Device NOT started.\n");
                         }
                      }
                   }
                }
                else
                {
                   QCD_Printf(Error, "\tCM_Get_DevNode_Status failed 0x%x HWID <%ws>\n", cmRet, (PTSTR)hwId);
                   memberIdx++;
                   continue;
                }
             }

             if (StrStrW((PTSTR)devClass, TEXT(D_CLASS_LIBUSB)) != NULL) {
                 if (bMatch == TRUE && devType == QC_DEV_TYPE_LIBUSB)
                 {
                     //libusb API's
                     EnterCriticalSection(&opLock);
                     _IsQcomLibusbEnable = TRUE;
                     /* ensure to clear deviceListHead*/
                     enumerate_devices();
                     LeaveCriticalSection(&opLock);
                 }
             }

             // retrieve driver/software key
             bResult = SetupDiGetDeviceRegistryProperty
                       (
                          devInfoHandle,
                          &devInfoData,
                          SPDRP_DRIVER,
                          NULL,
                          (LPBYTE)driverKey,
                          REG_HW_ID_SIZE,
                          &requiredSize
                       );
             if (bResult == FALSE)
                {
                // QCD_Printf("No driver info returned\n");
                memberIdx++;
                continue;
             }
             else
             {
                pSwIdx = StrStrW((PTSTR)driverKey, TEXT("\\"));
                if (pSwIdx != NULL)
                {
                   pSwIdx++;
                   // QCD_Printf("driver key <%ws> IDX <%ws>\n", (PWSTR)driverKey, pSwIdx);
                }
             }
             // retrieve device description for parsing
             DEVPROPTYPE propType = 0;
             DWORD propSize = sizeof(busRepDescW);
             ZeroMemory(busRepDescW, sizeof(busRepDescW));

             if (SetupDiGetDeviceProperty(
                 devInfoHandle,
                 &devInfoData,
                 &QDS_DEVPKEY_BusReportedDeviceDesc,
                 &propType,
                 (PBYTE)busRepDescW,
                 propSize,
                 &propSize,
                 0) == TRUE)
             {
                 WideCharToMultiByte(CP_ACP, 0, busRepDescW, -1,
                     busReportedDevDesc, QC_MAX_VALUE_NAME, NULL, NULL);
                     QCD_Printf(Debug, "BusReportedDeviceDesc: <%ws> for <%ws>\n",busRepDescW, (PTSTR)friendlyName);
             }

             // retrieve deivce name for display
             bResult = SetupDiGetDeviceRegistryProperty
                       (
                          devInfoHandle,
                          &devInfoData,
                          SPDRP_FRIENDLYNAME,
                          NULL,
                          (LPBYTE)friendlyName,
                          QC_MAX_VALUE_NAME,
                          &requiredSize
                       );
             if (bResult == FALSE)
             {
                SetupDiGetDeviceRegistryProperty
                (
                   devInfoHandle,
                   &devInfoData,
                   SPDRP_DEVICEDESC,
                   NULL,
                   (LPBYTE)friendlyName,
                   QC_MAX_VALUE_NAME,
                   &requiredSize
                );

                // QCD_Printf("<%ws> class <%ws> devType %d ADB %d\n", friendlyName, (PTSTR)devClass, devType, bAdbDetected);

                if (bAdbDetected == TRUE)
                {
                   CHAR  custName[QC_MAX_VALUE_NAME];
                   // TODO: for Android, set friendly name
                   StringCchCopy((PTSTR)custName, QC_MAX_VALUE_NAME/2, (PTSTR)friendlyName);
                   StringCchCat((PTSTR)custName, QC_MAX_VALUE_NAME/2, TEXT(" ("));
                   StringCchCat((PTSTR)custName, QC_MAX_VALUE_NAME/2, (PTSTR)pSwIdx);
                   StringCchCat((PTSTR)custName, QC_MAX_VALUE_NAME/2, TEXT(")"));

                   bResult = SetupDiSetDeviceRegistryProperty
                             (
                                devInfoHandle,
                                &devInfoData,
                                SPDRP_FRIENDLYNAME,
                                (LPBYTE)custName,
                                QC_MAX_VALUE_NAME
                             );
                   if (bResult == TRUE)
                   {
                      QCD_Printf(Debug, "ADB custName <%ws>\n", (PTSTR)custName);
                      StringCchCopy((PTSTR)friendlyName, QC_MAX_VALUE_NAME/2, (PTSTR)custName);
                   }
                }
             }

             if (StrStrW((PTSTR)devClass, TEXT(D_CLASS_CPU)) != NULL) 
             {
                 // Snapdragon on Windows support
                 if (StrStrIW((PTSTR)friendlyName, TEXT("Snapdragon")) != NULL ||
                     StrStrIW((PTSTR)friendlyName, TEXT("Qualcomm")) != NULL)
                 {
                     QCD_Printf(Info, "CPU Name: <%ws>\n", (PTSTR)friendlyName);
                     bProcessorAdded = TRUE;
                 }
                 else
                 {
                     memberIdx++;
                     continue;
                 }
             }

             #ifdef QC_LPC_SUPPORT
             // LPC_TEST -- add filters
             if ((StrStrW((PTSTR)friendlyName, TEXT("LPC Device")) == NULL) &&
                 (StrStrW((PTSTR)friendlyName, TEXT("HS-USB Diagnostics")) == NULL))
             {
                // QCD_Printf("Skipping <%ws>\n", friendlyName);
                memberIdx++;
                continue;
             }
             #endif


             if (SetupDiGetDevicePropertyW
                 (devInfoHandle,
                 &devInfoData,
                 &DEVPKEY_Device_LocationPaths,
                 &type,
                 (PBYTE)LocationPath,
                 sizeof(LocationPath),
                 &requiredSize,
                 0))
             {
               //   QCD_Printf(Info, "Location Path: %ws\n", LocationPath);
             }
             else
             {
                 // ERROR_NOT_FOUND (1168) is expected for devices that do not
                 // publish a LocationPaths property (non-USB nodes, or USB
                 // nodes mid-PnP-publish right after arrival / mode switch).
                 DWORD err = GetLastError();
                 if (err == ERROR_NOT_FOUND) {
                     QCD_Printf(Data,
                         "%d: DEVPKEY_Device_LocationPaths not present (err=%lu type=%u)\n",
                         __LINE__, err, (unsigned)type);
                 } else {
                     QCD_Printf(Warn,
                         "%d: DEVPKEY_Device_LocationPaths failed err=%lu type=%u\n",
                         __LINE__, err, (unsigned)type);
                 }
                 LocationPath[0] = L'\0';
             }

             unsigned Port = 0;
             vector<unsigned> portsarr;
             if (!LocationPath) {
                 QCD_Printf(Info, "LocationPath Empty");
             }
             else {
                 const WCHAR* p = LocationPath;

                 while ((p = wcsstr(p, L"USB(")) != nullptr) {
                     p += 4; // skip "USB("
                     const WCHAR* end = wcschr(p, L')');
                     if (!end) break;

                     //lastPort = (unsigned)wcstoul(std::wstring(p, end).c_str(), nullptr, 10);
                     Port = (unsigned)wcstoul(p, nullptr, 10);
                     portsarr.push_back(Port);
                     p = end + 1; // continue searching for next USB(n)
                 }
             }

             string portnos = "";
             for (size_t i = 0; i < portsarr.size(); i++) {
                 if (i) {
                     portnos += "-";
                 }
                 portnos += to_string(portsarr[i]);
             }

             // Builds the port-number chain on ACPI-enumerated USB controllers
             // for Snapdragon X Elite ARM64 based processor
             if ((portnos.empty() || wcsstr(LocationPath, L"ACPI(") != NULL) &&
                StrStrW((PTSTR)devClass, TEXT(D_CLASS_LIBUSB)) != NULL)
             {
                 portnos = GetUsbPortChainFromCmTree(devInfoData.DevInst);
                 if (!portnos.empty())
                 {
                     QCD_Printf(Info, "ACPI USB path: CM-tree port chain: %s\n",
                         portnos.c_str());
                 }
                 else
                 {
                     QCD_Printf(Warn, "ACPI USB path: CM-tree walk yielded no port numbers; "
                         "portnos will remain empty\n");
                 }
             }

             QCD_Printf(Info, "Location Path: %ws, portnos: %s\n", LocationPath, portnos.c_str());
             StringCchCopyW(driverKeyPath, REG_HW_ID_SIZE, TEXT(QC_REG_SW_KEY));
             StringCchCatW(driverKeyPath, REG_HW_ID_SIZE, (PTSTR)driverKey);
             WideCharToMultiByte(CP_ACP, 0, driverKeyPath, -1, driverKeyPathNew, REG_HW_ID_SIZE, NULL, NULL);

             if ((StrStrW((PTSTR)friendlyName, TEXT("Qualcomm")) != NULL) ||
                 (StrStrW((PTSTR)friendlyName, TEXT("QDSS")) != NULL) ||
                 StrStrIW((PTSTR)friendlyName, TEXT("Snapdragon")) != NULL)
             {
                QCD_Printf(Debug, "friendlyName: <%ws> class <%ws> devType %d ADB %d\n", friendlyName, (PTSTR)devClass, devType, bAdbDetected);
                if (cmRet == CR_SUCCESS)
                {
                   if ((devStatus & DN_STARTED) == 0)
                   {
                      QCD_Printf(Debug, "\tDevice NOT started.\n");
                   }
                   else
                   {
                     //  QCD_Printf(Debug, "\tDevice started with driverKeyPath: %s\n", driverKeyPathNew);
                      EnterCriticalSection(&opLock);
                      if (StrStrW((PTSTR)devClass, TEXT(D_CLASS_LIBUSB)) != NULL) {
                      	if (_IsQcomLibusbEnable == TRUE) { // QcDevice namespace variable
                            CHAR friendlyNameANSI[QC_MAX_VALUE_NAME];
                            WideCharToMultiByte(CP_ACP, 0, (PTSTR)friendlyName, -1, friendlyNameANSI, QC_MAX_VALUE_NAME, NULL, NULL);
                            set_param_in_registry(driverKeyPathNew, (const char *)friendlyNameANSI, portnos);
			            }
                      }
                      LeaveCriticalSection(&opLock);
                   }
                }
                else
                {
                   QCD_Printf(Error, "\tQDSSDPL: CM_Get_DevNode_Status failed 0x%x\n", cmRet);
                }
             }

             bResult = SetupDiGetDeviceRegistryProperty
                       (
                          devInfoHandle,
                          &devInfoData,
                          SPDRP_LOCATION_INFORMATION,
                          NULL,
                          (LPBYTE)location,
                          QC_MAX_VALUE_NAME,
                          &requiredSize
                       );

             if (bResult == FALSE)
             {
                if (StrStrW((PTSTR)hwId, (PTSTR)TEXT(BUS_TEST_USB)) != NULL)
                {
                   QCD_Printf(Error, "\tLOCATION_INFORMATION failure: 0x%x\n", GetLastError());
                }
             }

             bResult = SetupDiGetDeviceRegistryProperty
                       (
                          devInfoHandle,
                          &devInfoData,
                          SPDRP_LOCATION_PATHS,
                          NULL,
                          (LPBYTE)devPath,
                          QC_MAX_VALUE_NAME,
                          &requiredSize
                       );
             if (bResult == FALSE)
             {
                if (StrStrW((PTSTR)hwId, (PTSTR)TEXT(BUS_TEST_USB)) != NULL)
                {
                   QCD_Printf(Error, "\tLOCATION_PATHS failure: 0x%x\n", GetLastError());
                }
             }
             QCD_Printf(Debug, "SPDRP Location path: %ws", devPath);
             // if location is not available, use devPath
             if ((location[0] == 0) && (location[1] == 0))
             {
                CopyMemory((PVOID)location, (PVOID)devPath, QC_MAX_VALUE_NAME);
             }

             // Get a dev item
             EnterCriticalSection(&opLock);
             if (!IsListEmpty(&FreeList))
             {
                PLIST_ENTRY pEntry;

                pEntry = RemoveHeadList(&FreeList);
                pItem = CONTAINING_RECORD(pEntry, QC_DEV_ITEM, List);
                LeaveCriticalSection(&opLock);
                pItem->Info.Type = devType;
             }
             else
             {
                QCD_Printf(Fatal, "CRITICAL: no pItem for dev\n");
                LeaveCriticalSection(&opLock);
                pItem = NULL;
             }

             requiredSize = 0;
             bInstanceIdOK = SetupDiGetDeviceInstanceId
                             (
                                devInfoHandle,
                                &devInfoData,
                                (PWSTR)instanceId,
                                256,
                                &requiredSize
                             );

             if (bInstanceIdOK == TRUE)
             {
                if (StrStrW((PTSTR)instanceId, TEXT(BUS_TEST_USB)) != NULL)
                {
                   busType = QC_DEV_BUS_TYPE_USB;
                }
                else if (StrStrW((PTSTR)instanceId, TEXT(BUS_TEST_PCI)) != NULL)
                {
                   busType = QC_DEV_BUS_TYPE_PCI;
                }
                else if (StrStrW((PTSTR)instanceId, TEXT(BUS_TEST_PCIE)) != NULL)
                {
                   busType = QC_DEV_BUS_TYPE_PCIE;
                }
                 QCD_Printf(Debug, "ScanDevices: <%ws> BusType (%d) <%ws>\n", (PWSTR)instanceId, busType, (PTSTR)friendlyName);
             }

             if ((bInstanceIdOK == TRUE) && ((devType == QC_DEV_TYPE_PORTS) || (devType == QC_DEV_TYPE_MDM)))
             {
           
                pDevName = ValidateDevice
                           (
                              (PTSTR)driverKey,
                              (PVOID)instanceId,
                              (PVOID)ifName,
                              (PVOID)serNum,
                              (PVOID)socVer,
                              (PVOID)devDetails,
                              (PVOID)serNumMsm,
                              (PVOID)devCID,
                              (PVOID)parentDev,
                              &funcProtocol,
                              &mtu,
                              devType,
                              &bActive,
                              &bQCDriver,
                              devStatus
                           );
             }
             else
             {
                pDevName = ValidateDevice
                           (
                              (PTSTR)driverKey,
                              (PVOID)instanceId, // NULL,
                              (PVOID)ifName,
                              (PVOID)serNum,
                              (PVOID)socVer,
                              (PVOID)devDetails,
                              (PVOID)serNumMsm,
                              (PVOID)devCID,
                              (PVOID)parentDev,
                              &funcProtocol,
                              &mtu,
                              devType,
                              &bActive,
                              &bQCDriver,
                              devStatus
                           );
             }
             
             if (bActive == TRUE)
             {
                if (pItem != NULL)
                {
                   size_t rtnBytes;

                   if (busType == QC_DEV_BUS_TYPE_USB)
                   {
                       StringCchCopy((PTSTR)devDetails, 128, busRepDescW);
                       ParseDevDesc((PWSTR)devDetails, devDetailsMP);
                       if (pItem->DevDetails)
                       {
                           *(pItem->DevDetails) = devDetailsMP;
                       }
                       auto it = devDetailsMP.find("SN");
                       if (it != devDetailsMP.end())
                       {
                           MultiByteToWideChar(CP_ACP, 0, it->second.c_str(), -1, (LPWSTR)serNumMsm, 128);
                       }
                       //Until the driver registty code for dev description is not released, keep this segment commented
                       //else
                       //{
                       //    serNumMsm[0] = '\0';  // or handle error
                       //}

                       it = devDetailsMP.find("CID");
                       if (it != devDetailsMP.end())
                       {
                           MultiByteToWideChar(CP_ACP, 0, it->second.c_str(), -1, (LPWSTR)devCID, 128);
                       }
                       //Until the driver registty code for dev description is not released, keep this segment commented
                       //else
                       //{
                       //   devCID[0] = '\0';  // or handle error
                       //}
                   }
                   pItem->Info.Type = devType;
                   pItem->Info.Flag = DEV_FLAG_NONE;
                   pItem->Info.IsQCDriver = (UCHAR)bQCDriver;

                   if ((StrStrW((PTSTR)hwId, TEXT("VID_1679")) != NULL)  &&
                       (StrStrW((PTSTR)hwId, TEXT("PID_5002")) != NULL))
                   {
                       funcProtocol = DEV_PROTOCOL_PROMIRA;
                   }

                   GetDeviceInterfaceNumberFromHwId((PTSTR)hwId, &pItem->InterfaceNumber);
                   pItem->CbParams.Protocol = funcProtocol;
                   pItem->CbParams.Mtu = (ULONG)mtu;
                   pItem->BusType = busType;
                   StringCchCopy((PTSTR)pItem->DevDesc, QC_MAX_VALUE_NAME/2, (PTSTR)friendlyName);
                   if (StrStrW((PTSTR)devClass, TEXT(D_CLASS_LIBUSB)) != NULL) {
                       if (_IsQcomLibusbEnable == TRUE) {
                           if (!portnos.empty()) {
                               WCHAR portNo[128];
                               swprintf_s(portNo, 128, L" (%S)", portnos.c_str());
                               StringCchCat((PTSTR)pItem->DevDesc, QC_MAX_VALUE_NAME / 2, portNo);
                           }
                       }
                   }
                   wcstombs_s(&rtnBytes, pItem->DevDescA, QC_MAX_VALUE_NAME, (PWCHAR)pItem->DevDesc, _TRUNCATE);

                   StringCchCopy((PTSTR)pItem->DevNameW, QC_MAX_VALUE_NAME/2, TEXT(QC_DEV_PREFIX));
                   if (pDevName != NULL)
                   {
                      StringCchCat((PTSTR)pItem->DevNameW, QC_MAX_VALUE_NAME/2, (PTSTR)pDevName);
                   }
                   else
                   {
                      // QDSS/DPL - use friendly name as device name
                      StringCchCat((PTSTR)pItem->DevNameW, QC_MAX_VALUE_NAME/2, (PTSTR)friendlyName);
                   }
                   wcstombs_s(&rtnBytes, pItem->DevNameA, QC_MAX_VALUE_NAME, (PWCHAR)pItem->DevNameW, _TRUNCATE);
                   if (bHwIdOK == TRUE)
                   {
                      wcsncat_s((PWCHAR)hwId, 256, L"&CID_", 256);
                      wcsncat_s((PWCHAR)hwId, 256, (PWCHAR)devCID, 256);
                      StringCchCopy((PTSTR)pItem->HwId, QC_MAX_VALUE_NAME/2, (PTSTR)hwId);
                   }
                   if (devType == QC_DEV_TYPE_NET)
                   {
                      StringCchCopy((PTSTR)pItem->InterfaceName, QC_MAX_VALUE_NAME/2, (PTSTR)ifName);
                   }
                   StringCchCopy((PTSTR)pItem->Location, QC_MAX_VALUE_NAME/2, (PTSTR)location);
                   StringCchCopy((PTSTR)pItem->DevPath, QC_MAX_VALUE_NAME/2, (PTSTR)devPath);
                   StringCchCopy((PTSTR)pItem->SerNum, 128, (PTSTR)serNum);
                   StringCchCopy((PTSTR)pItem->SocVer, 128, (PTSTR)socVer);
                   StringCchCopy((PTSTR)pItem->SerNumMsm, 128, (PTSTR)serNumMsm);
                   StringCchCopyA(pItem->PortNos, 128, portnos.c_str());
                   QCD_Printf(Debug, "PortNos <%s>\n", pItem->PortNos);
                   // Since we need the parent location so find parent is necessary for every enumeration, use the DevDesc
                   // if ((parentDev[0] == 0) && (parentDev[1] == 0))
                   {
                      QCD_Printf(Debug, " To find parent for <%ws> <%ws>\n", (PTSTR)pItem->DevNameW, (PTSTR)pItem->DevDesc);
                      if (FALSE == FindParent((PVOID)instanceId, (PVOID)parentDev, (PVOID)parentLocationInformation, (PVOID)potentialSerNum))
                      {
                         StringCchCopy((PTSTR)pItem->ParentDev, 128, (PTSTR)friendlyName);
                         StringCchCopy((PTSTR)pItem->ParentLocationInfomation, QC_MAX_VALUE_NAME / 2, (PTSTR)location);
                      }
                      else
                      {
                         StringCchCopy((PTSTR)pItem->ParentDev, 128, (PTSTR)parentDev);
                         StringCchCopy((PTSTR)pItem->ParentLocationInfomation, QC_MAX_VALUE_NAME / 2, (PTSTR)parentLocationInformation);
                         if ((serNum[0] == 0) && (serNum[1] == 0))
                         {
                            // StringCchCopy((PTSTR)pItem->SerNum, 128, (PTSTR)potentialSerNum);
                            QCD_Printf(Debug, "Ignore potential SerNum from parent for <%ws>\n", (PTSTR)pItem->DevDesc);
                            
                         }
                      }
                   }
                   InsertTailList(&NewArrivalList, &pItem->List);
                   QCD_Printf(Debug, "Dev Added: <%ws> HWID <%ws> INST <%ws> 0x%p\n", (PTSTR)pItem->DevDesc, (PTSTR)hwId, (PWSTR)instanceId, pItem);
                }
             }  // if
             else
             {
                //QCD_Printf(Debug, "Dev failed to be added: <%ws>\n", (PTSTR)pItem->DevDesc);
                EnterCriticalSection(&opLock);
                InsertHeadList(&FreeList, &pItem->List);
                LeaveCriticalSection(&opLock);
             }

             memberIdx++;
          }  // while

          if (devInfoHandle != INVALID_HANDLE_VALUE)
          {
             SetupDiDestroyDeviceInfoList(devInfoHandle);
          }

          return;

      }  // ScanDevices

      DWORD WINAPI RunDevMonitor(PVOID Context)
      {
         DWORD status = WAIT_OBJECT_0;
         static BOOL bScannedAlready = FALSE;
         DWORD  tid;

         while (bMonitorRunning == TRUE)  // set in StartDeviceMonitor()
         {
            if (bScannedAlready == FALSE)
            {
               bScannedAlready = TRUE;
               hAnnouncementThread = ::CreateThread(NULL, 0, AnnouncementThread, NULL, 0, &tid);
               SetEvent(hMonitorStartedEvt);
            }
            // if (status == WAIT_OBJECT_0)
            if (status < QC_REG_MAX)
            {
                //QCD_Printf(Info, "scanning with alert %u...\n", status);
               ScanDevices();
               TryToAnnounce(&ArrivalList, &NewArrivalList);
            }
            status = MonitorDeviceChange();
            //GetDriverVersion();
         }
         SetEvent(hAnnouncementEvt);  // last announcement to clean up queues
         WaitForSingleObject(hAnnouncementExitEvt, 1000);
         CleanupList(&ArrivalList);
         CleanupList(&NewArrivalList);
         CleanupList(&FreeList);

         if (NotifyStore != NULL)
         {
            free(NotifyStore);
         }
         QCD_Printf(Info, "exiting device monitor...\n");
         SetEvent(hStopMonitorEvt);  // signal after cleaning up
         bScannedAlready = FALSE;

         return 0;
      }  // RunDevMonitor

   }  // anonymous namespace

   VOID DisplayDevices(VOID)
   {
      PQC_DEV_ITEM pDevInfo;
      PLIST_ENTRY headOfArrival, peekArrival;

      QCD_Printf(Debug, "================ DEVICES =================\n");

      EnterCriticalSection(&opLock);

      if (!IsListEmpty(&ArrivalList))
      {
         headOfArrival = &ArrivalList;
         peekArrival = headOfArrival->Flink;
         while (peekArrival != headOfArrival)
         {
            pDevInfo = CONTAINING_RECORD(peekArrival, QC_DEV_ITEM, List);
            peekArrival = peekArrival->Flink;
            QCD_Printf(Debug, "[Type_%d] <%ws>\n", pDevInfo->Info.Type, pDevInfo->DevDesc);
            QCD_Printf(Debug, "   Dev-to-open <%ws>\n", pDevInfo->DevNameW);
         }
      }

      LeaveCriticalSection(&opLock);
   }  // DisplayDevices

   // ConfigureCommChannel: Initial implementation for quick validation
   // Configure communication channel, error codes to be defined later
   int ConfigureCommChannel(UCHAR DevType, HANDLE DeviceHandle, DWORD Baudrate, BOOL IsLegacyTimeoutConfig)
   {
      int retVal = 0;

      if (DevType == QC_DEV_TYPE_PORTS)
      {
         DCB dcb;
         COMMTIMEOUTS commTimeouts;

         dcb.DCBlength = sizeof(DCB);
         if (GetCommState(DeviceHandle, &dcb) == TRUE)
         {            
            dcb.BaudRate = Baudrate;
            dcb.ByteSize    = 8;
            dcb.Parity      = NOPARITY;
            dcb.StopBits    = ONESTOPBIT;
            dcb.fDtrControl = DTR_CONTROL_ENABLE;
            dcb.fRtsControl = RTS_CONTROL_ENABLE;
            if (SetCommState (DeviceHandle, &dcb) == FALSE)
            {
               QCD_Printf(Error, "[Type_%d] SetCommState failed on handle 0x%x, error 0x%x\n",
                           DevType, DeviceHandle, GetLastError());
               retVal = 1;
            }
            else
            {
				   QCD_Printf(Debug, "[Type_%d] SetCommState done", DevType);
            }
         }
         else
         {
            QCD_Printf(Error, "[Type_%d] GetCommState failed on handle 0x%x, error 0x%x\n",
                        DevType, DeviceHandle, GetLastError());
            retVal = 2;
         }
         
         if (IsLegacyTimeoutConfig)
         {
            commTimeouts.ReadIntervalTimeout = 20;
            commTimeouts.ReadTotalTimeoutMultiplier = 0;
            commTimeouts.ReadTotalTimeoutConstant = 100;
            commTimeouts.WriteTotalTimeoutMultiplier = 1;
            commTimeouts.WriteTotalTimeoutConstant = 10;
         }
         else
         {
            commTimeouts.ReadIntervalTimeout = MAXDWORD;
            commTimeouts.ReadTotalTimeoutMultiplier = MAXDWORD;
            commTimeouts.ReadTotalTimeoutConstant = 100;
            commTimeouts.WriteTotalTimeoutMultiplier = 0;
            commTimeouts.WriteTotalTimeoutConstant = 2000;
         }

         if (SetCommTimeouts(DeviceHandle, &commTimeouts) == false)
         {
            QCD_Printf(Error, "[Type_%d] SetCommTimeouts failed on handle 0x%x Error 0x%x\n",
                        DevType, DeviceHandle, GetLastError());
            retVal = 3;
         }
         else
         {
            QCD_Printf(Debug, "[Type_%d] SetCommTimeouts done", DevType);
         }
      }
      return retVal;
   }  // ConfigureCommChannel
}  // QcDevice

// ----------- PUBLIC APIs -------------

QCDEVLIB_API VOID QcDevice::StartDeviceMonitor(VOID)
{
   DWORD  tid;

   QCD_Printf(Info, "StartDeviceMonitor::QDS version <%s>\n", LibVersion());

   if (InterlockedIncrement(&lInOperation) > 1)
   {
      InterlockedDecrement(&lInOperation);
      return;
   }

   if (bMonitorRunning == FALSE)
   {
      if (bFeatureSet == FALSE)
      {
         FeatureSetting.User.Version       = 1;
         FeatureSetting.User.Settings      = DEV_FEATURE_INCLUDE_NONE_QC_PORTS;
         FeatureSetting.User.DeviceClass   = (DEV_CLASS_NET | DEV_CLASS_PORTS | DEV_CLASS_LIBUSB | DEV_CLASS_USB);
         FeatureSetting.TimerInterval      = 1000; // INFINITE;  // ADB device needs timer
         ZeroMemory(FeatureSetting.VID, QC_MAX_VALUE_NAME);
      }
      InitializeCriticalSection(&opLock);
      InitializeCriticalSection(&notifyLock);
      hStopMonitorEvt = CreateEvent(NULL, FALSE, FALSE, NULL);
      hMonitorStartedEvt = CreateEvent(NULL, FALSE, FALSE, NULL);
      hAnnouncementEvt = CreateEvent(NULL, FALSE, FALSE, NULL);
      hAnnouncementExitEvt = CreateEvent(NULL, FALSE, FALSE, NULL);
      InitializeLists();
      bMonitorRunning = TRUE;
      //libusb API's
      if (!load_libusb()) {
          QCD_Printf(Info, "Failed to load libusb library.\n");
          QCD_Printf(Info, "libusb DLL not found at QUD-Userspace DriverPackage path, skipping libusb initialization\n");
      }
      else 
      {
         initialize_libusb();
      }
      hMonitorThread = ::CreateThread(NULL, 0, RunDevMonitor, NULL, 0, &tid);
      if (hMonitorThread == NULL)
      {
         bMonitorRunning = FALSE;
      }
      WaitForSingleObject(hMonitorStartedEvt, 5000);
      CloseHandle(hMonitorStartedEvt);  // dispose
   }

   InterlockedDecrement(&lInOperation);

   return;
}  // StartDeviceMonitor

QCDEVLIB_API VOID QcDevice::StopDeviceMonitor(VOID)
{
   int i;

   bMonitorRunning = FALSE;
   SetEvent(hRegChangeEvt[0]);
   WaitForSingleObject(hStopMonitorEvt, 10000);
   if (hAnnouncementThread != 0)
   {
      CloseHandle(hAnnouncementThread);
      hAnnouncementThread = 0;
   }
   if (hMonitorThread != 0)
   {
      CloseHandle(hMonitorThread);
      hMonitorThread = 0;
   }
   CloseHandle(hAnnouncementEvt);
   CloseHandle(hAnnouncementExitEvt);
   CloseHandle(hStopMonitorEvt);

   if (is_libusb_loaded())
   {
         deinitialize_libusb();
   }
   else
   {
      QCD_Printf(Info, "libusb DLL not found at QUD-Userspace DriverPackage path, skipping libusb deinitialization\n");
   }
   _IsQcomLibusbEnable = FALSE;

   for (i = QC_REG_DEVMAP; i < QC_REG_MAX; i++)
   {
      CloseHandle(hRegChangeEvt[i]);
      CloseHandle(hSysKey[i]);
   }
   DeleteCriticalSection(&opLock);
   DeleteCriticalSection(&notifyLock);
   lInitialized = 0;
   QCD_Printf(Info, "%s", __func__);
}  // StopDeviceMonitor

// =================== Removal Notification ====================
QCDEVLIB_API VOID QcDevice::SetDeviceChangeCallback(DEVICECHANGE_CALLBACK Cb)
{
   fNotifyCb = Cb;
   CallerContext = NULL;
}  // SetDeviceChangeCallback

QCDEVLIB_API VOID QcDevice::SetDeviceChangeCallback(DEVICECHANGE_CALLBACK Cb, PVOID AppContext)
{
   fNotifyCb = Cb;
   CallerContext = AppContext;
}  // SetDeviceChangeCallback

QCDEVLIB_API VOID QcDevice::SetDeviceChangeCallback(DEVICECHANGE_CALLBACK_N Cb)
{
   fNotifyCb_N = Cb;
}  // SetDeviceChangeCallback

QCDEVLIB_API VOID QcDevice::SetLoggingCallback(QCD_LOGGING_CALLBACK Cb)
{
   setLoggerCallback(Cb);
}  // SetLoggingCallback

QCDEVLIB_API VOID QcDevice::SetFeature(PVOID Settings)
{
   PDEV_FEATURE_SETTING pFeature = (PDEV_FEATURE_SETTING)Settings;

   if (pFeature->Version != 1)
   {
      return;
   }
   FeatureSetting.User.Version       = pFeature->Version;
   FeatureSetting.User.Settings      = pFeature->Settings;
   FeatureSetting.User.DeviceClass   = pFeature->DeviceClass;
   ZeroMemory(FeatureSetting.VID, QC_MAX_VALUE_NAME);

   FeatureSetting.TimerInterval = 1000; // INFINITE; // 500; // ADB device needs timer

   bFeatureSet = TRUE;

   if ((FeatureSetting.User.Settings & DEV_FEATURE_SCAN_USB_WITH_VID) != 0)
   {
      if (pFeature->VID != NULL)
      {
         StringCchCopy((PTSTR)FeatureSetting.VID, QC_MAX_VALUE_NAME/2, (PTSTR)pFeature->VID);
      }
   }

} // SetFeature

QCDEVLIB_API ULONG QcDevice::GetDevice(PVOID UserBuffer)
{
   PDEV_PARAMS_N DevInfo = (PDEV_PARAMS_N)UserBuffer;
   PQC_NOTIFICATION_STORE storeBranch;
   PQC_DEV_ITEM devItem;
   PLIST_ENTRY headOfList;
   ULONG infoFilled = 0, returnVal = DEV_INFO_OK;

   QCD_Printf(Debug, "-->GetDevice (%d, %d, %d, %d, %d)\n",
               DevInfo->DevDescBufLen, DevInfo->DevnameBufLen, DevInfo->IfNameBufLen,
               DevInfo->LocBufLen, DevInfo->SerNumBufLen);

   EnterCriticalSection(&notifyLock);
   if (!IsListEmpty(&AnnouncementList))
   {
      headOfList = RemoveHeadList(&AnnouncementList);
      storeBranch = CONTAINING_RECORD(headOfList, QC_NOTIFICATION_STORE, List);
      LeaveCriticalSection(&notifyLock);

      while (!IsListEmpty(&(storeBranch->DevItemChain)))
      {
         headOfList = RemoveHeadList(&(storeBranch->DevItemChain));
         devItem = CONTAINING_RECORD(headOfList, QC_DEV_ITEM, List);

         if (DevInfo->DevDescBufLen > 0)
         {
            ZeroMemory(DevInfo->DevDesc, DevInfo->DevDescBufLen);
            StringCchCopy((PTSTR)DevInfo->DevDesc, DevInfo->DevDescBufLen/2, (PTSTR)devItem->DevDesc);
         }
         else
         {
            returnVal = DEV_INFO_DEV_DESC_LEN;
            break;
         }
         if (DevInfo->DevnameBufLen > 0)
         {
            ZeroMemory(DevInfo->DevName, DevInfo->DevnameBufLen);
            StringCchCopy((PTSTR)DevInfo->DevName, DevInfo->DevnameBufLen, (PTSTR)devItem->DevNameA);
         }
         else
         {
            returnVal = DEV_INFO_DEV_NAME_LEN;
            break;
         }
         if (DevInfo->IfNameBufLen > 0)
         {
            ZeroMemory(DevInfo->IfName, DevInfo->IfNameBufLen);
            StringCchCopy((PTSTR)DevInfo->IfName, DevInfo->IfNameBufLen/2, (PTSTR)devItem->InterfaceName);
         }
         else
         {
            returnVal = DEV_INFO_IF_NAME_LEN;
            break;
         }
         if (DevInfo->LocBufLen > 0)
         {
            ZeroMemory(DevInfo->Loc, DevInfo->LocBufLen);
            StringCchCopy((PTSTR)DevInfo->Loc, DevInfo->LocBufLen/2, (PTSTR)devItem->Location);
         }
         if (DevInfo->DevPathBufLen > 0)
         {
            ZeroMemory(DevInfo->DevPath, DevInfo->DevPathBufLen);
            StringCchCopy((PTSTR)DevInfo->DevPath, DevInfo->DevPathBufLen/2, (PTSTR)devItem->DevPath);
         }
         else
         {
            returnVal = DEV_INFO_LOC_LEN;
            break;
         }
         if (DevInfo->SerNumBufLen > 0)
         {
            ZeroMemory(DevInfo->SerNum, DevInfo->SerNumBufLen);
            StringCchCopy((PTSTR)DevInfo->SerNum, DevInfo->SerNumBufLen/2, (PTSTR)devItem->SerNum);
         }
         if (DevInfo->SerNumMsmBufLen > 0)
         {
            ZeroMemory(DevInfo->SerNumMsm, DevInfo->SerNumMsmBufLen);
            StringCchCopy((PTSTR)DevInfo->SerNumMsm, DevInfo->SerNumMsmBufLen/2, (PTSTR)devItem->SerNumMsm);
         }
         else
         {
            returnVal = DEV_INFO_SER_NUM_LEN;
            break;
         }
         DevInfo->Mtu = 0;  // not used

         if (devItem->Info.Flag == DEV_FLAG_ARRIVAL)
         {
            DevInfo->Flag = (((ULONG)devItem->Info.Type << 8) | ((ULONG)1 << 4) |
                             (ULONG)devItem->Info.IsQCDriver);
         }
         else
         {
            DevInfo->Flag = (((ULONG)devItem->Info.Type << 8) | (ULONG)devItem->Info.IsQCDriver);
         }

         free(devItem);
         infoFilled = 1;
         break;
      }
      if ((infoFilled == 0) && (returnVal != DEV_INFO_OK))
      {
         QCD_Printf(Error, "GetDevice: error %d, restore the device info for future retrieval\n", returnVal);
         InsertHeadList(&(storeBranch->DevItemChain), &(devItem->List)); // re-store
      }

      EnterCriticalSection(&notifyLock);
      if (IsListEmpty(&(storeBranch->DevItemChain)))
      {
         QCD_Printf(Debug, "GetDevice: recycle storeBranch\n");
         InsertTailList(&NotifyFreeList, &(storeBranch->List)); // recycle
      }
      else
      {
         QCD_Printf(Debug, "GetDevice: restore storeBranch\n");
         InsertHeadList(&AnnouncementList, &(storeBranch->List)); // restore
      }
   }  // if
   LeaveCriticalSection(&notifyLock);

   if ((returnVal == DEV_INFO_OK) && (infoFilled == 0))
   {
      QCD_Printf(Debug, "GetDevice: no more device\n");
      returnVal = DEV_INFO_END;
   }

   QCD_Printf(Debug, "<--GetDevice (ST %d, filled %d Flag 0x%x)\n", returnVal, infoFilled, DevInfo->Flag);
   return returnVal;   
}  // GetDevice

QCDEVLIB_API PCHAR QcDevice::GetPortName(PVOID DeviceName)
{
   size_t length;
   PCHAR p;

   p = (PCHAR)DeviceName;
   length = strlen(p);
   p += length;
   while (p > DeviceName)
   {
      if (*p == 0x5C)  // '\'
      {
         break;
      }
      p--;
   }

   if (p == DeviceName)
   {
      return NULL;
   }

   return (p+1);
}  // GetPortName

QCDEVLIB_API VOID QcDevice::GetDriverVersion(VOID)
{
    typedef std::pair <DWORDLONG, const WCHAR*> drvVerAndName;
    std::set <drvVerAndName> drvList;

    // Get the "device info set" for our driver GUID
    HDEVINFO devInfoSet = SetupDiGetClassDevsW(NULL, NULL, NULL, DIGCF_ALLCLASSES | DIGCF_PRESENT);

    // Cycle through all devices currently present
    for (int dev = 0; ; dev++)
    {
        // Get the device info for this device
        SP_DEVINFO_DATA devInfo;
        devInfo.cbSize = sizeof(SP_DEVINFO_DATA);
        if (!SetupDiEnumDeviceInfo(devInfoSet, dev, &devInfo))
            break;

        // Build a list of driver info items that we will retrieve below
        if (!SetupDiBuildDriverInfoList(devInfoSet,
            &devInfo, SPDIT_COMPATDRIVER))
            return; // Exit on error

        // Get all the info items for this driver 
        // (I don't understand why there is more than one)
        for (int devList = 0; ; devList++)
        {
            SP_DRVINFO_DATA drvInfo;
            drvInfo.cbSize = sizeof(SP_DRVINFO_DATA);
            if (!SetupDiEnumDriverInfo(devInfoSet, &devInfo,
                SPDIT_COMPATDRIVER, devList, &drvInfo))
                break;

            int ret = wcscmp(drvInfo.ProviderName, L"Qualcomm Incorporated") &
                wcscmp(drvInfo.ProviderName, L"Qualcomm") &
                wcscmp(drvInfo.ProviderName, L"Qualcomm Inc.") &
                wcscmp(drvInfo.ProviderName, L"QUALCOMM");
            if (ret == 0)
            {
                if (wcsstr(drvInfo.Description, L"QDSS") != 0)
                {
                    drvVerAndName val = std::make_pair(drvInfo.DriverVersion, L"QDSS");
                    drvList.insert(val);
                }
                else if ((wcsstr(drvInfo.Description, L"Diagnostics") || wcsstr(drvInfo.Description, L"QDLoader")) != 0)
                {
                    drvVerAndName val = std::make_pair(drvInfo.DriverVersion, L"serial");
                    drvList.insert(val);
                }
                else if (wcsstr(drvInfo.Description, L"Modem") != 0)
                {
                    drvVerAndName val = std::make_pair(drvInfo.DriverVersion, L"Modem");
                    drvList.insert(val);
                }
                else if (wcsstr(drvInfo.Description, L"WWAN") != 0)
                {
                    drvVerAndName val = std::make_pair(drvInfo.DriverVersion, L"Network");
                    drvList.insert(val);
                }
                else if (wcsstr(drvInfo.Description, L"ADB") != 0)
                {
                    drvVerAndName val = std::make_pair(drvInfo.DriverVersion, L"adb");
                    drvList.insert(val);
                }
                else if (wcsstr(drvInfo.Description, L"Composite") != 0)
                {
                    drvVerAndName val = std::make_pair(drvInfo.DriverVersion, L"Filter");
                    drvList.insert(val);
                }
            }
        }
    }
    for (auto const& x : drvList) {
        unsigned short part1 = ((x.first) >> 48) & 0xffff;
        unsigned short part2 = ((x.first) >> 32) & 0xffff;
        unsigned short part3 = ((x.first) >> 16) & 0xffff;
        unsigned short part4 = (x.first) & 0xffff;
        QCD_Printf(Info,"Driver: %ws --> Version: %d.%d.%d.%d\n", x.second, part1, part2, part3, part4);
    }
    SetupDiDestroyDeviceInfoList(devInfoSet);

}

QCDEVLIB_API ULONG QcDevice::GetDeviceList(PVOID Buffer, ULONG BufferSize, PULONG ActualSize)
{
   PQC_DEV_ITEM pDevInfo;
   PLIST_ENTRY headOfArrival, peekArrival;
   PCHAR pDest;
   size_t remainingSpace, nameLen;
   ULONG numOfDevices = 0;

   remainingSpace = BufferSize;
   pDest = (PCHAR)Buffer;
   *ActualSize = 0;

   EnterCriticalSection(&opLock);

   ZeroMemory(Buffer, BufferSize);
   if (!IsListEmpty(&ArrivalList))
   {
      headOfArrival = &ArrivalList;
      peekArrival = headOfArrival->Flink;
      while (peekArrival != headOfArrival)
      {
         pDevInfo = CONTAINING_RECORD(peekArrival, QC_DEV_ITEM, List);
         peekArrival = peekArrival->Flink;

         // copy DevDescA to the supplied buffer
         nameLen = strlen(pDevInfo->DevDescA);
         if (0 != strncpy_s(pDest, remainingSpace, pDevInfo->DevDescA, nameLen))
         {
            QCD_Printf(Error, "GetDeviceList error\n");
            break;
         }
         else
         {
            pDest += (nameLen + 1); // including the NULL
            remainingSpace -= (nameLen + 1);
            *ActualSize += (ULONG)(nameLen + 1);
            numOfDevices++;
         }
      }
   }

   LeaveCriticalSection(&opLock);

   return numOfDevices;

}  // GetDeviceList

QCDEVLIB_API HANDLE QcDevice::OpenDevice(PVOID DeviceName, DWORD Baudrate, BOOL IslegacyTimeoutConfig)
{
   PQC_DEV_ITEM pDevInfo;
   PLIST_ENTRY headOfArrival, peekArrival;
   PVOID pDevName = NULL;
   HANDLE hDevice = INVALID_HANDLE_VALUE;  // -1
   WCHAR usbDev[256];
   CHAR deviceNAME[256];
   CHAR SerialNAME[256];
   WCHAR serialMsmPortNosW[256];
   CHAR serialMsmPortNos[256];

   QCD_Printf(Info, "-->QcDevice::OpenDevice: <%ws>\n", (PWCHAR)DeviceName);
   
   // special handling for libusb devices
   EnterCriticalSection(&opLock);
   if (!IsListEmpty(&ArrivalList))
   {
       headOfArrival = &ArrivalList;
       peekArrival = headOfArrival->Flink;
       while (peekArrival != headOfArrival)
       {
           pDevInfo = CONTAINING_RECORD(peekArrival, QC_DEV_ITEM, List);
           peekArrival = peekArrival->Flink;
           QCD_Printf(Info, "%s: DeviceName: %ws, pDevInfo->DevDesc: %ws\n", __func__, (PWCHAR)DeviceName, (PWCHAR)pDevInfo->DevDesc);
           if (pDevInfo->Info.Type == QC_DEV_TYPE_LIBUSB)
           {
               if (StrIntlEqN((PWCHAR)DeviceName, (PWCHAR)pDevInfo->DevDesc, wcslen((PWCHAR)pDevInfo->DevDesc)))
               {
                   WideCharToMultiByte(CP_ACP, 0, (PWCHAR)pDevInfo->DevDesc, -1, deviceNAME, 256, NULL, NULL);
                   QCD_Printf(Info, "%s: OpenDevice I Port Path: %s, SerNumMsm: <%ws>\n", __func__, (char*)pDevInfo->PortNos, (PWCHAR)pDevInfo->SerNumMsm);
                   int res = QcomLibusbDevice::qcom_libusb_open(deviceNAME, (char *)pDevInfo->PortNos, &hDevice);
                   if (res != LIBUSB_SUCCESS)
                   {
                       res = QcomLibusbDevice::qcom_libusb_open((char*)pDevInfo->PortNos, pDevInfo->InterfaceNumber, &hDevice);
                   }
                   LeaveCriticalSection(&opLock);
                   return (res ? INVALID_HANDLE_VALUE : hDevice);
               }
           }
       }
   }
   LeaveCriticalSection(&opLock);

   if ((StrStrW((PWCHAR)DeviceName, L"QDSS") == NULL) && (StrStrW((PWCHAR)DeviceName, L"DPL") == NULL))
   {
      EnterCriticalSection(&opLock);
      if (!IsListEmpty(&ArrivalList))
      {
         headOfArrival = &ArrivalList;
         peekArrival = headOfArrival->Flink;
         while (peekArrival != headOfArrival)
         {
            pDevInfo = CONTAINING_RECORD(peekArrival, QC_DEV_ITEM, List);
            peekArrival = peekArrival->Flink;
            if (StrCmpW((PWCHAR)DeviceName, (PWCHAR)pDevInfo->DevDesc) == 0)
            {
               // Look for "EUD" in device name to determine whether EUD device
               if(nullptr != wcsstr((PWSTR)pDevInfo->DevNameW, L"EUD")) // e.g. "\\\\.\\Qualcomm EUD Control Device 9501 (0001)"
               { 
                  // create copy of device name and append \DEBUG to it to connect
                  WCHAR nameCopy[QC_MAX_VALUE_NAME];   
                  
                  StrCpyW(nameCopy, (PCWSTR)pDevInfo->DevNameW);                  
                  StrCatW((PWSTR)nameCopy, L"\\DEBUG");    // Need to append \DEBUG to device name to connect as per usb driver requirements   
                                                           // e.g. "\\\\.\\Qualcomm EUD Control Device 9501 (0001)\\DEBUG"                                  
                  
                  pDevName = (PVOID)nameCopy;
               }
               else
               {
                  pDevName = (PVOID)pDevInfo->DevNameW;
               }
               break;
            }            
         }
      }
      LeaveCriticalSection(&opLock);
   }
   else // QDSS or DPL case
   {
           wcsncpy_s(usbDev, 256, L"\\\\.\\", 256);
           wcsncat_s(usbDev, 256, (PWCHAR)DeviceName, 256);
           pDevName = usbDev;
   }
   
   if (pDevName != NULL)
   {
      hDevice = ::CreateFileW
                (
                   (PWCHAR)pDevName,
                   GENERIC_WRITE | GENERIC_READ,
                   FILE_SHARE_READ | FILE_SHARE_WRITE,
                   NULL,
                   OPEN_EXISTING,
                   FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OVERLAPPED, // async operation
                   NULL
                );

      if (hDevice == INVALID_HANDLE_VALUE)
      {
         DWORD errorCode = GetLastError();
         QCD_Printf(Error, "QcDevice::OpenDevice: Error 0x%x\n, attempt read only", errorCode);
         std::string strDeviceName(static_cast<char*>(pDevName));     
      }
      ConfigureCommChannel(QC_DEV_TYPE_PORTS, hDevice, Baudrate, IslegacyTimeoutConfig);
   }
   else
   {
      QCD_Printf(Error, "QcDevice::OpenDevice: cannot find device <%ws>\n", (PCHAR)DeviceName);
   }

   QCD_Printf(Info, "<--QcDevice::OpenDevice: <%ws> 0x%x\n", DeviceName, hDevice);
   return hDevice;
}  // OpenDevice

QCDEVLIB_API VOID QcDevice::CloseDevice(HANDLE hDevice)
{
   QCD_Printf(Info, "QcDevice::CloseDevice: 0x%x\n", hDevice);
   if (_IsQcomLibusbEnable == TRUE) {
       QcomLibusbDevice::qcom_libusb_close(&hDevice);
   }
   else {
       ::CloseHandle(hDevice);
   }
}  // CloseDevice

QCDEVLIB_API BOOL QcDevice::ReadFromDevice
(
   HANDLE hDevice,
   PVOID  RxBuffer,
   DWORD  NumBytesToRead,
   LPDWORD NumBytesReturned
)
{
   OVERLAPPED ov;
   BOOL       bResult = FALSE;
   DWORD      dwStatus = NO_ERROR;

   QCD_Printf(Debug, "-->QcDevice::ReadFromDevice: 0x%x bufferSize %d bytes\n", hDevice, NumBytesToRead);

   if (_IsQcomLibusbEnable == TRUE) {
       BOOL bytesTransferred = 0;
       bResult = QcomLibusbDevice::qcom_libusb_read(&hDevice, RxBuffer, NumBytesToRead, &bytesTransferred, 0);
       *NumBytesReturned = bytesTransferred;
       if (bResult == LIBUSB_SUCCESS)
       {
           QCD_Printf(Debug, "%s: Handle: 0x%x, readFrom %d bytes, ActualNumBytesReturned: %d\n", __func__, hDevice, NumBytesToRead, bytesTransferred);
           return TRUE;
       }
       else if (bResult == LIBUSB_ERROR_TIMEOUT)
       {
          QCD_Printf(Debug, "%s: Timeout!! Handle: 0x%x, readFrom %d bytes, ActualNumBytesReturned: %d\n", __func__, hDevice, NumBytesToRead, bytesTransferred);
          return TRUE;
       }
       else
       {
           QCD_Printf(Debug, "%s: Failed!! Handle: 0x%x, readFrom %d bytes, ActualNumBytesReturned: %d, result: %d\n", __func__, hDevice, NumBytesToRead, bytesTransferred, bResult);
           return FALSE;
       }
   }
   else {

       ZeroMemory(&ov, sizeof(ov));
       ov.Offset = 0;
       ov.OffsetHigh = 0;
       ov.hEvent = CreateEvent(NULL, TRUE, FALSE, NULL);
       if (ov.hEvent == NULL)
       {
           QCD_Printf(Error, "QcDevice::ReadFromDevice event error %u\n", GetLastError());
           return bResult;
       }
       *NumBytesReturned = 0;

       bResult = ::ReadFile
       (
           hDevice,
           RxBuffer,
           NumBytesToRead,
           NumBytesReturned,
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
               QCD_Printf(Error, "QcDevice::ReadFromDevice failure, error %u\n", dwStatus);
           }
           else
           {
               bResult = GetOverlappedResult
               (
                   hDevice,
                   &ov,
                   NumBytesReturned,
                   TRUE  // no return until operaqtion completes
               );

               if (bResult == FALSE)
               {
                   dwStatus = GetLastError();
                   QCD_Printf(Error, "QcDevice::ReadFromDevice/GetOverlappedResult failure %u\n", dwStatus);
                   if (dwStatus == ERROR_TIMEOUT)
                   {
                       *NumBytesReturned = 0;
                       bResult = TRUE;
                   }
               }
           }
       }

       if (ov.hEvent != NULL)
       {
           CloseHandle(ov.hEvent);
       }
       QCD_Printf(Debug, "<--QcDevice::ReadFromDevice: 0x%x read %d bytes (result %d)\n", hDevice, *NumBytesReturned, bResult);
   }

   return bResult;
}  // ReadFromDevice

QCDEVLIB_API BOOL QcDevice::SendToDevice
(
   HANDLE hDevice,
   PVOID  TxBuffer,
   DWORD  NumBytesToSend,
   LPDWORD NumBytesSent
)
{
   BOOL       bResult = FALSE;
   OVERLAPPED ov;
   DWORD      dwStatus = NO_ERROR;

   QCD_Printf(Debug, "-->QcDevice::SendToDevice: 0x%x %d bytes\n", hDevice, NumBytesToSend);

   if (_IsQcomLibusbEnable == TRUE) {
       BOOL bytesTransferred = 0;
       bResult = QcomLibusbDevice::qcom_libusb_write(&hDevice, TxBuffer, NumBytesToSend, &bytesTransferred, 0);
       *NumBytesSent = bytesTransferred;
       if (bResult == LIBUSB_SUCCESS)
       {
           QCD_Printf(Debug, "%s: Handle: 0x%x, sentTo %d bytes, ActualNumBytesSent: %d\n", __func__, hDevice, NumBytesToSend, bytesTransferred);
           return TRUE;
       }
       else
       {
           QCD_Printf(Debug, "%s: Failed!! Handle: 0x%x, sentTo %d bytes, Actual sent: %d, result: %d\n", __func__, hDevice, NumBytesToSend, bytesTransferred, bResult);
           return FALSE;
       }
   }
   else {
       ZeroMemory(&ov, sizeof(ov));
       ov.Offset = 0;
       ov.OffsetHigh = 0;
       ov.hEvent = CreateEvent(NULL, TRUE, FALSE, NULL);
       if (ov.hEvent == NULL)
       {
           QCD_Printf(Error, "SendToDevice error, event error %u\n", GetLastError());
           return bResult;
       }

       *NumBytesSent = 0;

       if (NumBytesToSend != 0)
       {
           bResult = ::WriteFile
           (
               hDevice,
               TxBuffer,
               NumBytesToSend,
               NumBytesSent,
               &ov
           );
           if (bResult == FALSE)
           {
               dwStatus = GetLastError();

               if (ERROR_IO_PENDING != dwStatus)
               {
                   QCD_Printf(Error, "QcDevice::SendToDevice-0 error %u\n", dwStatus);
               }
               else
               {
                   bResult = GetOverlappedResult
                   (
                       hDevice,
                       &ov,
                       NumBytesSent,
                       TRUE  // no return until operaqtion completes
                   );

                   if (bResult == FALSE)
                   {
                       dwStatus = GetLastError();
                       QCD_Printf(Error, "QcDevice::SendToDevice-1 %u\n", dwStatus);
                   }
               }
           }
       }
       else
       {
           QCD_Printf(Info, "QcDevice::SendToDevice - nothing to send\n");
       }

       if (ov.hEvent != NULL)
       {
           CloseHandle(ov.hEvent);
       }

       QCD_Printf(Debug, "-->QcDevice::SendToDevice: 0x%x sent %d bytes (result %d)\n", hDevice, *NumBytesSent, bResult);
   }
   return bResult;
}  // SendToDevice
