// Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
// SPDX-License-Identifier: BSD 3-Clause Clear License
#include "qdaio.h"
#include "QdIoQmi.h"
#include "qdutils.h"
#include <stdio.h>       /* for perror() */
#include <unistd.h>      /* for syscall() */
#include <sys/syscall.h> /* for __NR_* definitions */
#include <sys/ioctl.h>
#include <linux/aio_abi.h> /* for AIO types and constants */
#include <fcntl.h>         /* O_RDWR */
#include <errno.h>
#include <string.h>        /* memset() */
#include <inttypes.h>      /* uint64_t */
#include <pthread.h>

using namespace QDAIO;

inline int io_setup(unsigned nr, aio_context_t *ctxp)
{
    return syscall(__NR_io_setup, nr, ctxp);
}

inline int io_destroy(aio_context_t ctx)
{
    return syscall(__NR_io_destroy, ctx);
}

inline int io_submit(aio_context_t ctx, long nr, struct iocb **iocbpp)
{
    return syscall(__NR_io_submit, ctx, nr, iocbpp);
}

inline int io_getevents(aio_context_t ctx, long min_nr, long max_nr,
                        struct io_event *events, struct timespec *timeout)
{
    return syscall(__NR_io_getevents, ctx, min_nr, max_nr, events, timeout);
}

inline int io_cancel(aio_context_t ctx, struct iocb *iocb,
                     struct io_event *result)
{
    return syscall(__NR_io_cancel, ctx, iocb, result);
}

