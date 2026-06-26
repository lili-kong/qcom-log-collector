// Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
// SPDX-License-Identifier: BSD 3-Clause Clear License
/*====*====*====*====*====*====*====*====*====*====*====*====*====*====*====*

                              S A M P L E D I S C O V E R Y . C P P

GENERAL DESCRIPTION

  To get notified on device arrival/departue events

*====*====*====*====*====*====*====*====*====*====*====*====*====*====*====*/
#include <dlfcn.h>
#include <qdpublic.h>
#include <qdaio.h>
#include "lpcpublic.h"

PCHAR DevTypeName[4] =
{
   (PCHAR)"NONE",
   (PCHAR)"NET",
   (PCHAR)"PORTS/MODEM",
   (PCHAR)"USB"
};

//typedef VOID (*TASK_CALLBACK)(PDEVICE_CONTEXT DevInfo);

#ifdef __cplusplus
extern "C" {
#endif
static TASK_CALLBACK appTaskCb;

pthread_mutex_t mutex = PTHREAD_MUTEX_INITIALIZER;
pthread_cond_t cond = PTHREAD_COND_INITIALIZER;

typedef struct aioCbCtx {
    pthread_mutex_t mutex;
    pthread_cond_t cond;
    ULONG   ret;
    PVOID UserData;
}AIO_CB_CTX, *PAIO_CB_CTX;

typedef struct aioGblCtx {
    PDEVICE_CONTEXT     pAioDevCtx;
    struct aioGblCtx    *pNext;
} AIO_GBL_CTX, *PAIO_GBL_CTX;

PAIO_GBL_CTX pAioGblCtx = NULL;

// Logging callback, DLL directs log messages to Windows logging if CB is not set
VOID LogMsgCb(PCHAR Msg)
{
   printf("<QCD_PREFIX> %s", Msg);
}

void *TriggerUserCb(void *Userdata)
{
   PDEVICE_CONTEXT devCtx = NULL;
   PAIO_GBL_CTX pAioCtx = NULL;
   PAIO_GBL_CTX pTmpCtx = NULL;
   PAIO_GBL_CTX pPrevCtx= NULL;

   devCtx = (PDEVICE_CONTEXT)Userdata;

   if (!devCtx)
   {
       printf("Invalid data\n");
       return NULL;
   }

   pAioCtx = (PAIO_GBL_CTX)malloc(sizeof(struct aioGblCtx));
   if (!pAioCtx)
   {
       printf("Unable to allocate memory");
       return NULL;
   }

   pAioCtx->pAioDevCtx = devCtx;
   pAioCtx->pNext = NULL;

   pthread_mutex_lock(&mutex);

   if (pAioGblCtx == NULL)
   {
       pAioGblCtx = pAioCtx;
   }
   else
   {
       pAioCtx->pNext = pAioGblCtx;
       pAioGblCtx = pAioCtx;
   }

   pthread_mutex_unlock(&mutex);

   devCtx->DevHandle = QDAIO::OpenDevice(devCtx->SymbolicName);
   printf("devCtx->DevHandle : %d, %s\n", devCtx->DevHandle, devCtx->SymbolicName);
   appTaskCb(devCtx);
   QDAIO::CloseDevice(devCtx->DevHandle);
   pthread_mutex_lock(&mutex);
   pTmpCtx = pAioGblCtx;

   if (pTmpCtx != NULL && pTmpCtx->pAioDevCtx == devCtx)
   {
       //pAioCtx = pAioCtx->pNext;
       pAioGblCtx = pAioGblCtx->pNext;
       free(devCtx);
       free(pTmpCtx);
   }
   else
   {
       while (pTmpCtx != NULL && pTmpCtx->pAioDevCtx != devCtx)
       {
           pPrevCtx = pTmpCtx;
           pTmpCtx = pTmpCtx->pNext;
       }
       if (pTmpCtx != NULL)
       {
           pPrevCtx->pNext = pTmpCtx->pNext;
           free(devCtx);
           free(pTmpCtx);
       }
   }

   pthread_cond_signal(&cond);
   pthread_mutex_unlock(&mutex);

   return NULL;
}

// Device-change callback (device arrival/departure)
VOID MyDeviceChangeCb(PCB_PARAMS CbParams, PVOID *Context)
{
   DWORD id = 0;
   UCHAR devState = (UCHAR)((CbParams->Flag & QC_FLAG_MASK_DEV_STATE) >> 4);
   UCHAR devType  = (UCHAR)((CbParams->Flag & QC_FLAG_MASK_DEV_TYPE) >> 8);
   UCHAR isQCDriver = (UCHAR)(CbParams->Flag & QC_FLAG_MASK_QC_DRIVER);
   PDEVICE_CONTEXT devCtx = NULL;

   id = *Context;
   if (devState == QC_DEV_STATE_ARRIVAL)
   {
      pthread_t thId;
      *Context = (PVOID)(id++);

      printf("  ->Arrival: Q[%d] %s <%s> [%s] Vid Context 0x%p\n",
              isQCDriver, (PCHAR)DevTypeName[devType], (PCHAR)CbParams->DevDesc, (PCHAR)(CbParams->DevName), *Context);
      if (devType == QC_DEV_TYPE_NET)
      {
         printf("       >>InterfaceName: <%s>\n", (PTSTR)CbParams->IfName);
         printf("       >>MTU          : <%lu>\n", CbParams->Mtu);
      }

      //if (CbParams->DevPid) //== )
	  if(strstr((PCHAR)(CbParams->DevName),"QCOM_LPC_DEVICE") != NULL)
	  {
          devCtx = (PDEVICE_CONTEXT)malloc(sizeof(DEVICE_CONTEXT));

          devCtx->Cid = (ULONG)*Context;
          strncpy(devCtx->DevName, (char*)CbParams->DevDesc, QC_NAME_MAX);
          strncpy(devCtx->SymbolicName, CbParams->DevName, QC_NAME_MAX);

          pthread_create(&thId, NULL, TriggerUserCb, devCtx);
      }

   }
   else if (devState == QC_DEV_STATE_DEPARTURE)
   {
      printf("  ->Departure: Q[%d] %s <%s> [%s] Context (0x%p)\n",
              isQCDriver, (PCHAR)DevTypeName[devType], (PCHAR)CbParams->DevDesc, (PCHAR)(CbParams->DevName), *Context);
   }
   //printf("       >>Bus Location <%s> SerialNumber <%s>, pid : <0x%lx>\n\n", (PTSTR)CbParams->Loc, (PTSTR)CbParams->SerNum, CbParams->DevPid);

   return;
}

void aioCallback_w(LONG AppHdl, PVOID *Context, ULONG Status, ULONG IoSize)
{
    PAIO_CONTEXT userCtx;
    PAIO_CB_CTX  callbackCtx;
    PCHAR Buffer;
    int i = 0;

    userCtx = (PAIO_CONTEXT)Context;
    callbackCtx = (PAIO_CB_CTX)userCtx->UserData;
    Buffer = (PCHAR)callbackCtx->UserData;
    callbackCtx->ret = IoSize;

    pthread_mutex_lock(&callbackCtx->mutex);
    pthread_cond_signal(&callbackCtx->cond);
    pthread_mutex_unlock(&callbackCtx->mutex);

    return;
}

void aioCallback_r(LONG AppHdl, PVOID *Context, ULONG Status, ULONG IoSize)
{
    PAIO_CONTEXT userCtx;
    PAIO_CB_CTX  callbackCtx;
    PCHAR Buffer;

    userCtx = (PAIO_CONTEXT)Context;
    callbackCtx = (PAIO_CB_CTX)userCtx->UserData;
    callbackCtx->ret = IoSize;

    pthread_mutex_lock(&callbackCtx->mutex);
    pthread_cond_signal(&callbackCtx->cond);
    pthread_mutex_unlock(&callbackCtx->mutex);

    return;
}

BOOL QDDLL_SendToDevice(HANDLE handle, PVOID Buffer, DWORD BufferSize, LPDWORD ActualSize)
{
    AIO_CONTEXT userCtx;
    AIO_CB_CTX  callbackCtx; 

    userCtx.Callback = aioCallback_w;
    callbackCtx.mutex = PTHREAD_MUTEX_INITIALIZER;
    callbackCtx.cond  = PTHREAD_COND_INITIALIZER;
    callbackCtx.UserData = Buffer;

    userCtx.UserData = &callbackCtx;

    //unsigned char req[] = { 0x0C, 0x14, 0x3A, 0x7E, 0x0};

    pthread_mutex_lock(&callbackCtx.mutex);
    QDAIO::Send(handle, (PCHAR)Buffer, BufferSize, (PAIO_CONTEXT)&userCtx);
    //QDAIO::Send(handle, (PCHAR)req, 5, (PAIO_CONTEXT)&userCtx);
    //*ActualSize = 5;

    pthread_cond_wait(&callbackCtx.cond, &callbackCtx.mutex);
    pthread_mutex_unlock(&callbackCtx.mutex);
    //*ActualSize = BufferSize;
    *ActualSize = callbackCtx.ret;
    if (*ActualSize != BufferSize)
    {
        return FALSE;
    }
    return TRUE;
}

BOOL QDDLL_ReadFromDevice(HANDLE handle, PVOID Buffer, DWORD BufferSize, LPDWORD ActualSize)
{
    AIO_CONTEXT userCtx;
    AIO_CB_CTX  callbackCtx; 

    userCtx.Callback = aioCallback_r;
    callbackCtx.mutex = PTHREAD_MUTEX_INITIALIZER;
    callbackCtx.cond  = PTHREAD_COND_INITIALIZER;
    callbackCtx.UserData = Buffer;

    userCtx.UserData = &callbackCtx;
    pthread_mutex_lock(&callbackCtx.mutex);
    QDAIO::Read(handle, Buffer, BufferSize, (PAIO_CONTEXT)&userCtx);
    //QDAIO::Read(handle, Buffer, 51, (PAIO_CONTEXT)&userCtx);
    pthread_cond_wait(&callbackCtx.cond, &callbackCtx.mutex);
    //*ActualSize = 51;
    //*ActualSize = BufferSize;
    *ActualSize = callbackCtx.ret;
    pthread_mutex_unlock(&callbackCtx.mutex);
    if (*ActualSize != BufferSize)
    {
        return FALSE;
    }

    return TRUE;
}

ULONG QDDLL_GetDeviceList(PVOID Buffer, ULONG BufferSize, PULONG ActualSize)
{
    printf("%s:%d> Yet to be Implemented\n", __func__, __LINE__);
    return TRUE;
}

BOOL QDDLL_InitializeTask(TASK_CALLBACK TaskCb)
{
    DEV_FEATURE_SETTING     mySetting;
    ULONG numDev, actualSize,AppContext;

    if (!TaskCb)
    {
        printf("Invalid callback\n");
        return FALSE;
    }

    mySetting.Version = 1;
    mySetting.Settings = DEV_FEATURE_INCLUDE_NONE_QC_PORTS | DEV_FEATURE_SCAN_USB_WITH_VID;
    //mySetting.DeviceClass = DEV_CLASS_PORTS; // DEV_CLASS_NET | DEV_CLASS_PORTS | DEV_CLASS_USB | DEV_CLASS_MDM;
    mySetting.DeviceClass =  DEV_CLASS_NET | DEV_CLASS_PORTS | DEV_CLASS_USB | DEV_CLASS_MDM; //To filter based on Class
    mySetting.DeviceClass =  DEV_CLASS_PORTS; //To filter based on Class
    mySetting.VID = (PTSTR)TEXT("VID_05C6"); //To filter based on VID
    //mySetting.VID = (PTSTR)TEXT(NULL);

    QcDevice::SetLoggingCallback(LogMsgCb);

    QcDevice::SetDeviceChangeCallback(MyDeviceChangeCb , (PVOID)&AppContext);

    QcDevice::SetFeature((PVOID)&mySetting);

    appTaskCb = TaskCb;

    return TRUE;
} 

void *StartDiscoveryThread(void *userData)
{
    QcDevice::StartDeviceMonitor();
    return NULL;
}

VOID QDDLL_WaitAndFinishTask(VOID)
{
    pthread_t thId;
    PAIO_GBL_CTX ctx;

    QDAIO::Initialize();
    pthread_mutex_lock(&mutex);
    pthread_create(&thId, NULL, StartDiscoveryThread, NULL);

    while (1) {
        pthread_cond_wait(&cond, &mutex);

        //QDAIO::AsyncCloseDevice(appHdl);
        if (!pAioGblCtx)
        {
            sleep(1);
            QcDevice::StopDeviceMonitor();
            printf("After Stop Device Monitor\n");
            break;
        }
    }
    pthread_mutex_unlock(&mutex);

    pthread_exit(NULL);
    
    return;
}

#ifdef __cplusplus
}
#endif

