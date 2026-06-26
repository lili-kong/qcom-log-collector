// Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
// SPDX-License-Identifier: BSD 3-Clause Clear License
#ifndef QDAIO_H
#define QDAIO_H

#include <stdlib.h>
#include <stdio.h>
//#include <windows.h>
//#include <conio.h>
#include "windows_compatible.h"     /**< Local header to have compatibility */
#include <string.h>
//#include <winioctl.h>
#include "../../common/utils.h"
#include <linux/aio_abi.h>          /**< For Aio context in Linux */

#define QDAIO_LNX
#define QDA_MAX_DEV 64              /**< Max number of dev connectons*/
#define QDAIO_MAX_NUM_EVENTS    2048  /**< Sealed max number of events */
#define QCDEV_MIN_REF_CNT 2

#define BUS_LOC "/dev/bus/usb/"

/* Align structures to 8 bytes packing. */
#pragma pack(push, 8)

/**
 * @brief       Application handle states
 */
typedef enum _QDAIO_DEV_STATE
{
    QDAIO_DEV_STATE_UNINIT = 0,     /**< App handle un-initialized  */
    QDAIO_DEV_STATE_INIT   = 1,     /**< App handle initialized     */
    QDAIO_DEV_STATE_OPEN   = 2,     /**< App handle open            */
    QDAIO_DEV_STATE_CLOSE  = 3      /**< App handle closed          */
} QDAIO_DEV_STATE;

/**
 * @brief       QDAIO library error codes
 */
typedef enum _QDAIO_STATUS
{
    QDAIO_STATUS_SUCCESS            = 0,    /**< Success                    */
    QDAIO_STATUS_FAILURE            = -1,    /**< Failed to submit request   */
    QDAIO_STATUS_INVALID_DEVICE     = -2,    /**< Invalid device             */
    QDAIO_STATUS_DEVICE_NOT_INIT    = -3,    /**< Device Deinitialized       */
    QDAIO_STATUS_NO_DEVICE          = -4,    /**< Device context is empty    */
    QDAIO_STATUS_HANDLE_FAILURE     = -5,    /**< Failed to open device      */
    QDAIO_STATUS_BAD_HANDLE         = -6,    /**< Invalid app handle         */
    QDAIO_STATUS_NO_MEMORY          = -7,    /**< Failed to allocate memory  */
    QDAIO_STATUS_PENDING            = -8,    /**< I/O req in pending state   */
    QDAIO_STATUS_BAD_EVENT          = -9,    /**< Invalid event              */
} QDAIO_STATUS;

/**
 * @brief       QDAIO app handle context
 */
typedef struct _QDAIODEV_EXTENTION
{
    HANDLE     DevHandle;           /**< Handle to device   */
    LIST_ENTRY DispatchQueueRx;     /**< Rx dispatch queue  */
    LIST_ENTRY DispatchQueueTx;     /**< Tx dispatch queue  */
    LIST_ENTRY PendingQueueRx;      /**< Rx pending queue   */
    LIST_ENTRY PendingQueueTx;      /**< Tx pending queue   */
    LIST_ENTRY CancelQueueRx;       /**< Rx cancel queue    */
    LIST_ENTRY CancelQueueTx;       /**< Tx cancel queue    */
    HANDLE     DispatchThreadRx;    /**< Rx thread handle   */
    HANDLE     DispatchThreadTx;    /**< Tx thread handle   */
    HANDLE     CancelThread;        /**< Cancel thread ctx  */
    aio_context_t AioIdCtxRx;       /**< Rx aio context     */
    aio_context_t AioIdCtxTx;       /**< Tx aio context     */
    pthread_cond_t DispatchEvtRx;   /**< Rx dispatch event  */
    pthread_cond_t DispatchEvtTx;   /**< Tx dispatch event  */

} QDAIODEV_EXTENSION, *PQDAIODEV_EXTENSION;

/**
 * @brief       QDAIO device context
 */