inline int min(int a, int b)
{
    return a > b ? a : b;
}
namespace QDAIO
{
QDAIODEV AioDevice[QDA_MAX_DEV];
LONG DevInitialized = 0;
CRITICAL_SECTION MasterLock; /* pthread_mutex_t */

namespace // unnamed
{
VOID InitDevExtension(PQDAIODEV_EXTENSION Ext)
{
    PQDAIODEV_EXTENSION pDevExt = Ext;

    QCDEV_LOG_DBG("InitDevExtension 0x%p, 0x%p\n", pDevExt, &pDevExt->CancelQueueRx);

    // initialize extension
    pDevExt->DevHandle = INVALID_HANDLE_VALUE;
    InitializeListHead(&pDevExt->DispatchQueueRx);
    InitializeListHead(&pDevExt->DispatchQueueTx);
    InitializeListHead(&pDevExt->PendingQueueRx);
    InitializeListHead(&pDevExt->PendingQueueTx);
    InitializeListHead(&pDevExt->CancelQueueRx);
    InitializeListHead(&pDevExt->CancelQueueTx);

    pthread_cond_init(&pDevExt->DispatchEvtTx, NULL);
    pthread_cond_init(&pDevExt->DispatchEvtRx, NULL);

    return;
}

VOID PurgeDevExtension(PQDAIODEV Dev)
{
    PQDAIODEV_EXTENSION pDevExt = (PQDAIODEV_EXTENSION)Dev->DevExtension;
    struct timespec ts;
    int ret;
    QCDEV_LOG_DBG("QDAIO::PurgeDevExtension 0x%p\n", pDevExt);

    EnterCriticalSection(&MasterLock);

    while (Dev->Ref > QCDEV_MIN_REF_CNT)
    {
        clock_gettime(CLOCK_REALTIME, &ts);
        ts.tv_sec += (2);
        ret = pthread_cond_timedwait(&Dev->CancelEvt, &MasterLock, &ts);
        if ((ret == ETIMEDOUT) || (ret == 0))
        {
            QCDEV_LOG_ERR("QDAIO::PurgeDevExtension,  Dev->Ref : %d\n", Dev->Ref);
            continue;
        }
        break; //No need to wait in case of error
    }

    Dev->DevExtension = NULL;
    pthread_cond_signal(&pDevExt->DispatchEvtRx);
    pthread_cond_signal(&pDevExt->DispatchEvtTx);

    while (Dev->Ref > 0)
    {
        clock_gettime(CLOCK_REALTIME, &ts);
        ts.tv_sec += (2);
        ret = pthread_cond_timedwait(&Dev->CancelEvt, &MasterLock, &ts);
        if ((ret == ETIMEDOUT) || (ret == 0))
        {
            QCDEV_LOG_ERR("QDAIO::PurgeDevExtension, Dev->Ref : %d\n", Dev->Ref);
            continue;
        }
        break; //No need to wait in case of error
    }
    pthread_cond_destroy(&pDevExt->DispatchEvtRx);
    pthread_cond_destroy(&pDevExt->DispatchEvtTx);
    io_destroy(pDevExt->AioIdCtxRx);
    io_destroy(pDevExt->AioIdCtxTx);
    delete pDevExt;
    Dev->DevExtension = NULL;
    LeaveCriticalSection(&MasterLock);
    return;
}

PVOID WINAPI CancelThread(PVOID Context)
{
    QCDEV_LOG_DBG("In -->\n");
    PQDAIODEV pIoDev = (PQDAIODEV)Context;
    PAIO_ITEM item;
    LIST_ENTRY *pEntry;
    struct io_event events[1];

    PAIO_CONTEXT pAioCtxt;

    QCDEV_LOG_ERR("QDAIO::CancelThread IoDev 0x%p Ext 0x%p\n", pIoDev->DevExtension, Context);

    // cancel RX
    EnterCriticalSection(&pIoDev->DevLockRx);
    while (!IsListEmpty(&(pIoDev->DevExtension->CancelQueueRx)))
    {
        pEntry = RemoveHeadList(&(pIoDev->DevExtension->CancelQueueRx));
        item = CONTAINING_RECORD(pEntry, AIO_ITEM, List);
        pAioCtxt = (PAIO_CONTEXT)item->UserContext;
        LeaveCriticalSection(&pIoDev->DevLockRx);
        //TODO: Need to finalize the cancel callback
        //pAioCtxt->Callback(pIoDev->DevExtension->DevHandle, item->UserContext, -1, 0);
        //pAioCtxt->Callback = NULL;
        io_cancel(pIoDev->DevExtension->AioIdCtxRx, &item->ioCntrlBlock, &events[0]);

        EnterCriticalSection(&pIoDev->DevLockRx);
    }
    LeaveCriticalSection(&pIoDev->DevLockRx);

    // cancel TX
    EnterCriticalSection(&pIoDev->DevLockTx);
    while (!IsListEmpty(&(pIoDev->DevExtension->CancelQueueTx)))
    {
        pEntry = RemoveHeadList(&(pIoDev->DevExtension->CancelQueueTx));
        item = CONTAINING_RECORD(pEntry, AIO_ITEM, List);
        pAioCtxt = (PAIO_CONTEXT)item->UserContext;
        LeaveCriticalSection(&pIoDev->DevLockTx);
        //TODO: Need to finalize the cancel callback
        //pAioCtxt->Callback(pIoDev->DevExtension->DevHandle, item->UserContext, -1, 0);
        //pAioCtxt->Callback = NULL;
        io_cancel(pIoDev->DevExtension->AioIdCtxTx, &item->ioCntrlBlock, &events[0]);
        EnterCriticalSection(&pIoDev->DevLockTx);
    }
    LeaveCriticalSection(&pIoDev->DevLockTx);

    EnterCriticalSection(&MasterLock);
    InterlockedDecrement(&pIoDev->Ref);
    pthread_cond_signal(&pIoDev->CancelEvt);
    LeaveCriticalSection(&MasterLock);

    QCDEV_LOG_DBG("Out <--\n");
    return 0;
} // CancelThread

int FindAndCancel(PQDAIODEV Dev, int IoType, PVOID Context)
{
    QCDEV_LOG_DBG("In -->\n");
    //PQDAIO_THREAD_CONTEXT ioContext;
    PQDAIODEV pIoDev;
    PAIO_ITEM item;
    CRITICAL_SECTION devLock;
    LIST_ENTRY *dispatchQueue;
    LIST_ENTRY *pendingQueue;
    LIST_ENTRY *cancelQueue;
    PLIST_ENTRY peekItem;
    int itemFound = 0;

    //pIoDev = ioContext->Dev;
    pIoDev = Dev;
    QCDEV_LOG_DBG("In --> IoDev 0x%p Ext 0x%p Ctxt 0x%p\n", pIoDev, pIoDev->DevExtension, Context);
    //if (ioContext->IoType == 0)
    if (IoType == 0)
    {
        devLock = pIoDev->DevLockRx;
        dispatchQueue = &pIoDev->DevExtension->DispatchQueueRx;
        pendingQueue = &pIoDev->DevExtension->PendingQueueRx;
        cancelQueue = &pIoDev->DevExtension->CancelQueueRx;
    }
    else
    {
        devLock = pIoDev->DevLockTx;
        dispatchQueue = &pIoDev->DevExtension->DispatchQueueTx;
        pendingQueue = &pIoDev->DevExtension->PendingQueueTx;
        cancelQueue = &pIoDev->DevExtension->CancelQueueTx;
    }

    EnterCriticalSection(&devLock);
    if (!IsListEmpty(dispatchQueue))
    {
        // find item and en-queue to CancelQueue
        peekItem = dispatchQueue->Flink;
        while (peekItem != dispatchQueue)
        {
            item = CONTAINING_RECORD(peekItem, AIO_ITEM, List);
            peekItem = peekItem->Flink;
            if (NULL == Context)
            {
                RemoveEntryList(&item->List);
                InsertTailList(cancelQueue, &item->List);
                break;
            }
            else if (item->UserContext == Context)
            {
                RemoveEntryList(&item->List);
                InsertTailList(cancelQueue, &item->List);
                itemFound = 1;
                break;
            }
        }
    }

    // create and execute cancel thread
    InterlockedIncrement(&Dev->Ref);

    if (0 != pthread_create((pthread_t *)&pIoDev->DevExtension->CancelThread, NULL, CancelThread, (PVOID)Dev))
    {
        QCDEV_LOG_DBG("Failed to create thread\n");
        pIoDev->DevExtension->CancelThread = 0;
    }

    if (!IsListEmpty(pendingQueue))
    {
        if (NULL == Context)
        {
            peekItem = pendingQueue->Flink;
            item = CONTAINING_RECORD(peekItem, AIO_ITEM, List);
            peekItem = peekItem->Flink;
            RemoveEntryList(&item->List);
            InsertTailList(cancelQueue, &item->List);
            //close(pIoDev->DevExtension->DevHandle);
            itemFound = 1;
        }
        else if (itemFound == 0)
        {
            // find item and en-queue to CancelQueue
            peekItem = pendingQueue->Flink;
            while (peekItem != pendingQueue)
            {
                item = CONTAINING_RECORD(peekItem, AIO_ITEM, List);
                peekItem = peekItem->Flink;
                if (item->UserContext == Context)
                {
                    RemoveEntryList(&item->List);
                    InsertTailList(cancelQueue, &item->List);
                    itemFound = 1;
                    //close(pIoDev->DevExtension->DevHandle);
                    break;
                }
            }
        }
    }
    LeaveCriticalSection(&devLock);
    QCDEV_LOG_DBG("Out <--\n");
    return itemFound;
} // FindAndCancel

//DWORD WINAPI IoThreadLnx(PVOID Context)
PVOID WINAPI IoThreadLnx(PVOID Context)
{
    PQDAIODEV pIoDev;
    HANDLE ioHandle[QDAIO_MAX_NUM_EVENTS];
    PAIO_ITEM ioItem[QDAIO_MAX_NUM_EVENTS];
    struct io_event ioDispatchEvt[QDAIO_MAX_NUM_EVENTS];
    aio_context_t *pAioIdCtx;
    int ret = 0;
    LIST_ENTRY *dispatchQueue;
    LIST_ENTRY *pendingQueue;
    CRITICAL_SECTION devLock;
    PQDAIO_THREAD_CONTEXT ioContext;
    PLIST_ENTRY pEntry;
    int nDequeued;
    pthread_cond_t *dispatchEvt;
    DWORD waitIdx;

    ioContext = (PQDAIO_THREAD_CONTEXT)Context;

    QCDEV_LOG_DBG("%s : Ctx 0x%p\n", ioContext->IoType ? "Tx" : "Rx", Context);

    pIoDev = ioContext->Dev;
    if (ioContext->IoType == 0)
    {
        devLock = pIoDev->DevLockRx;
        dispatchEvt = &pIoDev->DevExtension->DispatchEvtRx;
        dispatchQueue = &pIoDev->DevExtension->DispatchQueueRx;
        pendingQueue = &pIoDev->DevExtension->PendingQueueRx;
        pAioIdCtx = &pIoDev->DevExtension->AioIdCtxRx;
    }
    else
    {
        devLock = pIoDev->DevLockTx;
        dispatchEvt = &pIoDev->DevExtension->DispatchEvtTx;
        dispatchQueue = &pIoDev->DevExtension->DispatchQueueTx;
        pendingQueue = &pIoDev->DevExtension->PendingQueueTx;
        pAioIdCtx = &pIoDev->DevExtension->AioIdCtxTx;
    }

    while (pIoDev->DevExtension != NULL)
    {
        int idx = 0;
        int waitState;
        struct timespec ts;

        EnterCriticalSection(&devLock);

        clock_gettime(CLOCK_REALTIME, &ts);
        ts.tv_sec += 2;

        waitState = pthread_cond_timedwait(dispatchEvt, &devLock, &ts);

        if (waitState == 0)
        {
            QCDEV_LOG_DBG("%s : Event Triggered\n", ioContext->IoType ? "Tx" : "Rx");
        }
        else if (waitState == ETIMEDOUT)
        {
            QCDEV_LOG_DBG("%s : No Event Triggered - Timedout\n", ioContext->IoType ? "Tx" : "Rx");
        }
        else
        {
            QCDEV_LOG_DBG("%s : No Event Triggered wait state : %d\n", ioContext->IoType ? "Tx" : "Rx", waitState);
        }

        // dequeue DispatchQueue up to 64 items for each WAIT
        nDequeued = 0;
        while (!IsListEmpty(dispatchQueue) && (pIoDev->DevState == QDAIO_DEV_STATE_OPEN))
        {
            pEntry = RemoveHeadList(dispatchQueue);

            ioItem[nDequeued] = CONTAINING_RECORD(pEntry, AIO_ITEM, List);
            InsertTailList(pendingQueue, &(ioItem[nDequeued]->List));
            if (++nDequeued >= QDAIO_MAX_NUM_EVENTS) // max 64 items
            {
                break;
            }
        }

        LeaveCriticalSection(&devLock);

        if ((pIoDev->DevState == QDAIO_DEV_STATE_CLOSE) && IsListEmpty(pendingQueue))
        {
            QCDEV_LOG_ERR("%s : QcDevice::IoThread[%d]: Exit...\n", ioContext->IoType ? "Tx" : "Rx", ioContext->IoType);
            break;
        }

        // wait for completion
        while (!IsListEmpty(pendingQueue) && (nDequeued > 0)) // && (pIoDev->DevState != QDAIO_DEV_STATE_CLOSE))
        {
            BOOLEAN ioResult;
            struct timespec timeout;
            struct iocb *obj;
            PAIO_ITEM localIoItem;

#define USB_TIMEOUT 300
            timeout.tv_sec = USB_TIMEOUT;
            timeout.tv_nsec = 0;
            /* 
                     * Wait upto min 1 Evt or get 'QDAIO_MAX_NUM_EVENTS' from the
                     * completion queue of the AIO context
                     */
            /* There is a possibility of receiving events, before adding to pending queue 
		     * So restricting the events to, count of pending Queue..!,
                     * else need to modify logic, to match with the elements of dispatch queue and process
		     */
            //ret = io_getevents(*pAioIdCtx, 1, nDequeued, ioDispatchEvt, &timeout);
            ret = io_getevents(*pAioIdCtx, 1, min(nDequeued, QDAIO_MAX_NUM_EVENTS), ioDispatchEvt, &timeout);
            if (ret == 0)
            {
                QCDEV_LOG_ERR("%s : No Event Found and timeout has elapsed \n", ioContext->IoType ? "Tx" : "Rx");
                continue;
            }
            else if (ret > 0)
            {
                int resp = 0;
                //idx = 0;/*every time it is resetting to first io item which is calling wrong callback */
                nDequeued -= ret;

                QCDEV_LOG_DBG("%s :  num of events : %d, len : %lld\n",
                              ioContext->IoType ? "Tx" : "Rx", ret, ioDispatchEvt[resp].res);
                if (ioContext->IoType == false)
                {
                    while ((resp < ret))
                    {
                        obj = (struct iocb *)ioDispatchEvt[resp].obj;
                        localIoItem = CONTAINING_RECORD(obj, AIO_ITEM, ioCntrlBlock);

                        EnterCriticalSection(&devLock);
                        RemoveEntryList(&(ioItem[idx]->List));
                        LeaveCriticalSection(&devLock);

                        ioItem[idx]->IoSize = ioDispatchEvt[resp].res;
                        if (ioItem[idx]->IoCallback != NULL)
                        {
                            AIO_CALLBACK IoCallback;
                            IoCallback = ioItem[idx]->IoCallback;
                            IoCallback(0, ioItem[idx]->UserContext, ioDispatchEvt[resp].res2, ioItem[idx]->IoSize);
                            //IoCallback(0, obj->aio_buf, ioDispatchEvt[resp].res2, ioItem[idx]->IoSize);
                        }
                        delete localIoItem;
                        idx++;
                        resp++;
                    }
                }
                else
                {
                    while ((resp < ret))
                    {
                        obj = (struct iocb *)ioDispatchEvt[resp].obj;
                        localIoItem = CONTAINING_RECORD(obj, AIO_ITEM, ioCntrlBlock);

                        EnterCriticalSection(&devLock);
                        RemoveEntryList(&(ioItem[idx]->List));
                        LeaveCriticalSection(&devLock);

                        ioItem[idx]->IoSize = ioDispatchEvt[resp].res;
                        if ((ioItem[idx]->IoCallback != NULL))
                        {
                            AIO_CALLBACK IoCallback;
                            IoCallback = ioItem[idx]->IoCallback;
                            IoCallback(0, ioItem[idx]->UserContext, ioDispatchEvt[resp].res2, ioItem[idx]->IoSize);
                            //IoCallback(0, obj->aio_buf, ioDispatchEvt[resp].res2, ioItem[idx]->IoSize);
                        }
                        delete localIoItem;
                        resp++;
                        idx++;
                    }
                }
            }
            else
            {
                QCDEV_LOG_ERR("QcDevice::io_getevents: Error 0x%lx\n", GetLastError());
                //Handle accordingly
            }
        }
    }
    EnterCriticalSection(&MasterLock);
    InterlockedDecrement(&pIoDev->Ref);
    pthread_cond_signal(&pIoDev->CancelEvt);
    LeaveCriticalSection(&MasterLock);
    return (HANDLE)NULL;
} //IoThreadLnx

} // namespace
} // namespace QDAIO