typedef struct _QDAIODEV
{
    LONG    AppHandle;                  /**< Handle to device   */
    LONG    Ref;                        /**< Ref count to thread*/
    PQDAIODEV_EXTENSION DevExtension;   /**< App handle ctx     */
    CRITICAL_SECTION    DevLockRx;      /**< Rx txn lock        */
    CRITICAL_SECTION    DevLockTx;      /**< Tx txn lock        */
    QDAIO_DEV_STATE     DevState;       /**< Device status      */
    pthread_cond_t      CancelEvt;      /**< cancel event       */
} QDAIODEV, *PQDAIODEV;

/**
 * @brief       QDAIO thread context
 */
typedef struct _QDAIO_THREAD_CONTEXT
{
    PQDAIODEV Dev;                  /**< QDAIO device context   */
    int       IoType;               /**< RX -- 0; TX -- 1       */
} QDAIO_THREAD_CONTEXT, *PQDAIO_THREAD_CONTEXT;

/**
 * @brief       Callback to the Tx/Rx transaction
 */
typedef VOID (_stdcall *AIO_CALLBACK)(LONG AppHdl, PVOID *Context, ULONG Status, LONG IoSize);

/**
 * @brief       Callback context for Async Rx/Tx
 */
typedef struct _AIO_CONTEXT
{
    AIO_CALLBACK Callback;      /**< Callback to be triggered   */
    PVOID        UserData;      /**< Userdata                   */
} AIO_CONTEXT, *PAIO_CONTEXT;

/**
 * @brief       Internal context for Async Rx/Tx
 */
typedef struct _AIO_ITEM
{
    LIST_ENTRY List;            /**< List to Rx/Tx contexts */
    PVOID      Buf;             /**< User buffer            */
    ULONG      IoSize;          /**< User specified size    */
    //OVERLAPPED OverlappedContext;
    AIO_CALLBACK IoCallback;    /**< Callback to be trigged */
    PVOID      UserContext;     /**< Context to the callback*/
    struct iocb ioCntrlBlock;   /**< AIO control block      */
} AIO_ITEM, *PAIO_ITEM;

namespace QDAIO
{
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
    QCDEVLIB_API HANDLE QCWWAN_OpenService(IN PCHAR QmiDeviceName, IN UCHAR ServiceType);

    /**
     * @brief   To close the selected handle
     *
     * Close the device handle
     *
     * @param   AppHandle   app handle
     *
     * @returns status      on success returns TRUE
     */
    QCDEVLIB_API BOOL QCWWAN_CloseService(LONG AppHandle);

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
    QCDEVLIB_API LONG Read(HANDLE AppHandle, PCHAR Buffer, ULONG Length, PAIO_CONTEXT Context);

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
    QCDEVLIB_API LONG Send(HANDLE AppHandle, PCHAR Buffer, ULONG Length, PAIO_CONTEXT Context);

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
    QCDEVLIB_API LONG CloseDevice(LONG AppHandle);

    /**
     * @brief   To open the selected device
     *
     * Opens the device and returns the device handle
     *
     * @param   DeviceName  Device which is to be opened
     * 
     * @returns appHandle   on success returns app handle
     */ 
    QCDEVLIB_API LONG OpenDevice(PVOID DeviceName);

    QCDEVLIB_API LONG GetDevHandle(LONG AppHandle);
    /**
     * @brief   To initialize the QDAIO for async support
     *
     * To Initialize QDAIO related data
     *
     * @returns Nothing
     */ 
    QCDEVLIB_API VOID Initialize(void);

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
    QCDEVLIB_API LONG Cancel(HANDLE AppHandle, PAIO_CONTEXT Context);

    /**
     * @brief   To purge the QMI queue
     *
     * To clear all the qmi queue related to all services
     *
     * @param   AppHandle           app handle
     * 
     * @returns on success returns TRUE else FALSE
     */ 
    QCDEVLIB_API BOOL QCWWAN_PurgeQmiQueue(HANDLE AppHandle);

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
    QCDEVLIB_API BOOL ReadFromDevice(HANDLE AppHandle, PVOID  RxBuffer,
            DWORD  NumBytesToRead, LPDWORD NumBytesReturned);

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
    QCDEVLIB_API BOOL SendToDevice(HANDLE AppHandle, PVOID  TxBuffer,
            DWORD  NumBytesToSend, LPDWORD NumBytesSent);

}  // QDAIO

#pragma pack(pop)

#endif