/**
 * @brief   To initialize the QDAIO for async support
 *
 * To Initialize QDAIO related data
 *
 * @returns Nothing
 */
QCDEVLIB_API VOID QDAIO::Initialize(void)
{
    LONG i, x, y;
    if (InterlockedIncrement(&DevInitialized) >= 1)
    {
        QCDEV_LOG_INFO("already in progress or done!\n");
        InterlockedDecrement(&DevInitialized);
        return;
    }

    InitializeCriticalSection(&MasterLock);
    for (i = 1; i < QDA_MAX_DEV; i++)
    {
        AioDevice[i].AppHandle = i;
        AioDevice[i].DevState = QDAIO_DEV_STATE_UNINIT;
        AioDevice[i].DevExtension = NULL;
        InitializeCriticalSection(&(AioDevice[i].DevLockRx));
        InitializeCriticalSection(&(AioDevice[i].DevLockTx));
        InitializeEventSection(&(AioDevice[i].CancelEvt));
    }

    QCDEV_LOG_DBG("DONE!\n");

    return;
}

/**
 * @brief   To open the selected device
 *
 * Opens the device and returns the device handle
 *
 * @param   DeviceName  Device which is to be opened
 * 
 * @returns appHandle   on success returns app handle
 */
QCDEVLIB_API LONG QDAIO::OpenDevice(PVOID DeviceName)
{
    LONG i, appHdl = -1;
    PQDAIODEV_EXTENSION pDevExt;
    PQDAIO_THREAD_CONTEXT pRx, pTx;

    //PQC_DEV_ITEM pDevInfo;
    PLIST_ENTRY headOfArrival, peekArrival;
    PVOID pDevName = NULL;
    HANDLE hDevice = INVALID_HANDLE_VALUE; // -1
    pDevName = DeviceName;

    QCDEV_LOG_INFO("In -->::DeviceName:%s\n",(PCHAR)pDevName);

    if (pDevName == NULL)
    {
        QCDEV_LOG_ERR("Device Name is NULL \n");
        return QDAIO_STATUS_INVALID_DEVICE;
    }

    EnterCriticalSection(&MasterLock);
    if (DevInitialized == 0)
    {
        LeaveCriticalSection(&MasterLock);
        QCDEV_LOG_ERR("Device didn't initialized \n");
        return QDAIO_STATUS_DEVICE_NOT_INIT;
    }

    // locate a free slot
    for (i = 1; i < QDA_MAX_DEV; i++)
    {
        if (AioDevice[i].DevExtension == NULL)
        {
            appHdl = i;

            // allocate devExtension
            AioDevice[i].DevExtension = pDevExt = new QDAIODEV_EXTENSION;
            AioDevice[i].DevExtension->AioIdCtxTx = 0;
            AioDevice[i].DevExtension->AioIdCtxRx = 0;
            break;
        }
    }
    LeaveCriticalSection(&MasterLock);

    if ((appHdl >= QDA_MAX_DEV) || (appHdl < 1))
    {

        return QDAIO_STATUS_BAD_HANDLE;
    }
    if (pDevExt == NULL)
    {
        return QDAIO_STATUS_NO_DEVICE;
    }

    // initialize extension
    InitDevExtension(pDevExt);

    hDevice = open((PCHAR)pDevName, O_RDWR | O_NONBLOCK | O_ASYNC | O_CLOEXEC);
    if (hDevice == INVALID_HANDLE_VALUE)
    {
        QCDEV_LOG_ERR(" Error 0x%lx\n", GetLastError());
        PurgeDevExtension(&AioDevice[i]);
        return QDAIO_STATUS_HANDLE_FAILURE;
    }
    else
    {
        pDevExt->DevHandle = hDevice; //AioDevice[appHdl].DevExtension.DevHandle = hDevice;
        QCDEV_LOG_DBG("Device handle: %d\n",hDevice);
    }

    pRx = new QDAIO_THREAD_CONTEXT;
    pTx = new QDAIO_THREAD_CONTEXT;
    if (pRx == NULL)
    {
        QCDEV_LOG_ERR(" Error 0x%lx\n", GetLastError());
        close(hDevice);
        PurgeDevExtension(&AioDevice[i]);
        return QDAIO_STATUS_HANDLE_FAILURE;
    }
    else if (pTx == NULL)
    {
        QCDEV_LOG_ERR(" Error 0x%lx\n", GetLastError());
        delete pRx;
        close(hDevice);
        PurgeDevExtension(&AioDevice[i]);
        return QDAIO_STATUS_HANDLE_FAILURE;
    }

    if (io_setup(QDAIO_MAX_NUM_EVENTS, &AioDevice[i].DevExtension->AioIdCtxTx) < 0)
    {
        QCDEV_LOG_ERR(" Error 0x%lx\n", GetLastError());
        delete pRx;
        delete pTx;
        close(hDevice);
        PurgeDevExtension(&AioDevice[i]);
        return QDAIO_STATUS_HANDLE_FAILURE;
    }

    if (io_setup(QDAIO_MAX_NUM_EVENTS, &AioDevice[i].DevExtension->AioIdCtxRx) < 0)
    {
        QCDEV_LOG_ERR(" Error 0x%lx\n", GetLastError());
        delete pRx;
        delete pTx;
        close(hDevice);
        PurgeDevExtension(&AioDevice[i]);
        return QDAIO_STATUS_HANDLE_FAILURE;
    }

    QCDEV_LOG_DBG("Completed IO Setup for Tx and Rx \n");

    pRx->Dev = pTx->Dev = &AioDevice[i];
    pRx->IoType = 0;
    pTx->IoType = 1;

    AioDevice[i].DevState = QDAIO_DEV_STATE_OPEN;
    InterlockedExchange(&(AioDevice[i].Ref), 0);
    InterlockedIncrement(&AioDevice[i].Ref);
    if (0 != pthread_create((pthread_t *)&pDevExt->DispatchThreadRx, NULL, IoThreadLnx, pRx))
    {
        QCDEV_LOG_ERR("Failed to create thread\n");
        pDevExt->DispatchThreadRx = 0;
    }
    InterlockedIncrement(&AioDevice[i].Ref);
    if (0 != pthread_create((pthread_t *)&pDevExt->DispatchThreadTx, NULL, IoThreadLnx, pTx))
    {
        QCDEV_LOG_ERR("Failed to create thread\n");
        pDevExt->DispatchThreadTx = 0;
    }
    QCDEV_LOG_INFO("Out <--\n");
    return appHdl;
}

/**
 * @brief   To close the selected handle
 *
 * Close the device handle
 *
 * @param   AppHandle   app handle
 *
 * @returns status      QDAIO_STATUS_SUCCESS -> success in closing app handle
 *                      (or) any of the QDAIO_STATUS
 */
QCDEVLIB_API LONG QDAIO::CloseDevice(LONG AppHandle)
{
	QCDEV_LOG_INFO("In -->\n");
	LONG appHdl = INVALID_HANDLE_VALUE;

    if ((AppHandle >= QDA_MAX_DEV) || (AppHandle < 1))
    {
        QCDEV_LOG_ERR("Invalid appHdl %ld\n", AppHandle);
        return QDAIO_STATUS_BAD_HANDLE;
    }

    if (AioDevice[AppHandle].DevExtension == NULL)
    {
        QCDEV_LOG_ERR("non-exist appHdl %ld\n", AppHandle);
        return QDAIO_STATUS_NO_DEVICE;
    }

    if (AioDevice[AppHandle].DevState != QDAIO_DEV_STATE_OPEN)
    {
        return QDAIO_STATUS_SUCCESS;
    }
    QDAIO::Cancel(AppHandle, NULL);
    AioDevice[AppHandle].DevState = QDAIO_DEV_STATE_CLOSE;

    //close dev handle
    appHdl = close(AioDevice[AppHandle].DevExtension->DevHandle);
    if (appHdl < 0)
    {
        QCDEV_LOG_ERR("Failed to close device error:%d\n",appHdl);
        return INVALID_HANDLE_VALUE;
    }
    else 
    {
        QCDEV_LOG_INFO("Successfully close dev handle: %d\n", appHdl);

        PurgeDevExtension(&(AioDevice[AppHandle]));

        QCDEV_LOG_INFO("Out <--\n");
    }
    return QDAIO_STATUS_SUCCESS;
}

/**
 * @brief   To write to the selected handle
 *
 * Sends the data on device handle 
 *
 * @param   AppHandle           app handle
 * @param   TxBuffer            Send buffer
 * @param   NumBytesToSend      Length of send buffer
 * @param   NumBytesSent        Length of data sent
 *
 * @returns status              QDAIO_STATUS_PENDING -> expect the result in callback
 *                              QDAIO_STATUS_SUCCESS -> data written
 *                              (or) any of the QDAIO_STATUS
 */
QCDEVLIB_API LONG QDAIO::Send(HANDLE AppHandle, PCHAR Buffer, ULONG Length, PAIO_CONTEXT Context)
{
    PAIO_ITEM ioItem;
    BOOL bResult = FALSE;
    DWORD dwStatus = NO_ERROR;
    DWORD numBytesSent = 0;
    struct iocb *ioCntrlBlockList[QDAIO_MAX_NUM_EVENTS];
    aio_context_t *pAioIdCtx;
    int ret;

    QCDEV_LOG_INFO("In -->\n");

    if ((AppHandle >= QDA_MAX_DEV) || (AppHandle < 1))
    {
        QCDEV_LOG_ERR("Invalid appHdl %ld\n", AppHandle);
        return QDAIO_STATUS_BAD_HANDLE;
    }

    if (AioDevice[AppHandle].DevExtension == NULL)
    {
        QCDEV_LOG_ERR("non-exist appHdl %ld\n", AppHandle);
        return QDAIO_STATUS_NO_DEVICE;
    }

    if (AioDevice[AppHandle].DevState != QDAIO_DEV_STATE_OPEN)
    {
        return QDAIO_STATUS_FAILURE;
    }

    ioItem = new AIO_ITEM;
    if (ioItem == NULL)
    {
        QCDEV_LOG_ERR("out of memory for appHdl %ld\n", AppHandle);
        return QDAIO_STATUS_NO_MEMORY;
    }

    // en-queue request
    ioItem->Buf = Buffer;
    ioItem->IoSize = Length;
    ioItem->UserContext = (PVOID)Context;
    ioItem->IoCallback = Context->Callback;

    memset(&ioItem->ioCntrlBlock, 0, sizeof(struct iocb));
    pAioIdCtx = &AioDevice[AppHandle].DevExtension->AioIdCtxTx;
    /*
     * queues nr I/O request blocks for processing in the AIO context ctx_id.
     * The iocbpp argument should be an array of nr AIO control blocks, which
     * will be submitted to context ctx_id.
     */
    ioItem->ioCntrlBlock.aio_fildes = AioDevice[AppHandle].DevExtension->DevHandle;
    ioItem->ioCntrlBlock.aio_lio_opcode = IOCB_CMD_PWRITE;
    ioItem->ioCntrlBlock.aio_buf = (ULONG)Buffer;
    ioItem->ioCntrlBlock.aio_offset = 0;
    ioItem->ioCntrlBlock.aio_nbytes = Length;

    ioCntrlBlockList[0] = &ioItem->ioCntrlBlock;
    ret = io_submit(*pAioIdCtx, 1, ioCntrlBlockList);
    if (ret != 1) /* Number of io reqests submitted */
    {
        QCDEV_LOG_ERR("event error %lu\n", GetLastError());
        delete ioItem;
        return QDAIO_STATUS_FAILURE;
    }

    EnterCriticalSection(&(AioDevice[AppHandle].DevLockTx));
    InsertTailList(&(AioDevice[AppHandle].DevExtension->DispatchQueueTx), &(ioItem->List));
    pthread_cond_signal(&(AioDevice[AppHandle].DevExtension->DispatchEvtTx));
    LeaveCriticalSection(&(AioDevice[AppHandle].DevLockTx));

    QCDEV_LOG_INFO("Out <--\n");
    return QDAIO_STATUS_PENDING;
}

/**
 * @brief   To Read from the selected handle
 *
 * Non Blocking I/O read
 *
 * @param   AppHandle           app handle
 * @param   RxBuffer            Receive buffer
 * @param   NumBytesToRead      Length of receieve buffer
 * @param   NumBytesReturned    Length of data Read
 *
 * @returns status              QDAIO_STATUS_PENDING -> expect the result in callback
 *                              QDAIO_STATUS_SUCCESS -> data obtained
 *                              (or) any of the QDAIO_STATUS
 */
QCDEVLIB_API LONG QDAIO::Read(HANDLE AppHandle, PCHAR Buffer, ULONG Length, PAIO_CONTEXT Context)
{
    PAIO_ITEM ioItem;
    BOOL bResult = FALSE;
    DWORD dwStatus = NO_ERROR;
    DWORD numBytesRead = 0;
    struct iocb *ioCntrlBlockList[QDAIO_MAX_NUM_EVENTS];
    aio_context_t *pAioIdCtx;
    int ret;
	
	QCDEV_LOG_DBG("In -->\n");
	
    if ((AppHandle >= QDA_MAX_DEV) || (AppHandle < 1))
    {
        QCDEV_LOG_ERR("Invalid appHdl %ld\n", AppHandle);
        return QDAIO_STATUS_BAD_HANDLE;
    }

    if (AioDevice[AppHandle].DevExtension == NULL)
    {
        QCDEV_LOG_ERR(" non-exist appHdl %ld\n", AppHandle);
        return QDAIO_STATUS_NO_DEVICE;
    }

    if (AioDevice[AppHandle].DevState != QDAIO_DEV_STATE_OPEN)
    {
        QCDEV_LOG_ERR(" non-exist appHdl %ld\n", AppHandle);
        return QDAIO_STATUS_FAILURE;
    }

    ioItem = new AIO_ITEM;
    if (ioItem == NULL)
    {
        QCDEV_LOG_ERR(" out of memory for appHdl %ld\n", AppHandle);
        return QDAIO_STATUS_NO_MEMORY;
    }

    // en-queue request
    ioItem->Buf = Buffer;
    ioItem->IoSize = Length;
    ioItem->UserContext = (PVOID)Context;
    ioItem->IoCallback = Context->Callback;

    pAioIdCtx = &AioDevice[AppHandle].DevExtension->AioIdCtxRx;
    memset(&ioItem->ioCntrlBlock, 0, sizeof(struct iocb));
    /*
     * queues nr I/O request blocks for processing in the AIO context ctx_id.
     * The iocbpp argument should be an array of nr AIO control blocks, which
     * will be submitted to context ctx_id.
     */
    ioItem->ioCntrlBlock.aio_fildes = AioDevice[AppHandle].DevExtension->DevHandle;
    ioItem->ioCntrlBlock.aio_lio_opcode = IOCB_CMD_PREAD;
    ioItem->ioCntrlBlock.aio_buf = (ULONG)Buffer;
    ioItem->ioCntrlBlock.aio_offset = 0;
    ioItem->ioCntrlBlock.aio_nbytes = Length;

    ioCntrlBlockList[0] = &ioItem->ioCntrlBlock;
    ret = io_submit(*pAioIdCtx, 1, ioCntrlBlockList);
    if (ret != 1) /* Number of io reqests submitted */
    {
        QCDEV_LOG_ERR("event error %lu\n", GetLastError());
        delete ioItem;
        return QDAIO_STATUS_FAILURE;
    }

    EnterCriticalSection(&(AioDevice[AppHandle].DevLockRx));
    InsertTailList(&(AioDevice[AppHandle].DevExtension->DispatchQueueRx), &(ioItem->List));
    pthread_cond_signal(&(AioDevice[AppHandle].DevExtension->DispatchEvtRx));
    LeaveCriticalSection(&(AioDevice[AppHandle].DevLockRx));

    QCDEV_LOG_DBG("Out <--\n");
    return QDAIO_STATUS_PENDING;
}


QCDEVLIB_API HANDLE QDAIO::GetDevHandle(HANDLE AppHandle)
{

    if ((AppHandle >= QDA_MAX_DEV) || (AppHandle < 1))
    {
        QCDEV_LOG_ERR(" Invalid appHdl %ld\n", AppHandle);
        return QDAIO_STATUS_BAD_HANDLE;
    }

    if (AioDevice[AppHandle].DevExtension == NULL)
    {
        QCDEV_LOG_ERR(" non-exist appHdl %ld\n", AppHandle);
        return QDAIO_STATUS_NO_DEVICE;
    }

    return AioDevice[AppHandle].DevExtension->DevHandle; 
}


// cancell all RX/TX if Context == NULL
/**
 * @brief   To Cancel the request (read/write)
 *
 * To cancel the req corresponding to the context
 *
 * @param   AppHandle   app handle
 * @param   Context     Context
 * 
 * @returns status      QDAIO_STATUS_SUCCESS -> success in closing app handle
 *                      (or) any of the QDAIO_STATUS
 */
QCDEVLIB_API LONG QDAIO::Cancel(HANDLE AppHandle, PAIO_CONTEXT Context)
{
    PAIO_ITEM ioItem;

    if ((AppHandle >= QDA_MAX_DEV) || (AppHandle < 1))
    {
        QCDEV_LOG_ERR(" Invalid appHdl %ld\n", AppHandle);
        return QDAIO_STATUS_BAD_HANDLE;
    }

    if (AioDevice[AppHandle].DevExtension == NULL)
    {
        QCDEV_LOG_ERR(" non-exist appHdl %ld\n", AppHandle);
        return QDAIO_STATUS_NO_DEVICE;
    }

    if (AioDevice[AppHandle].DevState != QDAIO_DEV_STATE_OPEN)
    {
        return QDAIO_STATUS_FAILURE;
    }

    if (FindAndCancel(&AioDevice[AppHandle], 0, Context) == 0)
    {
        FindAndCancel(&AioDevice[AppHandle], 1, Context);
    }

    return QDAIO_STATUS_SUCCESS; // success from CancelIoEx
}

/**
 * @brief   To open the selected device
 *
 * Opens the device and returns the device handle
 *
 * @param   QmiDeviceName   Device which is to be opened
 * @param   ServiceType     Service type
 *
 * @returns HANDLE          on success returns app handle
 */
QCDEVLIB_API HANDLE QDAIO::QCWWAN_OpenService(IN PCHAR QmiDeviceName, IN UCHAR ServiceType)
{
    int res;
    LONG appHdl;

    QCDEV_LOG_DBG("In -->\n");
    if (QmiDeviceName == NULL)
    {
        QCDEV_LOG_ERR("Invalid number of arguements\n");
        return INVALID_HANDLE_VALUE;
    }
    appHdl = QDAIO::OpenDevice(QmiDeviceName);
    if (appHdl < 0)
    {
        QCDEV_LOG_ERR("Failed to open device\n");
        return INVALID_HANDLE_VALUE;
    }

    QCDEV_LOG_DBG("Fetching IOCTL Client ID\n");

    if (ioctl(AioDevice[appHdl].DevExtension->DevHandle, IOCTL_QMI_GET_SERVICE_FILE, ServiceType) < 0)
    {
        QCDEV_LOG_ERR("ERROR: cannot get service file. ServiceType %d\n", ServiceType);
        close(AioDevice[appHdl].AppHandle);
        PurgeDevExtension(&AioDevice[appHdl]);
        return INVALID_HANDLE_VALUE;
    }
    QCDEV_LOG_DBG("QmiDeviceName %s, handle %d \n", QmiDeviceName, appHdl);
    QCDEV_LOG_DBG("Out <--\n");
    return appHdl;
}

/**
 * @brief   To purge the QMI queue
 *
 * To clear all the qmi queue related to all services
 *
 * @param   AppHandle           app handle
 * 
 * @returns on success returns TRUE else FALSE
 */
QCDEVLIB_API BOOL QDAIO::QCWWAN_PurgeQmiQueue(HANDLE AppHandle)
{
    int idx = 0;

    if ((AppHandle >= QDA_MAX_DEV) || (AppHandle < 1))
    {
        QCDEV_LOG_ERR(" Invalid appHdl %ld\n", AppHandle);
        return FALSE;
    }

    for (idx = 0; idx < QDA_MAX_DEV; idx++)
    {
        if (QDAIO_STATUS_SUCCESS != Cancel(AppHandle, NULL))
        {
            break;
        }
    }
    return TRUE;
}

/**
 * @brief   To close the selected handle
 *
 * Close the device handle
 *
 * @param   AppHandle   app handle
 *
 * @returns status      on success returns TRUE
 */
QCDEVLIB_API BOOL QDAIO::QCWWAN_CloseService(LONG AppHandle)
{
    QCDEV_LOG_DBG("In -->\n");
    return (QDAIO_STATUS_SUCCESS == QDAIO::CloseDevice(AppHandle)) ? TRUE : FALSE;
}

typedef struct _AIO_CONTEXT_LOCAL
{
    CRITICAL_SECTION mIoLock;
    pthread_cond_t mIoCond;
    PVOID mpBuffer;
    LPDWORD mpNumBytesTxn;
    BOOL mpStatus;
} AIO_CONTEXT_LOCAL, *PAIO_CONTEXT_LOCAL;

static void aioLocalCallback(LONG AppHdl, PVOID *Context, ULONG Status, LONG IoSize)
{
    QCDEV_LOG_DBG("In -->\n");

    PAIO_CONTEXT_LOCAL pLocalAioCtx;
    PAIO_CONTEXT pUserCtx;

    pUserCtx = (PAIO_CONTEXT)Context;

    pLocalAioCtx = (PAIO_CONTEXT_LOCAL)pUserCtx->UserData;

    if (!Status)
    {
        if (IoSize <= 0)
            *(pLocalAioCtx->mpNumBytesTxn) = 0;
        else
            *(pLocalAioCtx->mpNumBytesTxn) = IoSize;
        pLocalAioCtx->mpStatus = TRUE;
    }

    EnterCriticalSection(&pLocalAioCtx->mIoLock);

    pthread_cond_signal(&pLocalAioCtx->mIoCond);

    LeaveCriticalSection(&pLocalAioCtx->mIoLock);

    return;
}

/**
 * @brief   To write to the selected handle
 *
 * Sends the data on device handle 
 *
 * @param   AppHandle           app handle
 * @param   TxBuffer            Send buffer
 * @param   NumBytesToSend      Length of send buffer
 * @param   NumBytesSent        Length of data sent
 *
 * @returns on success returns TRUE else FALSE
 */
QCDEVLIB_API BOOL QDAIO::SendToDevice(HANDLE AppHandle, PVOID TxBuffer,
                                      DWORD NumBytesToSend, LPDWORD NumBytesSent)
{
    QCDEV_LOG_INFO("In -->\n");
    AIO_CONTEXT_LOCAL localAioCtx = {PTHREAD_MUTEX_INITIALIZER, PTHREAD_COND_INITIALIZER, NULL, 0, FALSE};
    struct timespec ts;
    PAIO_CONTEXT userCtx;
    LONG ret;
    LONG status;

    localAioCtx.mpBuffer = TxBuffer;
    *NumBytesSent = 0;

    localAioCtx.mpNumBytesTxn = NumBytesSent;
    *localAioCtx.mpNumBytesTxn = 0;

    userCtx = (PAIO_CONTEXT)malloc(sizeof(AIO_CONTEXT));
    userCtx->Callback = aioLocalCallback;
    userCtx->UserData = &localAioCtx;

    clock_gettime(CLOCK_REALTIME, &ts);
    ts.tv_sec += (5); //Max wait for 3 minutes

    EnterCriticalSection(&localAioCtx.mIoLock);
    status = QDAIO::Send(AppHandle, TxBuffer, NumBytesToSend, (PAIO_CONTEXT)userCtx);
    QCDEV_LOG_INFO("Send Status: %s\n",status == QDAIO_STATUS_PENDING ? "QDAIO_STATUS_PENDING" : "Failed");

    if (QDAIO_STATUS_PENDING != status)
    {
        QCDEV_LOG_ERR("Send submission failed.. errorCode: %d Number of Bytes to Send :%lu\nEnd of SendToDevice\n", status, NumBytesToSend);
        LeaveCriticalSection(&localAioCtx.mIoLock);
        free(userCtx);
        return false;
    }

    ret = pthread_cond_timedwait(&localAioCtx.mIoCond, &localAioCtx.mIoLock, &ts);

    if (ret == 0)
    {
        QCDEV_LOG_INFO("Event Triggered\n");
    }
    else
    {
        if (ret == ETIMEDOUT)
        {
            clock_gettime(CLOCK_REALTIME, &ts);
            ts.tv_sec += (5); //Max wait for 3 minutes
            QDAIO::Cancel(AppHandle, (PAIO_CONTEXT)userCtx);
            QCDEV_LOG_ERR("No Event Triggered Timedout\n");
            pthread_cond_timedwait(&localAioCtx.mIoCond, &localAioCtx.mIoLock, &ts); //Will be triggered even in case of cancel
        }
    }

    LeaveCriticalSection(&localAioCtx.mIoLock);
    free(userCtx);
    QCDEV_LOG_INFO("Out <--\n");
    return localAioCtx.mpStatus;
}

/**
 * @brief   To Read from the selected handle
 *
 * Blocking I/O read
 *
 * @param   AppHandle           app handle
 * @param   RxBuffer            Receive buffer
 * @param   NumBytesToRead      Length of receieve buffer
 * @param   NumBytesReturned    Length of data Read
 *
 * @returns on success returns TRUE else FALSE
 */
QCDEVLIB_API BOOL QDAIO::ReadFromDevice(HANDLE AppHandle, PVOID RxBuffer,
                                        DWORD NumBytesToRead, LPDWORD NumBytesReturned)
{
    QCDEV_LOG_DBG("In -->\n");

    AIO_CONTEXT_LOCAL localAioCtx = {PTHREAD_MUTEX_INITIALIZER, PTHREAD_COND_INITIALIZER, NULL, 0, FALSE};
    struct timespec ts;
    PAIO_CONTEXT userCtx;
    LONG ret;
    LONG status;

    localAioCtx.mpBuffer = RxBuffer;
    localAioCtx.mpNumBytesTxn = NumBytesReturned;

    userCtx = (PAIO_CONTEXT)malloc(sizeof(AIO_CONTEXT));
    userCtx->Callback = aioLocalCallback;
    userCtx->UserData = &localAioCtx;

    clock_gettime(CLOCK_REALTIME, &ts);
    ts.tv_sec += (45); //increased timeout

    EnterCriticalSection(&localAioCtx.mIoLock);
    status = QDAIO::Read(AppHandle, RxBuffer, NumBytesToRead, (PAIO_CONTEXT)userCtx);
    QCDEV_LOG_INFO("Read Status: %s\n",status == QDAIO_STATUS_PENDING ? "QDAIO_STATUS_PENDING" : "Failed");

    if (QDAIO_STATUS_PENDING != status)
    {
        QCDEV_LOG_ERR("Read submission failed.. errorCode: %d Number of Bytes to Read :%lu\nEnd ReadFromDevice ......\n",status, NumBytesToRead);
        LeaveCriticalSection(&localAioCtx.mIoLock);
        free(userCtx);
        return false;
    }

    ret = pthread_cond_timedwait(&localAioCtx.mIoCond, &localAioCtx.mIoLock, &ts);

    if (ret == 0)
    {
        QCDEV_LOG_INFO("Event Triggered\n");
    }
    else
    {
        QCDEV_LOG_ERR("Event not triggeted  %d \n");
        if (ret == ETIMEDOUT)
        {
            clock_gettime(CLOCK_REALTIME, &ts);
            ts.tv_sec += (45); //increased timeout
            QDAIO::Cancel(AppHandle, (PAIO_CONTEXT)userCtx);
            QCDEV_LOG_ERR("No Event Triggered Timedout\n");
            pthread_cond_timedwait(&localAioCtx.mIoCond, &localAioCtx.mIoLock, &ts); //Will be triggered even in case of cancel
        }
    }

    LeaveCriticalSection(&localAioCtx.mIoLock);
    free(userCtx);
    QCDEV_LOG_INFO("Out <--\n");
    return localAioCtx.mpStatus;
}
