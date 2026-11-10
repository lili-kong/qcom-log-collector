// Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
// SPDX-License-Identifier: BSD 3-Clause Clear License
/*
* Copyright (c) 2022 Qualcomm Technologies, Inc.  All Rights Reserved.

 * Qualcomm Technologies Proprietary and Confidential.

 *

 * Not a Contribution.
 */
 
 /*
 * Copyright (c) 2017-2019 Cisco and/or its affiliates.
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at:
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "qdpublic.h"
#include <libudev.h>
#include "qdutils.h"
#include <pthread.h>
#include <memory>
#include <sys/socket.h>
#include <signal.h>
#include <time.h>
#include "qdaio.h"
#include <linux/rtnetlink.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <arpa/inet.h>
#include "../../libusb/source/qcom-libusb.h"
#include "../../EUD/QdsEud.h"
#ifdef ALPACA
#include "AlpacaDeviceDiscovery.h"
#endif

using namespace Utils;

#define ALLOW_TTY_DEVICE
#define MAX_BUFFER_SIZE 8192
#ifdef ENABLELOCALLOGGING
#include <bits/stdc++.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <chrono>
#include <sys/inotify.h>

#include <iostream>
using namespace std;

#define MAX_WAIT_TIME_IN_SECONDS (6)

/**
 * Macros to be used in libqcdev_read_loglvl
 */
#ifdef QCDEV_LOGGING
#define PATH "/opt/QTI/QDS/qcom/qcDeviceDiscovery/"
#define DEBUG_F "/opt/QTI/QDS/qcom/qcDeviceDiscovery/debug"
#define MAX_EVENTS	1024										/*Max. number of events to process at one go*/
#define LEN_NAME	16											/*Assuming that the length of the filename won't exceed 16 bytes*/
#define EVENT_SIZE  ( sizeof (struct inotify_event) )			/*size of one event*/
#define BUF_LEN     ( MAX_EVENTS * ( EVENT_SIZE + LEN_NAME ))	/*buffer to store the data of events*/
pthread_cond_t cond1 = PTHREAD_COND_INITIALIZER;// declaring mutex
pthread_mutex_t lock1 = PTHREAD_MUTEX_INITIALIZER;
#endif
FILE *fp = NULL;
#endif
QcdevCtx qcdevCtx = {NULL, NULL,
                     NULL, NULL, NULL, NULL, NULL, PTHREAD_MUTEX_INITIALIZER, -1, 0, 0, 0, NULL};
int log_level = 0;

static bool      g_libusb_event_running = false;
static pthread_t g_libusb_event_thread;

/* -----------------------------------------------------------------------
 * EUD pending-arrival list
 * EUD devices that arrived before their parent USB composite device.
 * Protected by eudPendingLock_.
 * ----------------------------------------------------------------------- */
struct EudPendingItem {
    EUD_DEVICE_INFO info;
    EudPendingItem  *next;
    EudPendingItem() {
        next = NULL;
    }
};
static EudPendingItem    *g_eudPendingHead = NULL;
static pthread_mutex_t    g_eudPendingLock = PTHREAD_MUTEX_INITIALIZER;

/* Forward declarations */
static void ProcessPendingEudArrivals(void);
static void pushToDeviceCtx(PDeviceCtx deviceCtx);
static void popFromDeviceCtx(PVOID devContextInt);
static void *fetchFromDeviceCtx(PVOID devNode);

/*
 * @brief Check whether the EUD shared library is present on disk.
 *
 * Used access() to check file-existence without loading the library
 *
 * When EUD is installed, QDS must NOT call load_libusb() / initialize_libusb()
 * because both EUD and qcom-libusb dynamically load the same system libusb.so.
 * A double libusb_init / libusb_exit on the shared instance corrupts the
 * internal reference count and causes crashes or silent failures.
 *
 * @returns true  if libeud.so exists (EUD will load libusb).
 * @returns false if libeud.so is absent (qcom-libusb will load libusb).
 */
static bool IsEudLibPresent()
{
    const char *eudPath = GetEudLibPath();
    if (eudPath == nullptr)
        return false;
    return (access(eudPath, F_OK) == 0);
}

/* -----------------------------------------------------------------------
 * EudDeviceChangeCb
 *   Invoked by EUD.so's monitor thread on peripheral arrival/departure.
 * ----------------------------------------------------------------------- */
static void EudDeviceChangeCb(const EUD_DEVICE_INFO *info)
{
    if (!info) return;

    if (strnlen(info->device_name, EUD_MAX_DEVICE_NAME) >= EUD_MAX_DEVICE_NAME)
    {
        QCDEV_LOG_ERR("EudDeviceChangeCb: device_name not null-terminated or exceeds max length\n");
        return;
    }

    if (info->flag == 1)
    {
        /* ---- ARRIVAL ---- */
        /* Compute correct SerNumMsm */
        char corrSerNumMsm[9];
        ConvertDeviceIdToSerNumMsm(info->device_id, corrSerNumMsm, sizeof(corrSerNumMsm));

        /* Check if parent USB device is already in qcdevCtx */
        bool parentPresent = false;
        pthread_mutex_lock(&qcdevCtx.mListLock);
        PDeviceCtx t = qcdevCtx.mpDeviceCtx;
        while (t)
        {
            if (t->mDevParams.SerNumMsm[0] != '\0' &&
                strcasecmp(t->mDevParams.SerNumMsm, corrSerNumMsm) == 0)
            {
                parentPresent = true;
                break;
            }
            t = t->mNext;
        }
        pthread_mutex_unlock(&qcdevCtx.mListLock);

        if (parentPresent && qcdevCtx.mCallback)
        {
            /* Fire immediately */
            CB_PARAMS cbp;
            cbp.DevDesc  = (PCHAR)info->device_name;
            cbp.DevName  = (PCHAR)info->device_name;
            cbp.SerNumMsm = new char[strlen(corrSerNumMsm) + 1];
            strcpy(cbp.SerNumMsm, corrSerNumMsm);
            cbp.Flag = ((ULONG)QC_DEV_TYPE_EUD << 8) |
                       ((ULONG)QC_DEV_STATE_ARRIVAL << 4) | (ULONG)1;
            cbp.DevDetails = std::make_unique<DevDetailsMap>();
            PVOID ctx = qcdevCtx.mCallbackCtx;
            QCDEV_LOG_INFO("EUD ARRIVAL (immediate): %s SerNumMsm=%s\n",
                           info->device_name, corrSerNumMsm);
            qcdevCtx.mCallback(&cbp, &ctx);
            delete [] cbp.SerNumMsm;
            cbp.SerNumMsm = NULL;

            /* Add EUD device to the device list so GetDeviceList can report it */
            PDeviceCtx pEudCtx = new _DeviceCtx();
            if (pEudCtx)
            {
                strncpy(pEudCtx->mDevParams.DevDesc, info->device_name, QCDEV_MAX_VALUE_NAME - 1);
                pEudCtx->mDevParams.DevDesc[QCDEV_MAX_VALUE_NAME - 1] = '\0';
                pEudCtx->mDevParams.DevDescBufLen = strlen(pEudCtx->mDevParams.DevDesc);
                strncpy(pEudCtx->mDevParams.DevName, info->device_name, QCDEV_MAX_VALUE_NAME - 1);
                pEudCtx->mDevParams.DevName[QCDEV_MAX_VALUE_NAME - 1] = '\0';
                pEudCtx->mDevParams.DevnameBufLen = strlen(pEudCtx->mDevParams.DevName);
                strncpy(pEudCtx->mDevParams.SerNumMsm, corrSerNumMsm, QCDEV_MAX_VALUE_NAME - 1);
                pEudCtx->mDevParams.SerNumMsm[QCDEV_MAX_VALUE_NAME - 1] = '\0';
                pEudCtx->mDevParams.SerNumMsmBufLen = strlen(pEudCtx->mDevParams.SerNumMsm);
                pEudCtx->mDevParams.Flag = ((ULONG)QC_DEV_TYPE_EUD << 8) |
                                           ((ULONG)QC_DEV_STATE_ARRIVAL << 4) | (ULONG)1;
                pEudCtx->mDevParams.DevDetails =
                                            std::make_unique<std::unordered_map<std::string, std::string>>();
                strncpy(pEudCtx->mDevNode, info->device_name, QCDEV_MAX_VALUE_NAME - 1);
                pEudCtx->mDevNode[QCDEV_MAX_VALUE_NAME - 1] = '\0';
                pEudCtx->mContextInt = (PVOID)(intptr_t)(++qcdevCtx.mDevidx);
                pushToDeviceCtx(pEudCtx);
            }
        }
        else
        {
            /* Queue as pending */
            EudPendingItem *item = new EudPendingItem();
            if (item)
            {
                item->info = *info;
                /* Store corrected SerNumMsm in serial_id for later use */
                strncpy(item->info.serial_id, corrSerNumMsm, EUD_MAX_SERIAL_ID - 1);
                item->info.serial_id[EUD_MAX_SERIAL_ID - 1] = '\0';
                pthread_mutex_lock(&g_eudPendingLock);
                item->next = g_eudPendingHead;
                g_eudPendingHead = item;
                pthread_mutex_unlock(&g_eudPendingLock);
                QCDEV_LOG_INFO("EUD ARRIVAL deferred (waiting for USB parent): %s SerNumMsm=%s\n",
                               info->device_name, corrSerNumMsm);
            }
        }
    }
    else
    {
        /* ---- DEPARTURE ---- */
        if (qcdevCtx.mCallback)
        {
            char corrSerNumMsm[9];
            ConvertDeviceIdToSerNumMsm(info->device_id, corrSerNumMsm, sizeof(corrSerNumMsm));

            CB_PARAMS cbp;
            cbp.DevDesc   = (PCHAR)info->device_name;
            cbp.DevName   = (PCHAR)info->device_name;
            cbp.SerNumMsm = new char[strlen(corrSerNumMsm) + 1];
            strcpy(cbp.SerNumMsm, corrSerNumMsm);
            cbp.Flag = ((ULONG)QC_DEV_TYPE_EUD << 8) |
                       ((ULONG)QC_DEV_STATE_DEPARTURE << 4) | (ULONG)1;
            cbp.DevDetails = std::make_unique<DevDetailsMap>();
            PVOID ctx = qcdevCtx.mCallbackCtx;
            QCDEV_LOG_INFO("EUD DEPARTURE: %s SerNumMsm=%s\n",
                           info->device_name, corrSerNumMsm);
            qcdevCtx.mCallback(&cbp, &ctx);
            delete [] cbp.SerNumMsm;
            cbp.SerNumMsm = NULL;
        }

        /* Remove from pending list if it was never promoted */
        pthread_mutex_lock(&g_eudPendingLock);
        EudPendingItem *prev = NULL, *cur = g_eudPendingHead;
        while (cur)
        {
            if (strcmp(cur->info.device_name, info->device_name) == 0)
            {
                if (prev) prev->next = cur->next;
                else      g_eudPendingHead = cur->next;
                delete cur;
                break;
            }
            prev = cur;
            cur  = cur->next;
        }
        pthread_mutex_unlock(&g_eudPendingLock);

        /* Reset the cached device ID when the CTL peripheral departs.
         * CTL represents the EUD device itself; when it departs the
         * device has disconnected and the next initializeEudTrace()
         * must re-query get_device_id_array for the new device ID. */
        if (info->peripheral_type == EUD_PERIPH_TYPE_CTL)
        {
            QcEud_ResetDeviceId();
        }

        /* Remove from handle map if the device was opened */
        QcEud_RemoveByName((PVOID)info->device_name);

        /* Remove EUD device from the device list */
        PDeviceCtx pEudCtx = (PDeviceCtx)fetchFromDeviceCtx((PVOID)info->device_name);
        if (pEudCtx)
        {
            PVOID ctxInt = pEudCtx->mContextInt;
            popFromDeviceCtx(ctxInt);
        }
    }
}

/* -----------------------------------------------------------------------
 * ProcessPendingEudArrivals
 *   Called after each device scan.  Promotes pending EUD items whose
 *   parent USB device has now appeared in qcdevCtx.mpDeviceCtx.
 * ----------------------------------------------------------------------- */
static void ProcessPendingEudArrivals(void)
{
    pthread_mutex_lock(&qcdevCtx.mListLock);
    pthread_mutex_lock(&g_eudPendingLock);
    EudPendingItem *prev = NULL, *cur = g_eudPendingHead;
    while (cur)
    {
        const char *corrSerNumMsm = cur->info.serial_id; /* stored at arrival */
        bool parentFound = false;

        PDeviceCtx t = qcdevCtx.mpDeviceCtx;
        while (t)
        {
            if (t->mDevParams.SerNumMsm[0] != '\0' &&
                strcasecmp(t->mDevParams.SerNumMsm, corrSerNumMsm) == 0)
            {
                parentFound = true;
                break;
            }
            t = t->mNext;
        }

        if (parentFound)
        {
            /* Promote: fire callback */
            if (qcdevCtx.mCallback)
            {
                CB_PARAMS cbp;
                cbp.DevDesc   = (PCHAR)cur->info.device_name;
                cbp.DevName   = (PCHAR)cur->info.device_name;
                cbp.SerNumMsm = new char[strlen(corrSerNumMsm) + 1];
                strcpy(cbp.SerNumMsm, corrSerNumMsm);
                cbp.Flag = ((ULONG)QC_DEV_TYPE_EUD << 8) |
                           ((ULONG)QC_DEV_STATE_ARRIVAL << 4) | (ULONG)1;
                cbp.DevDetails = std::make_unique<DevDetailsMap>();
                PVOID ctx = qcdevCtx.mCallbackCtx;
                QCDEV_LOG_INFO("EUD ARRIVAL (deferred, parent now present): %s SerNumMsm=%s\n",
                               cur->info.device_name, corrSerNumMsm);
                qcdevCtx.mCallback(&cbp, &ctx);

                delete [] cbp.SerNumMsm;
                cbp.SerNumMsm = NULL;
            }

            /* Add EUD device to the device list (lock already held, inline push) */
            PDeviceCtx pEudCtx = new _DeviceCtx();
            if (pEudCtx)
            {
                strncpy(pEudCtx->mDevParams.DevDesc, cur->info.device_name, QCDEV_MAX_VALUE_NAME - 1);
                pEudCtx->mDevParams.DevDesc[QCDEV_MAX_VALUE_NAME - 1] = '\0';
                pEudCtx->mDevParams.DevDescBufLen = strlen(pEudCtx->mDevParams.DevDesc);
                strncpy(pEudCtx->mDevParams.DevName, cur->info.device_name, QCDEV_MAX_VALUE_NAME - 1);
                pEudCtx->mDevParams.DevName[QCDEV_MAX_VALUE_NAME - 1] = '\0';
                pEudCtx->mDevParams.DevnameBufLen = strlen(pEudCtx->mDevParams.DevName);
                strncpy(pEudCtx->mDevParams.SerNumMsm, corrSerNumMsm, QCDEV_MAX_VALUE_NAME - 1);
                pEudCtx->mDevParams.SerNumMsm[QCDEV_MAX_VALUE_NAME - 1] = '\0';
                pEudCtx->mDevParams.SerNumMsmBufLen = strlen(pEudCtx->mDevParams.SerNumMsm);
                pEudCtx->mDevParams.Flag = ((ULONG)QC_DEV_TYPE_EUD << 8) |
                                           ((ULONG)QC_DEV_STATE_ARRIVAL << 4) | (ULONG)1;
                pEudCtx->mDevParams.DevDetails =
                                            std::make_unique<std::unordered_map<std::string, std::string>>();
                strncpy(pEudCtx->mDevNode, cur->info.device_name, QCDEV_MAX_VALUE_NAME - 1);
                pEudCtx->mDevNode[QCDEV_MAX_VALUE_NAME - 1] = '\0';
                pEudCtx->mContextInt = (PVOID)(intptr_t)(++qcdevCtx.mDevidx);
                /* Inline push since qcdevCtx.mListLock is already held */
                if (NULL == qcdevCtx.mpDeviceCtx)
                    qcdevCtx.mpDeviceCtx = pEudCtx;
                else
                {
                    pEudCtx->mNext = (PDeviceCtx)qcdevCtx.mpDeviceCtx;
                    qcdevCtx.mpDeviceCtx = pEudCtx;
                }
            }

            /* Remove from pending list */
            EudPendingItem *toFree = cur;
            if (prev) prev->next = cur->next;
            else      g_eudPendingHead = cur->next;
            cur = cur->next;
            delete toFree;
        }
        else
        {
            prev = cur;
            cur  = cur->next;
        }
    }
    pthread_mutex_unlock(&g_eudPendingLock);
    pthread_mutex_unlock(&qcdevCtx.mListLock);
}
/**
 * @brief   Map a raw bInterfaceProtocol byte to its DEV_CLASS_* owner.
 *
 * Adding a new protocol family: add a case here and add the corresponding
 * DEV_CLASS_* bit in qdpublic.h
 */
static ULONG ProtocolToDeviceClass(ULONG ifaceProtocol)
{
    switch (ifaceProtocol & DEV_PROTOCOL_MAJOR_REVISION_MASK)
    {
        case 0x10: return DEV_CLASS_SAHARA;  /* 0x10-0x1F Sahara family + Firehose 0x20 */
        case 0x20: return DEV_CLASS_SAHARA;  /* DEV_PROTOCOL_FIREHOSE */
        case 0x30: return DEV_CLASS_PORTS;   /* DEV_PROTOCOL_DIAG     */
        case 0x40: return DEV_CLASS_PORTS;   /* DEV_PROTOCOL_DUN      */
        case 0x50: return DEV_CLASS_NET;     /* DEV_PROTOCOL_RMNET    */
        case 0x60: return DEV_CLASS_PORTS;   /* DEV_PROTOCOL_NMEA     */
        case 0x70: return DEV_CLASS_USB;     /* DEV_PROTOCOL_QDSS     */
        case 0x80: return DEV_CLASS_USB;     /* DEV_PROTOCOL_ADPL     */
        default:   return 0;
    }
}

/**
 * @brief   Map a raw bInterfaceProtocol byte to its DEV_PROTOCOL_MASK_* bit.
 *
 * Used for sub-protocol narrowing within DEV_CLASS_SAHARA when
 * mSetting->DeviceProtocol != 0.
 */
static ULONG ProtocolToMask(ULONG ifaceProtocol)
{
    switch (ifaceProtocol)
    {
        case DEV_PROTOCOL_SAHARA:                          return DEV_PROTOCOL_MASK_SAHARA;
        case DEV_PROTOCOL_SAHARA_PBL_EMERGENCY_DOWNLOAD:  return DEV_PROTOCOL_MASK_EDL;
        case DEV_PROTOCOL_SAHARA_PBL_FLASHLESS_BOOT:      return DEV_PROTOCOL_MASK_FLASHLESS_BOOT;
        case DEV_PROTOCOL_SAHARA_SBL_XBL_RAMDUMP:         return DEV_PROTOCOL_MASK_RAMDUMP;
        case DEV_PROTOCOL_SAHARA_TN_APPS_Remote_EFS:      return DEV_PROTOCOL_MASK_EFS;
        case DEV_PROTOCOL_FIREHOSE:                       return DEV_PROTOCOL_MASK_FIREHOSE;
        default:                                           return 0;
    }
}

/**
 * @brief   Common protocol-level gate for both kernel and userspace paths.
 *
 * Rules:
 *   DeviceClass == 0            → accept all (no filter active)
 *   protocol == 0               → composite device node, always accept
 *   protocol not in class map   → reject (unknown protocol)
 *   protocol class not in
 *     mSetting->DeviceClass     → reject (class not subscribed)
 *   DeviceClass has SAHARA and
 *   DeviceProtocol != 0         → accept only if protocol mask bit is set
 *
 * To add a new DEV_CLASS_*: add a row to ProtocolToDeviceClass()
 *
 * @param   protocol  Raw 32-bit protocol field (bits[7:0] = bInterfaceProtocol).
 * @returns true  — device passes filter, continue processing.
 *          false — device rejected, discard without pushToDeviceCtx.
 */
static bool IsDeviceAllowed(ULONG protocol)
{
    if (!qcdevCtx.mSetting || qcdevCtx.mSetting->DeviceClass == 0)
        return true;

    /* Composite device node (protocol == 0) — always allow */
    if (protocol == 0)
        return true;

    ULONG ifaceProtocol = protocol & 0xFF;
    ULONG devClass = ProtocolToDeviceClass(ifaceProtocol);

    /* Unknown protocol — reject */
    if (devClass == 0)
        return false;

    /* Protocol's class is not in the subscribed DeviceClass — reject */
    if (!(qcdevCtx.mSetting->DeviceClass & devClass))
        return false;

    /* Sub-protocol narrowing: only meaningful for DEV_CLASS_SAHARA.
     * DeviceProtocol == 0 means accept all protocols in the class. */
    if ((devClass == DEV_CLASS_SAHARA) && (qcdevCtx.mSetting->DeviceProtocol != 0))
    {
        ULONG mask = ProtocolToMask(ifaceProtocol);
        if (!(qcdevCtx.mSetting->DeviceProtocol & mask))
            return false;
    }

    return true;
}

/**
 * @brief   To push the Device details to the Main Ctx
 *
 * The device info deviceCtx is pushed into the main Ctx
 *
 * @param   deviceCtx   Device Cntx (device info)
 *
 * @returns Nothing
 */
static void *libusb_event_thread_fn(void *arg)
{
    struct libusb_context *ctx = (struct libusb_context *)arg;
    struct timeval tv;
    tv.tv_sec  = 1;
    tv.tv_usec = 0;
    while (g_libusb_event_running) {
        int completed = 0;
        fn_libusb_handle_events_timeout_completed(ctx, &tv, &completed);
    }
    return nullptr;
}

static void pushToDeviceCtx(PDeviceCtx deviceCtx)
{
    QCDEV_LOG_DBG("In\n");

    if (NULL == deviceCtx)
    {
        QCDEV_LOG_ERR("Invalid data\n");
        return;
    }

    /* Lock a mutex and unlock it on return. */
    QCDEV_LOCK_MUTEX_AND_UNLOCK_ON_RETURN(&qcdevCtx.mListLock);
    /* Push to the device context */
    if (NULL == qcdevCtx.mpDeviceCtx)
    {
        qcdevCtx.mpDeviceCtx = deviceCtx;
    }
    else
    {
        deviceCtx->mNext = (PDeviceCtx)qcdevCtx.mpDeviceCtx;
        qcdevCtx.mpDeviceCtx = deviceCtx;
    }
    QCDEV_LOG_DBG("Out\n");
    return;
}

#ifdef ALPACA
int AlpacaDiscoveryCallback(DEVICECHANGE_CALLBACK Cb, PVOID AppContext)
{
    // set the callback method
    setAlpacaDiscoveryCallback(Cb, AppContext);

    return 0;
}
#endif
// little helper to parsing message using netlink macroses
void parseRtattr(struct rtattr *tb[], int max, struct rtattr *rta, int len)
{
    memset(tb, 0, sizeof(struct rtattr *) * (max + 1));

    while (RTA_OK(rta, len)) {  // while not end of the message
        if (rta->rta_type <= max) {
            tb[rta->rta_type] = rta; // read attr
        }
        rta = RTA_NEXT(rta,len);    // get next attr
    }
}

static void fetchIPAddr(char *ipAddr)
{
    FILE *fpipe;
    char *command = "/sbin/ifconfig -a mhi_swip0 | grep -w 'inet' | cut -d \" \" -f 10";
    char c = 0;
    char ip_addr[QCDEV_MAX_VALUE_NAME];
    int id = 0;

    if (0 == (fpipe = (FILE*)popen(command, "r")))
    {
    	perror("popen() failed.\n");
        return;
    }

    while (fread(&c, sizeof(c), 1, fpipe) && id < QCDEV_MAX_VALUE_NAME - 1)
    {
		if (c != '\n') {
			ip_addr[id++] = c;
            		if (id-1 == 12)
                		ip_addr[id-1] = (((c - '0') + 1) + '0');
		} else {
			ip_addr[id++] = '\0';
			break;
		}
    }
    ip_addr[id] = '\0';

   strncpy(ipAddr, ip_addr, QCDEV_MAX_VALUE_NAME - 1);
   ipAddr[QCDEV_MAX_VALUE_NAME - 1] = '\0';

    pclose(fpipe);
}

static void getHwID(struct udev_device *dev, PDeviceCtx pdeviceCtx, const char *cid)
{
	struct udev_device *devP;
	const char *vendor = NULL;
	const char *product = NULL;
	const char *hwid = NULL;

	devP = udev_device_get_parent(dev);

	if (strcasecmp("pci",udev_device_get_subsystem(dev)) == 0)
	{
	    vendor = udev_device_get_sysattr_value(dev, "subsystem_vendor");
	    product = udev_device_get_sysattr_value(dev, "subsystem_device");
	    vendor += 2;
	    product += 2;
	}
	else if (strcasecmp("usb",udev_device_get_subsystem(dev)) == 0)
	{
		vendor = udev_device_get_sysattr_value(dev, "idVendor");
		product = udev_device_get_sysattr_value(dev, "idProduct");
	}

    if (!vendor)
    {
    	if (strcasecmp("pci",udev_device_get_subsystem(dev)) == 0)
    		vendor = udev_device_get_sysattr_value(devP, "subsystem_vendor");
    	else if (strcasecmp("usb",udev_device_get_subsystem(dev)) == 0)
    		vendor = udev_device_get_sysattr_value(devP, "idVendor");
        if (!vendor)
            vendor = "0000";
    }

    if (!product)
    {
    	if (strcasecmp("pci",udev_device_get_subsystem(dev)) == 0)
    		product = udev_device_get_sysattr_value(devP, "subsystem_device");
    	else if (strcasecmp("usb",udev_device_get_subsystem(dev)) == 0)
    		product = udev_device_get_sysattr_value(devP, "idProduct");
    	if (!product)
    		product = "0000";
    }

	if (strcmp(vendor,"0000") != 0 && strcmp(product,"0000") != 0)
	{
		CHAR vendorID[VID_STR_LEN + 1];
		CHAR productID[PID_STR_LEN + 1];
		CHAR HwID[QCDEV_MAX_VALUE_NAME];
		memset(vendorID, 0, VID_STR_LEN + 1);
		memset(productID, 0, PID_STR_LEN + 1);
		memset(HwID, 0, QCDEV_MAX_VALUE_NAME);

		if (strcasecmp("pci",udev_device_get_subsystem(dev)) == 0)
			hwid = "PCI\\VEN_";
        else if (strcasecmp("usb",udev_device_get_subsystem(dev)) == 0)
			hwid = "USB\\VID_";

		strncpy(vendorID, vendor,VID_STR_LEN + 1);
		strncpy(productID, product,PID_STR_LEN + 1);
		strncpy(HwID, hwid, strlen(hwid)+1);

		strcat(HwID, vendorID);

		if (strcasecmp("pci",udev_device_get_subsystem(dev)) == 0)
			strcat(HwID, "&DEV_");
        else if (strcasecmp("usb",udev_device_get_subsystem(dev)) == 0)
			strcat(HwID, "&PID_");

		strcat(HwID, productID);

        if (cid != NULL)
            strcat(HwID, cid);

		if (strncpy(pdeviceCtx->mDevParams.HwId,
				HwID, QCDEV_MAX_VALUE_NAME - 1) != NULL)
		{
			 pdeviceCtx->mDevParams.HwId[QCDEV_MAX_VALUE_NAME - 1] = '\0';
			 pdeviceCtx->mDevParams.HwIdBufLen = strlen(pdeviceCtx->mDevParams.HwId);
		}

		QCDEV_LOG_DBG("pdeviceCtx->mDevParams.HwId: %s\n", pdeviceCtx->mDevParams.HwId);
	}
}

/**
 * @brief   To pop the Device info from the Main Ctx
 *
 * The device info deviceCtx is poped from the main Ctx
 *
 * @param   devContextInt   Device Cntx (device info)
 *
 * @returns Nothing
 */
static void popFromDeviceCtx(PVOID devContextInt)
{
    QCDEV_LOG_DBG("In\n");

    PDeviceCtx traversal, prevNode;

    /* Lock a mutex and unlock it on return. */
    QCDEV_LOCK_MUTEX_AND_UNLOCK_ON_RETURN(&qcdevCtx.mListLock);
    if (NULL == qcdevCtx.mpDeviceCtx)
    {
        QCDEV_LOG_ERR("No data found to pop out\n");
        goto fail;
    }

    traversal = qcdevCtx.mpDeviceCtx;

    if (traversal != NULL && traversal->mContextInt == devContextInt)
    {
        qcdevCtx.mpDeviceCtx = traversal->mNext;
        goto end;
    }

    /* Retrive the device info from the stored context */
    while (traversal != NULL && traversal->mContextInt != devContextInt)
    {
        prevNode = traversal;
        traversal = traversal->mNext;
    }

    /* Clear the device info */
    if (traversal == NULL)
    {
        goto fail;
    }

    prevNode->mNext = traversal->mNext;

end:
    /* Destructor of _DeviceCtx handles mDevParams.DevDetails cleanup */
    delete traversal;

    QCDEV_LOG_DBG("Out\n");

fail:
    return;
}

/**
 * @brief   To extract the Device details from the Main Ctx
 *
 * The device info deviceCtx is extracted based on the devnode
 * from main device Ctx
 *
 * @param   devNode   Device Node (unique name)
 *
 * @returns deviceCtx on Success
 *          NULL on Failure
 */
static void *fetchFromDeviceCtx(PVOID devNode)
{
    PDeviceCtx traversal = NULL;

    /* Lock a mutex and unlock it on return. */
    QCDEV_LOCK_MUTEX_AND_UNLOCK_ON_RETURN(&qcdevCtx.mListLock);

    if (NULL == qcdevCtx.mpDeviceCtx)
    {
        //QCDEV_LOG_ERR("No data found to fetch\n");
        return NULL;
    }

    /* Retrive the device info from the stored context */
    traversal = qcdevCtx.mpDeviceCtx;
    while (traversal != NULL &&
            strncmp(traversal->mDevNode, (const char*)devNode,(QCDEV_MAX_VALUE_NAME-1)))

    {
        traversal = traversal->mNext;
    }

    /* It can be NULL or required data */
    return traversal;
}

/**
 * @brief   To extract the device name corresponding to TTY device
 *
 * The device Name corresponding to TTY (Diag/Modem) is extracted
 *
 * @param   devName   Device name populated (ex: /ttyUSBx )
 * @param   serialNum Serail number of device
 *
 * @returns vendor specific path on Success
 *          NULL on Failure
 */
static void *getName(const void *devName, const char *serialNum)
{
    QCDEV_LOG_DBG("In\n");
    DIR *dirp;
    struct dirent *dirent_p;
    static char path[QCDEV_PORT_NAME_LEN];
    char *sn = serialNum;

    memset(path, 0, QCDEV_PORT_NAME_LEN);
    if (sn == NULL)
    {
        sn = "NULL";
    }
    /* Traverse into dir '/dev/' and extract the device path based on devName*/
    if ((dirp = opendir(QCDEV_PORT_DIR)))
    {
        while ((dirent_p = readdir(dirp)))
        {
            /* Extract based on <SER_NUM>**ttyUSB0 */
            if (strcasestr(dirent_p->d_name, (const char *)(devName)))
            {
                snprintf(path, QCDEV_PORT_NAME_LEN, "%s%s", DevNode, dirent_p->d_name);
                break;
            }
        }
    }
    if (dirp && closedir(dirp) < 0)
    {
        QCDEV_LOG_ERR("Failed to close dirp\n");
    }
    QCDEV_LOG_DBG("Out\n");
    return path;
}

std::unordered_map<std::string, std::string> ParseDevDesc(const char* input, std::unordered_map<std::string, std::string>& deviceMap)
      {
          QCDEV_LOG_DBG("String:  %s\n", input);
          if (!input || strlen(input) == 0)
          {
              QCDEV_LOG_ERR("ParseDevDesc: Invalid or empty input\n");
              return deviceMap;
          }
          std::string s(input);

          size_t start = 0, n = s.size();
          while (start < n) {
              size_t sep = s.find('_', start);
              size_t end = (sep == std::string::npos) ? n : sep;
              size_t colon = s.find(':', start);

              if (colon != std::string::npos && colon < end) {
                  std::string key = s.substr(start, colon - start);
                  std::string val = s.substr(colon + 1, end - (colon + 1));
                  QCDEV_LOG_DBG("DevDetails: %s=%s\n", key.c_str(), val.c_str());
                  deviceMap.emplace(std::move(key), std::move(val));
              }
              start = (sep == std::string::npos) ? n : sep + 1;
          }

          return deviceMap;
      }

/**
 * @brief   To process the device info on trigger (called by processAddDevice)
 *
 * To process the PORT/NET device (Diag/Modem) and extract the required info
 *
 * @param   device      udev device info (ex: /ttyUSBx , /qcqmiX)
 * @param   pdeviceCtx  device cntx which is to be filled
 *
 * @returns QCDEV_TRUE on Success
 *          QCDEV_FALSE Failure
 */
static bool extractDevInfo(struct udev_device *device, PDeviceCtx pdeviceCtx)
{
    QCDEV_LOG_INFO("In\n");

    struct udev_device *device_parent = device;
    const char *str;
    const char *value;
    char *vendorId = NULL;
	struct udev_device *devp_tmp = NULL;
    std::unordered_map<std::string, std::string> devDetailsMP;
    if (qcdevCtx.mSetting && qcdevCtx.mSetting->VID)
    {
        vendorId = qcdevCtx.mSetting->VID;
        /* Assuming the format would be VID_05C6 */
        if (vendorId = strrchr(vendorId, '_'))
        {
            /* Move to the next location */
            vendorId++;
        }
    }

    /* Traverse to get the parent info, so that serial and
     * bus path can be obtained
     */
    device_parent = device;
    do
    {
        device_parent = udev_device_get_parent(device_parent);
        if (device_parent == NULL)
            break;

        value = udev_device_get_sysattr_value(device_parent, "idVendor");
        if (value && (!vendorId || !strcasecmp(vendorId, value)))
            break;
	if ((!strcasecmp(pdeviceCtx->mSubsystem, SUBSYSTEM_MHI)) ||
	    (!strcasecmp(pdeviceCtx->mSubsystem, SUBSYSTEM_WWAN)))
	{
        const char *parentPath = udev_device_get_devpath(device_parent);
	   strncpy(pdeviceCtx->mDevParams.Loc, parentPath, QCDEV_MAX_VALUE_NAME - 1);
	   if (parentPath != NULL)
        {
              char *p = parentPath + strlen(parentPath);
	      while (p > parentPath && *p != '/')
              {
                 --p;
              }
            if (*p == '/') ++p;
            strncpy(pdeviceCtx->mDevParams.ParentDev, p, QCDEV_MAX_VALUE_NAME - 1);
            strncpy(pdeviceCtx->mDevParams.ParentLocationInfomation, p, QCDEV_MAX_VALUE_NAME - 1);
        }

	   devp_tmp = udev_device_get_parent(device_parent);
	   if (devp_tmp) {
	       if (strcasecmp("pci",udev_device_get_subsystem(devp_tmp)) == 0) {
	    	  getHwID(devp_tmp, pdeviceCtx, NULL);
	    	  break;
	       }
	   }
	}

    } while (device_parent != NULL);

    if (device_parent)
    {
        const char *serial;
        /* Extract the serial number */
        serial = udev_device_get_sysattr_value(device_parent, "serial");
	    if (serial == NULL)
	    {
               // for MHI devices
               if (NULL != (serial = udev_device_get_sysattr_value(device_parent, "serial_number")))
               {
                  if (strcasestr(serial, "Serial Number: ") != NULL)
                  {
                     serial += strlen("Serial Number: ");
                  }
                  if (strncpy(pdeviceCtx->mDevParams.SerNumMsm,
                              serial, QCDEV_MAX_VALUE_NAME - 1) != NULL)
                  {
                      pdeviceCtx->mDevParams.SerNumMsm[QCDEV_MAX_VALUE_NAME - 1] = '\0';
                      pdeviceCtx->mDevParams.SerNumMsmBufLen = strlen(pdeviceCtx->mDevParams.SerNumMsm);
                  }
               }
	    }
        else
        {
            if (strncpy(pdeviceCtx->mDevParams.SerNum,
                        serial, QCDEV_MAX_VALUE_NAME - 1) != NULL)
            {
                pdeviceCtx->mDevParams.SerNum[QCDEV_MAX_VALUE_NAME - 1] = '\0';
                pdeviceCtx->mDevParams.SerNumBufLen = strlen(pdeviceCtx->mDevParams.SerNum);
            }
        }
	    getHwID(device_parent, pdeviceCtx, NULL);
        value = udev_device_get_devnode(device);
        if (value != NULL)
        {
            if (strncpy(pdeviceCtx->mDevParams.DevName,
                        value, QCDEV_MAX_VALUE_NAME - 1) != NULL)
            {
                pdeviceCtx->mDevParams.DevName[QCDEV_MAX_VALUE_NAME - 1] = '\0';
                pdeviceCtx->mDevParams.DevnameBufLen = strlen(pdeviceCtx->mDevParams.DevName);
            }
        }
        if (value && strstr(value, "tty"))
        {
            /* To extract the file name '/dev/qcqmiX' */
            if (!(value = strrchr(value, '/')))
            {
                QCDEV_LOG_ERR("QCDEV_FALSE\n");
                return QCDEV_FALSE;
            }
            value += 1;
            /* Device name is extracted
             * ex: Qualcomm_HS-USB_Diagnostics_9091:d6c76c01:ttyUSB0
             */
            value = (const char *)getName(value, serial);
            if (value != NULL)
            {
                if (strncpy(pdeviceCtx->mDevParams.DevDesc,
                            value, QCDEV_MAX_VALUE_NAME - 1) != NULL)
                {
                    pdeviceCtx->mDevParams.DevDesc[QCDEV_MAX_VALUE_NAME - 1] = '\0';
                    pdeviceCtx->mDevParams.DevDescBufLen = strlen(pdeviceCtx->mDevParams.DevDesc);
                }
            }
        }
        else if (value != NULL)
        {
            if (strncpy(pdeviceCtx->mDevParams.DevDesc,
                        value, QCDEV_MAX_VALUE_NAME - 1) != NULL)
            {
                pdeviceCtx->mDevParams.DevDesc[QCDEV_MAX_VALUE_NAME - 1] = '\0';
                pdeviceCtx->mDevParams.DevDescBufLen = strlen(pdeviceCtx->mDevParams.DevDesc);
            }
        }

        /* Extract the bus path from parent device ex: /dev/bus/usb/002/006 */
        /* BUS name, directly obtaining from parent */
        value = udev_device_get_devnode(device_parent);
        if (value != NULL)
        {
            if (strncpy(pdeviceCtx->mDevParams.Loc,
                        value, QCDEV_MAX_VALUE_NAME - 1) != NULL)
            {
                pdeviceCtx->mDevParams.Loc[QCDEV_MAX_VALUE_NAME - 1] = '\0';
                pdeviceCtx->mDevParams.LocBufLen = strlen(pdeviceCtx->mDevParams.Loc);
            }
        }

        /*TODO: Need to obtain MTU */
        pdeviceCtx->mDevParams.Mtu = 1500;

        value = udev_device_get_sysattr_value(device_parent, "product");
        if (value != NULL)
        {
            pdeviceCtx->mProductNameLen = 0;
            if (strncpy(pdeviceCtx->mProductName,
                        value, QCDEV_MAX_VALUE_NAME - 1) != NULL)
            {
                pdeviceCtx->mProductName[QCDEV_MAX_VALUE_NAME - 1] = '\0';
                pdeviceCtx->mProductNameLen = strlen(pdeviceCtx->mProductName);
            }
            ParseDevDesc(pdeviceCtx->mProductName, devDetailsMP);
            pdeviceCtx->mDevParams.DevDetails = std::make_unique<DevDetailsMap>(devDetailsMP);
        }

        value = strcasestr(pdeviceCtx->mProductName, "_CID:");
        if (value != NULL)
        {
            /* As the string start with _CID: and end with delimiter[SPACE, "_",
			 * or end of the size of string descriptor]
			 */
            CHAR HwID[QCDEV_MAX_VALUE_NAME];
            memset(HwID, 0, QCDEV_MAX_VALUE_NAME);
			char *delimiter = "_ ";
			value += strlen("_CID:");
            /* To extract the product serial info */
			if (value = strtok_r(value, delimiter, &value))
			{
                strcat(HwID, "&CID_");
                strcat(HwID, value);
                getHwID(device_parent, pdeviceCtx, HwID);
            }
        } 
    }
    else
    {
        return QCDEV_FALSE;
    }
    QCDEV_LOG_DBG("Out\n");

    return QCDEV_TRUE;
}

/**
 * @brief   To refine the DevPath value for consistent HW path representation
 *
 * To make sure lib returns consistent HW path from a same physical connection
 *
 * @param   pdeviceCtx      device context
 *
 * @returns Nothing  Function modifies the hw_path
 */
static void refineDeviceHardwarePath(PDeviceCtx pdeviceCtx)
{
    PCHAR hw_path = pdeviceCtx->mDevParams.DevPath;
    PCHAR p;

    if ((hw_path == NULL) || (hw_path[0] == '\0'))
    {
        return;
    }

    // save value of devnode for removal
    if (strncpy(pdeviceCtx->mDevNode, hw_path, QCDEV_MAX_VALUE_NAME - 1) != NULL)
    {
        pdeviceCtx->mDevNode[QCDEV_MAX_VALUE_NAME - 1] = '\0';
    }

    // handle "usb" device for now, may add "pcie" in the future if needed
    if ((p = strcasestr(hw_path, "usb")) != NULL)
    {
        while (*p != '\0')
        {
           if (*p == ':')
           {
               *p = '\0';
               break;
           }
           p++;
        }
    }
}

/**
 * @brief   To process the device info on add trigger(called by process_device)
 *
 * To process the devices on state add/present and save the data in main Ctx
 *
 * @param   dev      udev device info
 *
 * @returns Nothing  Saves the data to main Ctx
 */
static void processAddDevice(struct udev_device *dev)
{
    PDeviceCtx pdeviceCtx;
    struct udev_device *devP;
    PCHAR value;
    PVOID bInterfaceProtocol;
    PVOID bInterfaceClass;
    PVOID bAlternateSetting;
    PVOID bInterfaceNumber;
    int isQcDriver = 1;

    devP = udev_device_get_parent(dev);

    /* In case of TTY susystem vendor Id cannot be obtained directly */
    const char *vendor = udev_device_get_sysattr_value(dev, "idVendor");
    if (!vendor)
    {
        vendor = udev_device_get_sysattr_value(devP, "idVendor");
        if (!vendor)
            vendor = "0000";
    }

    /* To check subsystem of the uevent (USB/ TTY)*/
    const char *subsystem = udev_device_get_subsystem(dev);
    if (!subsystem)
    {
        /* This should not happen */
        QCDEV_LOG_ERR("subsystem name is null \n");
        return;
    }

    const char *devNode = udev_device_get_devnode(dev);
    if (!devNode)
        devNode = "NULL";

    pdeviceCtx = new _DeviceCtx();
    if (NULL == pdeviceCtx)
    {
        QCDEV_LOG_ERR("Failed to allocate memorey\n");
        return;
    }

    pdeviceCtx->mDevParams.DevDetails = std::make_unique<DevDetailsMap>();

    strncpy(pdeviceCtx->mSubsystem, subsystem, QCDEV_MAX_VALUE_NAME - 1);

    value = udev_device_get_devpath(dev);
    if (value != NULL)
    {
        if (strncpy(pdeviceCtx->mDevParams.DevPath,
                    value, QCDEV_MAX_VALUE_NAME - 1) != NULL)
        {
            pdeviceCtx->mDevParams.DevPath[QCDEV_MAX_VALUE_NAME - 1] = '\0';
            pdeviceCtx->mDevParams.DevPathBufLen = strlen(pdeviceCtx->mDevParams.DevPath);
        }
    }

    getHwID(dev, pdeviceCtx, NULL);

    if (strcasestr(subsystem, SUBSYSTEM_TTY) &&
        qcdevCtx.mSetting &&
        (qcdevCtx.mSetting->Settings & DEV_FEATURE_INCLUDE_NONE_QC_PORTS) &&
        (!qcdevCtx.mSetting->DeviceClass ||
         (qcdevCtx.mSetting->DeviceClass & DEV_CLASS_PORTS)))
    {
        struct udev_device *devPP;
        devPP = udev_device_get_parent(devP);
        /* Process the TTYUSB device and fills pdeviceCtx
         * Fails in case of non Qualcomm tty device (05c6)
         */
        if (!strcasestr(devNode, QCDEV_TTY_NAME) ||
            QCDEV_FALSE == extractDevInfo(dev, pdeviceCtx))
        {
            delete pdeviceCtx;
            return;
        }

        QCDEV_LOG_INFO("Arrival: %s : %s\n",subsystem, devNode);

        pdeviceCtx->mDevParams.Flag =
            ((ULONG)QC_DEV_TYPE_PORTS << 8) | (QC_DEV_STATE_ARRIVAL << 4) | ((ULONG)QC_DEV_BUS_TYPE_USB << 16) | (isQcDriver);

        bInterfaceProtocol = udev_device_get_sysattr_value(devPP, "bInterfaceProtocol");
        bInterfaceClass = udev_device_get_sysattr_value(devPP, "bInterfaceClass");
        bAlternateSetting = udev_device_get_sysattr_value(devPP, "bAlternateSetting");
        bInterfaceNumber = udev_device_get_sysattr_value(devPP, "bInterfaceNumber");

	if ((!bInterfaceProtocol) || (!bInterfaceClass) || (!bAlternateSetting)
			    || (!bInterfaceNumber))
	{
	    const char *product = udev_device_get_sysattr_value(dev, "idProduct");
	    if (!product)
		product = "0000";
	    QCDEV_LOG_INFO("Invalid interface properties\n");
	    QCDEV_LOG_INFO("Vendor= [%s], product= [%s], subsystem= [%s], devNode= [%s]\n",
			vendor, product, subsystem, devNode);
	    QCDEV_LOG_DBG("bInterfaceProtocol= [%s], bInterfaceClass= [%s], \
			bAlternateSetting= [%s], bInterfaceNumber= [%s]\n",
			bInterfaceProtocol, bInterfaceClass, bAlternateSetting,
			bInterfaceNumber);
	    delete pdeviceCtx;
	    return;
	}

        pdeviceCtx->mDevParams.Protocol =
            strtoul(bInterfaceProtocol, NULL, 16) |
            strtoul(bInterfaceClass, NULL, 16) << 8 |
            strtoul(bAlternateSetting, NULL, 16) << 16 |
            strtoul(bInterfaceNumber, NULL, 16) << 24;
    }
    else if ((strcasestr(subsystem, SUBSYSTEM_QCOM_PORTS)       ||
             strcasestr(subsystem, OLDER_SUBSYSTEM_GOBIPORTS)) &&
             (!qcdevCtx.mSetting->DeviceClass ||
              (qcdevCtx.mSetting->DeviceClass & (DEV_CLASS_PORTS | DEV_CLASS_SAHARA))))
    {
        QCDEV_LOG_INFO("Arrival: %s : %s\n",subsystem, devNode);
        /* Process the USB device and obtain the RMNET Info*/
        if (QCDEV_FALSE == extractDevInfo(dev, pdeviceCtx))
        {
            QCDEV_LOG_ERR("DEV_CLASS_PORTS extractDevInfo Failed\n");
            delete pdeviceCtx;
            return;
        }

        bInterfaceProtocol = udev_device_get_sysattr_value(devP, "bInterfaceProtocol");
        bInterfaceClass = udev_device_get_sysattr_value(devP, "bInterfaceClass");
        bAlternateSetting = udev_device_get_sysattr_value(devP, "bAlternateSetting");
        bInterfaceNumber = udev_device_get_sysattr_value(devP, "bInterfaceNumber");

	if ((!bInterfaceProtocol) || (!bInterfaceClass) || (!bAlternateSetting)
			    || (!bInterfaceNumber))
	{
	    const char *product = udev_device_get_sysattr_value(dev, "idProduct");
	    if (!product)
		product = "0000";
	    QCDEV_LOG_INFO("Invalid interface properties\n");
	    QCDEV_LOG_INFO("Vendor= [%s], product= [%s], subsystem= [%s], devNode= [%s]\n",
			vendor, product, subsystem, devNode);
	    QCDEV_LOG_DBG("bInterfaceProtocol= [%s], bInterfaceClass= [%s], \
			bAlternateSetting= [%s], bInterfaceNumber= [%s]\n",
			bInterfaceProtocol, bInterfaceClass, bAlternateSetting,
			bInterfaceNumber);
	    delete pdeviceCtx;
	    return;
	}

        pdeviceCtx->mDevParams.Protocol =
            strtoul(bInterfaceProtocol, NULL, 16) |
            strtoul(bInterfaceClass, NULL, 16) << 8 |
            strtoul(bAlternateSetting, NULL, 16) << 16 |
            strtoul(bInterfaceNumber, NULL, 16) << 24;

        if (!IsDeviceAllowed(pdeviceCtx->mDevParams.Protocol))
        {
            delete pdeviceCtx;
            return;
        }

        {
            ULONG ifaceProtocol = strtoul(bInterfaceProtocol, NULL, 16);
            ULONG devType = (ProtocolToDeviceClass(ifaceProtocol) == DEV_CLASS_SAHARA)
                                ? QC_DEV_TYPE_SAHARA : QC_DEV_TYPE_PORTS;
            pdeviceCtx->mDevParams.Flag =
                (devType << 8) | (QC_DEV_STATE_ARRIVAL << 4) |
                ((ULONG)QC_DEV_BUS_TYPE_USB << 16) | isQcDriver;
        }
    }
    else if ((strcasestr(subsystem, SUBSYSTEM_QCOM_USB)     || 
             strcasestr(subsystem, OLDER_SUBSYSTEM_GOBIUSB)) &&
             (!qcdevCtx.mSetting->DeviceClass ||
              (qcdevCtx.mSetting->DeviceClass & DEV_CLASS_USB)))
    {
        QCDEV_LOG_INFO("Arrival: %s : %s\n",subsystem, devNode);
        /* Process the USB device and obtain the RMNET Info*/
        if (QCDEV_FALSE == extractDevInfo(dev, pdeviceCtx))
        {
            QCDEV_LOG_ERR("qcom_usb extractDevInfo Failed\n");
            delete pdeviceCtx;
            return;
        }
        pdeviceCtx->mDevParams.Flag =
            ((ULONG)QC_DEV_TYPE_USB << 8) | (QC_DEV_STATE_ARRIVAL << 4) |
            ((ULONG)QC_DEV_BUS_TYPE_USB << 16) | isQcDriver;

        bInterfaceProtocol = udev_device_get_sysattr_value(devP, "bInterfaceProtocol");
        bInterfaceClass = udev_device_get_sysattr_value(devP, "bInterfaceClass");
        bAlternateSetting = udev_device_get_sysattr_value(devP, "bAlternateSetting");
        bInterfaceNumber = udev_device_get_sysattr_value(devP, "bInterfaceNumber");

	if ((!bInterfaceProtocol) || (!bInterfaceClass) || (!bAlternateSetting)
			    || (!bInterfaceNumber))
	{
	    const char *product = udev_device_get_sysattr_value(dev, "idProduct");
	    if (!product)
		    product = "0000";
	    QCDEV_LOG_INFO("Invalid interface properties\n");
	    QCDEV_LOG_INFO("Vendor= [%s], product= [%s], subsystem= [%s], devNode= [%s]\n",
			vendor, product, subsystem, devNode);
	    QCDEV_LOG_DBG("bInterfaceProtocol= [%s], bInterfaceClass= [%s], \
			bAlternateSetting= [%s], bInterfaceNumber= [%s]\n",
			bInterfaceProtocol, bInterfaceClass, bAlternateSetting,
			bInterfaceNumber);
	    delete pdeviceCtx;
	    return;
	}
        pdeviceCtx->mDevParams.Protocol =
            strtoul(bInterfaceProtocol, NULL, 16) |
            strtoul(bInterfaceClass, NULL, 16) << 8 |
            strtoul(bAlternateSetting, NULL, 16) << 16 |
            strtoul(bInterfaceNumber, NULL, 16) << 24;
    }
    else if ((strcasestr(subsystem, SUBSYSTEM_QCOM_NET)      || 
             strcasestr(subsystem, OLDER_SUBSYSTEM_GOBINET)) &&
             (!qcdevCtx.mSetting->DeviceClass ||
              (qcdevCtx.mSetting->DeviceClass & DEV_CLASS_NET)))
    {
        QCDEV_LOG_INFO("Arrival: %s : %s\n",subsystem, devNode);
        /* Process the USB device and obtain the RMNET Info*/
        if (QCDEV_FALSE == extractDevInfo(dev, pdeviceCtx))
        {
            QCDEV_LOG_ERR("SUBSYTEM qcom extractDevInfo Failed \n");
            delete pdeviceCtx;
            return;
        }
        pdeviceCtx->mDevParams.Flag =
            ((ULONG)QC_DEV_TYPE_NET << 8) | (QC_DEV_STATE_ARRIVAL << 4) | ((ULONG)QC_DEV_BUS_TYPE_USB << 16) | isQcDriver;
        /* extract the interface name from dev name ex: qcqmiX => usbX*/
        value = pdeviceCtx->mDevParams.DevName;
        value = strstr(value, QCDEV_QMI_NAME);

        if (value != NULL)
        {
            value += strlen(QCDEV_QMI_NAME);
            pdeviceCtx->mDevParams.IfNameBufLen = 0;

            if (snprintf(pdeviceCtx->mDevParams.IfName, QCDEV_PORT_NAME_LEN,
                         "usb%s", value) > 0)
            {
                pdeviceCtx->mDevParams.IfNameBufLen =
                    strlen(pdeviceCtx->mDevParams.IfName);
            }
        }

        bInterfaceProtocol = udev_device_get_sysattr_value(devP, "bInterfaceProtocol");
        bInterfaceClass = udev_device_get_sysattr_value(devP, "bInterfaceClass");
        bAlternateSetting = udev_device_get_sysattr_value(devP, "bAlternateSetting");
        bInterfaceNumber = udev_device_get_sysattr_value(devP, "bInterfaceNumber");

	if ((!bInterfaceProtocol) || (!bInterfaceClass) || (!bAlternateSetting)
			    || (!bInterfaceNumber))
	{
	    const char *product = udev_device_get_sysattr_value(dev, "idProduct");
	    if (!product)
		product = "0000";
	    QCDEV_LOG_INFO("Invalid interface properties\n");
	    QCDEV_LOG_INFO("Vendor= [%s], product= [%s], subsystem= [%s], devNode= [%s]\n",
			vendor, product, subsystem, devNode);
	    QCDEV_LOG_DBG("bInterfaceProtocol= [%s], bInterfaceClass= [%s], \
			bAlternateSetting= [%s], bInterfaceNumber= [%s]\n",
			bInterfaceProtocol, bInterfaceClass, bAlternateSetting,
			bInterfaceNumber);
	    delete pdeviceCtx;
	    return;
	}

        pdeviceCtx->mDevParams.Protocol =
            strtoul(bInterfaceProtocol, NULL, 16) |
            strtoul(bInterfaceClass, NULL, 16) << 8 |
            strtoul(bAlternateSetting, NULL, 16) << 16 |
            strtoul(bInterfaceNumber, NULL, 16) << 24;
    }
    else if ((strcasestr(subsystem, SUBSYSTEM_MHI) || strcasestr(subsystem, SUBSYSTEM_WWAN)) &&
             (!qcdevCtx.mSetting->DeviceClass ||
              (qcdevCtx.mSetting->DeviceClass & DEV_CLASS_MDM)))
    {
        QCDEV_LOG_INFO("Arrival: %s : %s\n",subsystem, devNode);
        /* Process the MHI device and obtain the MHI Dev Info*/
	if (!strcasecmp(devNode, "NULL") || (QCDEV_FALSE == extractDevInfo(dev, pdeviceCtx)))
        {
            QCDEV_LOG_ERR("MHI DEV extractDevInfo Failed\n");
            delete pdeviceCtx;
            return;
        }
        pdeviceCtx->mDevParams.Flag =
            ((ULONG)QC_DEV_TYPE_PORTS << 8) | (QC_DEV_STATE_ARRIVAL << 4) | ((ULONG)QC_DEV_BUS_TYPE_PCIE << 16) | isQcDriver;
        pdeviceCtx->mDevParams.Protocol = 0;
    }
    else if (qcdevCtx.mSetting &&
             (!qcdevCtx.mSetting->DeviceClass ||
              (qcdevCtx.mSetting->DeviceClass & (DEV_CLASS_NET | DEV_CLASS_SAHARA))) &&
             (!(qcdevCtx.mSetting->Settings & DEV_FEATURE_SCAN_USB_WITH_VID) || !(qcdevCtx.mSetting)->VID || NULL != strcasestr((qcdevCtx.mSetting)->VID, vendor)))
    {
        QCDEV_LOG_INFO("Arrival: %s : %s\n",subsystem, devNode);
        /* To extract the serial num  */
        const char *serial = udev_device_get_sysattr_value(dev, "serial");
        if (!serial)
            serial = "0000";

        if (!udev_device_get_devnode(dev))
        {
            PVOID bInterfaceSubClass;
            ULONG ifaceProtocol;
            ULONG ifaceClass;
            ULONG ifaceSubClass;
            ULONG alterSetting;
            ULONG ifaceNum;

            bInterfaceProtocol = udev_device_get_sysattr_value(dev, "bInterfaceProtocol");
            bInterfaceSubClass = udev_device_get_sysattr_value(dev, "bInterfaceSubClass");
            bInterfaceClass = udev_device_get_sysattr_value(dev, "bInterfaceClass");
            bAlternateSetting = udev_device_get_sysattr_value(dev, "bAlternateSetting");
            bInterfaceNumber = udev_device_get_sysattr_value(dev, "bInterfaceNumber");

	    if ((!bInterfaceProtocol) || (!bInterfaceSubClass) || (!bInterfaceClass)
		    || (!bAlternateSetting) || (!bInterfaceNumber))
            {
		const char *product = udev_device_get_sysattr_value(dev, "idProduct");
		if (!product)
		    product = "0000";
		QCDEV_LOG_INFO("Invalid interface properties\n");
		QCDEV_LOG_INFO("Vendor= [%s], product= [%s], subsystem= [%s], devNode= [%s]\n",
			    vendor, product, subsystem, devNode);
		QCDEV_LOG_DBG("bInterfaceProtocol= [%s], bInterfaceSubClass= [%s],	    \
			    bInterfaceClass= [%s], bAlternateSetting= [%s],		    \
			    bInterfaceNumber= [%s]\n", bInterfaceProtocol, bInterfaceSubClass,
				bInterfaceClass, bAlternateSetting, bInterfaceNumber);
                delete pdeviceCtx;
                return;
            }

            ifaceProtocol = strtoul(bInterfaceProtocol, NULL, 16);
            ifaceClass = strtoul(bInterfaceClass, NULL, 16);
            ifaceSubClass = strtoul(bInterfaceSubClass, NULL, 16);
            alterSetting = strtoul(bAlternateSetting, NULL, 16);
            ifaceNum = strtoul(bInterfaceNumber, NULL, 16);

            bool isSaharaFamily = (ProtocolToDeviceClass(ifaceProtocol) == DEV_CLASS_SAHARA);

            if (isSaharaFamily)
            {
                /* no-devnode udev events. Drop this event. */
                delete pdeviceCtx;
                return;
            }
            else
            {
                /* ADB / RNDIS / Fastboot — identified by (protocol, class, subclass) triple.
                 * Gated by DEV_CLASS_ADB subscription; caller sets it to receive these. */
                bool isAdb = (ifaceProtocol == QCDEV_INTERFACE_PROTOCOL_ADB &&
                              ifaceClass == QCDEV_INTERFACE_CLASS_ADB &&
                              ifaceSubClass == QCDEV_INTERFACE_SUBCLASS_ADB);
                bool isFastboot = (ifaceProtocol == QCDEV_INTERFACE_PROTOCOL_FASTBOOT &&
                                   ifaceClass == QCDEV_INTERFACE_CLASS_FASTBOOT &&
                                   ifaceSubClass == QCDEV_INTERFACE_SUBCLASS_FASTBOOT);
                bool isRndis = (ifaceProtocol == QCDEV_INTERFACE_PROTOCOL_RNDIS &&
                                ifaceClass == QCDEV_INTERFACE_CLASS_RNDIS &&
                                ifaceSubClass == QCDEV_INTERFACE_SUBCLASS_RNDIS);

                if (!(isAdb || isFastboot || isRndis))
                {
                    delete pdeviceCtx;
                    return;
                }
            }

            pdeviceCtx->mDevParams.Protocol =
                strtoul(bInterfaceProtocol, NULL, 16) |
                strtoul(bInterfaceClass, NULL, 16) << 8 |
                strtoul(bAlternateSetting, NULL, 16) << 16 |
                strtoul(bInterfaceNumber, NULL, 16) << 24;

            serial = udev_device_get_sysattr_value(devP, "serial");
            value = udev_device_get_sysattr_value(dev, "interface");

            if (value != NULL)
            {
                if (strncpy(pdeviceCtx->mDevParams.DevName,
                            value, QCDEV_MAX_VALUE_NAME - 1) != NULL)
                {
                    pdeviceCtx->mDevParams.DevName[QCDEV_MAX_VALUE_NAME - 1] = '\0';
                    pdeviceCtx->mDevParams.DevnameBufLen = strlen(pdeviceCtx->mDevParams.DevName);
                }
            }
            if (value != NULL)
            {
                if (strncpy(pdeviceCtx->mDevParams.Loc,
                            value, QCDEV_MAX_VALUE_NAME - 1) != NULL)
                {
                    pdeviceCtx->mDevParams.Loc[QCDEV_MAX_VALUE_NAME - 1] = '\0';
                    pdeviceCtx->mDevParams.LocBufLen = strlen(pdeviceCtx->mDevParams.Loc);
                }
            }

            if (value != NULL)
            {
                if (strncpy(pdeviceCtx->mDevParams.DevDesc,
                            value, QCDEV_MAX_VALUE_NAME - 1) != NULL)
                {
                    pdeviceCtx->mDevParams.DevDesc[QCDEV_MAX_VALUE_NAME - 1] = '\0';
                    pdeviceCtx->mDevParams.DevDescBufLen = strlen(pdeviceCtx->mDevParams.DevDesc);
                }
            }

            if (serial != NULL)
            {
                if (strncpy(pdeviceCtx->mDevParams.SerNum,
                            serial, QCDEV_MAX_VALUE_NAME - 1) != NULL)
                {
                    pdeviceCtx->mDevParams.SerNum[QCDEV_MAX_VALUE_NAME - 1] = '\0';
                    pdeviceCtx->mDevParams.SerNumBufLen = strlen(pdeviceCtx->mDevParams.SerNum);
                }
            }

            value = udev_device_get_sysattr_value(devP, "product");
            if (value != NULL)
            {
                if (strncpy(pdeviceCtx->mProductName,
                            value, QCDEV_MAX_VALUE_NAME - 1) != NULL)
                {
                    pdeviceCtx->mProductName[QCDEV_MAX_VALUE_NAME - 1] = '\0';
                    pdeviceCtx->mProductNameLen = strlen(pdeviceCtx->mProductName);
                }
            }
            pdeviceCtx->mDevParams.Mtu = 0;
            pdeviceCtx->mDevParams.Flag =
                QC_DEV_TYPE_USB << 8 | QC_DEV_BUS_TYPE_USB << 16 | QC_DEV_STATE_ARRIVAL << 4;
        }
        else
        {

            if (devNode != NULL)
            {
                if (strncpy(pdeviceCtx->mDevParams.DevName,
                            devNode, QCDEV_MAX_VALUE_NAME - 1) != NULL)
                {
                    pdeviceCtx->mDevParams.DevName[QCDEV_MAX_VALUE_NAME - 1] = '\0';
                    pdeviceCtx->mDevParams.DevnameBufLen = strlen(pdeviceCtx->mDevParams.DevName);
                }
            }

            if (devNode != NULL)
            {
                if (strncpy(pdeviceCtx->mDevParams.Loc,
                            devNode, QCDEV_MAX_VALUE_NAME - 1) != NULL)
                {
                    pdeviceCtx->mDevParams.Loc[QCDEV_MAX_VALUE_NAME - 1] = '\0';
                    pdeviceCtx->mDevParams.LocBufLen = strlen(pdeviceCtx->mDevParams.Loc);
                }
            }

            if (serial != NULL)
            {
                if (strncpy(pdeviceCtx->mDevParams.SerNum,
                            serial, QCDEV_MAX_VALUE_NAME - 1) != NULL)
                {
                    pdeviceCtx->mDevParams.SerNum[QCDEV_MAX_VALUE_NAME - 1] = '\0';
                    pdeviceCtx->mDevParams.SerNumBufLen = strlen(pdeviceCtx->mDevParams.SerNum);
                }
            }

            value = udev_device_get_sysattr_value(dev, "product");
            if (value != NULL)
            {
                if (strncpy(pdeviceCtx->mDevParams.DevDesc,
                            value, QCDEV_MAX_VALUE_NAME - 1) != NULL)
                {
                    pdeviceCtx->mDevParams.DevDesc[QCDEV_MAX_VALUE_NAME - 1] = '\0';
                    pdeviceCtx->mDevParams.DevDescBufLen = strlen(pdeviceCtx->mDevParams.DevDesc);
                }
            }

            pdeviceCtx->mDevParams.Mtu = 0;
            pdeviceCtx->mDevParams.Flag =
                QC_DEV_TYPE_USB << 8 | QC_DEV_BUS_TYPE_USB << 16 | QC_DEV_STATE_ARRIVAL << 4;

            value = udev_device_get_sysattr_value(dev, "product");
            if (value != NULL)
            {
                if (strncpy(pdeviceCtx->mProductName,
                            value, QCDEV_MAX_VALUE_NAME - 1) != NULL)
                {
                    pdeviceCtx->mProductName[QCDEV_MAX_VALUE_NAME - 1] = '\0';
                    pdeviceCtx->mProductNameLen = strlen(pdeviceCtx->mProductName);
                }
            }

            //extracting socver 
            value = strcasestr(pdeviceCtx->mProductName, "_SOCVER:");
            if (value != NULL)
            {
                char *delimiter = "_ ";
                value += strlen("_SOCVER:");
                if (value = strtok_r(value, delimiter, &value))
                {
                    if (strncpy(pdeviceCtx->mDevParams.SocVer,value,QCDEV_MAX_VALUE_NAME - 1) != NULL)
                    {
                        pdeviceCtx->mDevParams.SocVer[QCDEV_MAX_VALUE_NAME - 1] = '\0';
                        pdeviceCtx->mDevParams.SocVerLen = strlen(pdeviceCtx->mDevParams.SocVer);
                    }
                }
            } 
            else 
            {
                QCDEV_LOG_DBG("SOCVER not found!\n");
            }
            
            //TODO: Check for unicode / ascii
            value = strcasestr(pdeviceCtx->mProductName, "_SN:");
            if (value != NULL)
            {
                /* As the string start with _SN: and end with delimiter[SPACE, "_",
				 * or end of the size of string descriptor]
				 */
				char *delimiter = "_ ";
				value += strlen("_SN:");
				/* To extract the product serial info */
				if (value = strtok_r(value, delimiter, &value))
				{
					if (strncpy(pdeviceCtx->mDevParams.SerNumMsm,
								value, QCDEV_MAX_VALUE_NAME - 1) != NULL)
					{
						pdeviceCtx->mDevParams.SerNumMsm[QCDEV_MAX_VALUE_NAME - 1] = '\0';
						pdeviceCtx->mDevParams.SerNumMsmBufLen = strlen(pdeviceCtx->mDevParams.SerNumMsm);
					}
				}

				if(pdeviceCtx->mDevParams.Protocol == 0x0){
					char composite_name[QCDEV_MAX_VALUE_NAME];
					strncpy(composite_name, (PCHAR)pdeviceCtx->mDevParams.DevDesc, QCDEV_MAX_VALUE_NAME - 1);
					composite_name[QCDEV_MAX_VALUE_NAME - 1] = '\0';
					snprintf(pdeviceCtx->mDevParams.DevDesc, QCDEV_MAX_VALUE_NAME,
								  "Qualcomm USB Composite Device:%s", composite_name);
					if(pdeviceCtx->mDevParams.SerNum[0]){
						size_t devDescLen = strlen(pdeviceCtx->mDevParams.DevDesc);
						snprintf(pdeviceCtx->mDevParams.DevDesc + devDescLen,
						QCDEV_MAX_VALUE_NAME - devDescLen,":%s",pdeviceCtx->mDevParams.SerNum);
					}
					if(pdeviceCtx->mDevParams.Loc[0]){
						size_t devDescLen = strlen(pdeviceCtx->mDevParams.DevDesc);
						const char *locSuffix = strncmp(pdeviceCtx->mDevParams.Loc, BUS_LOC, strlen(BUS_LOC)) == 0
							? pdeviceCtx->mDevParams.Loc + strlen(BUS_LOC) : pdeviceCtx->mDevParams.Loc;
						snprintf(pdeviceCtx->mDevParams.DevDesc + devDescLen,
						QCDEV_MAX_VALUE_NAME - devDescLen,":%s",locSuffix);
					}
					pdeviceCtx->mDevParams.DevDescBufLen = strlen(pdeviceCtx->mDevParams.DevDesc);
					snprintf(pdeviceCtx->mDevParams.ParentDev, QCDEV_MAX_VALUE_NAME, "%s", pdeviceCtx->mDevParams.DevDesc);
					pdeviceCtx->mDevParams.ParentLocationInfomation[QCDEV_MAX_VALUE_NAME - 1] = '\0';
				}
			}
			else{
				delete pdeviceCtx;
				return ;
			}

            value = strcasestr(pdeviceCtx->mProductName, "_CID:");
            if (value != NULL)
            {
                /* As the string start with _CID: and end with delimiter[SPACE, "_",
				 * or end of the size of string descriptor]
				 */
                CHAR HwID[QCDEV_MAX_VALUE_NAME];
                memset(HwID, 0, QCDEV_MAX_VALUE_NAME);
				char *delimiter = "_ ";
				value += strlen("_CID:");
                /* To extract the product serial info */
				if (value = strtok_r(value, delimiter, &value))
				{
                    strcat(HwID, "&CID_");
                    strcat(HwID, value);
                    getHwID(dev, pdeviceCtx, HwID);
                }
            }

        }
    }
    else
    {
        /* Unknown Device */
        QCDEV_LOG_ERR("Unknown Device \n");
        delete pdeviceCtx;
        return;
    }
    /*ignore the controllers*/
    if (pdeviceCtx->mDevParams.DevDesc != NULL)
    {
        if (strcasestr(pdeviceCtx->mDevParams.DevDesc, "Controller"))
        {
            delete pdeviceCtx;
            return;
        }
    }

    /*Assigning parent device as Composite Interface*/
    struct udev_device *devPP;
    devPP = udev_device_get_parent(devP);
    const char *parentName;
    parentName = udev_device_get_sysattr_value(devPP, "product");
    if (parentName != NULL && (!strcasestr(parentName, "Controller")) &&
        strcasestr(parentName, "_SN:") &&
        snprintf(pdeviceCtx->mDevParams.ParentDev, QCDEV_MAX_VALUE_NAME,
                 "Qualcomm USB Composite Device:%s", parentName) > 0)
    {
        pdeviceCtx->mDevParams.ParentDev[QCDEV_MAX_VALUE_NAME - 1] = '\0';
        pdeviceCtx->mDevParams.ParentDevBufLen = strlen(pdeviceCtx->mDevParams.ParentDev);
        pdeviceCtx->mDevParams.ParentLocationInfomation[QCDEV_MAX_VALUE_NAME - 1] = '\0';
        pdeviceCtx->mDevParams.ParentLocationInfomationBufLen = strlen(pdeviceCtx->mDevParams.ParentLocationInfomation);
    }
    else
    {
        struct udev_device *devP;
        devP = udev_device_get_parent(dev);
        parentName = udev_device_get_sysattr_value(devP, "product");
        if (parentName != NULL &&
            (!strcasestr(parentName, "Controller")) &&
            strcasestr(parentName, "_SN:") &&
            snprintf(pdeviceCtx->mDevParams.ParentDev, QCDEV_MAX_VALUE_NAME,
                     "Qualcomm USB Composite Device:%s", parentName) > 0)
        {
            pdeviceCtx->mDevParams.ParentDev[QCDEV_MAX_VALUE_NAME - 1] = '\0';
            pdeviceCtx->mDevParams.ParentDevBufLen = strlen(pdeviceCtx->mDevParams.ParentDev);
            pdeviceCtx->mDevParams.ParentLocationInfomation[QCDEV_MAX_VALUE_NAME - 1] = '\0';
            pdeviceCtx->mDevParams.ParentLocationInfomationBufLen = strlen(pdeviceCtx->mDevParams.ParentLocationInfomation);
        }
    }

    // In case this is a single-function device
    if ((parentName != NULL) && (pdeviceCtx->mDevParams.SerNumMsm[0] == 0))
    {
         /* As the string start with _SOCVER: and end with delimiter[SPACE, "_",
         * or end of the size of string descriptor]
         */
        char *delimiter = "_ ";
        char *value;
        char tmpPrd_soc[QCDEV_MAX_VALUE_NAME];

        // make a copy as strtok_r may break up the original string
        strncpy(tmpPrd_soc, parentName, QCDEV_MAX_VALUE_NAME-1);
        tmpPrd_soc[QCDEV_MAX_VALUE_NAME - 1] = '\0';
        if ((value = strcasestr(tmpPrd_soc, "_SOCVER:")) != NULL)
        {
            value += strlen("_SOCVER:");
            /* To extract the product serial info */
            if (value = strtok_r(value, delimiter, &value))
            {
                if (strncpy(pdeviceCtx->mDevParams.SocVer, value, QCDEV_MAX_VALUE_NAME - 1) != NULL)
                {
                     pdeviceCtx->mDevParams.SocVer[QCDEV_MAX_VALUE_NAME - 1] = '\0';
                     pdeviceCtx->mDevParams.SocVerLen = strlen(pdeviceCtx->mDevParams.SocVer);
                }
            }
        }

        /* As the string start with _SN: and end with delimiter[SPACE, "_",
         * or end of the size of string descriptor]
         */
        char tmpPrd[QCDEV_MAX_VALUE_NAME];

        // make a copy as strtok_r may break up the original string
        strncpy(tmpPrd, parentName, QCDEV_MAX_VALUE_NAME-1);
        tmpPrd[QCDEV_MAX_VALUE_NAME - 1] = '\0';
        if ((value = strcasestr(tmpPrd, "_SN:")) != NULL)
        {
            value += strlen("_SN:");
            /* To extract the product serial info */
            if (value = strtok_r(value, delimiter, &value))
            {
                if (strncpy(pdeviceCtx->mDevParams.SerNumMsm, value, QCDEV_MAX_VALUE_NAME - 1) != NULL)
                {
                     pdeviceCtx->mDevParams.SerNumMsm[QCDEV_MAX_VALUE_NAME - 1] = '\0';
                     pdeviceCtx->mDevParams.SerNumMsmBufLen = strlen(pdeviceCtx->mDevParams.SerNumMsm);
                }
            }
        }
    }

    /* pushs pdeviceCtx to the device context */
    pushToDeviceCtx(pdeviceCtx);

    if (qcdevCtx.mCallback)
    {
        pdeviceCtx->mCbParams.DevDesc = pdeviceCtx->mDevParams.DevDesc;
        pdeviceCtx->mCbParams.DevName = pdeviceCtx->mDevParams.DevName;
        pdeviceCtx->mCbParams.IfName = pdeviceCtx->mDevParams.IfName;
        pdeviceCtx->mCbParams.Loc = pdeviceCtx->mDevParams.Loc;
        pdeviceCtx->mCbParams.DevPath = pdeviceCtx->mDevParams.DevPath;
        pdeviceCtx->mCbParams.SerNum = pdeviceCtx->mDevParams.SerNum;
        pdeviceCtx->mCbParams.SocVer = pdeviceCtx->mDevParams.SocVer;
        pdeviceCtx->mCbParams.SerNumMsm = pdeviceCtx->mDevParams.SerNumMsm;
        pdeviceCtx->mCbParams.Mtu = pdeviceCtx->mDevParams.Mtu;
        pdeviceCtx->mCbParams.Flag = pdeviceCtx->mDevParams.Flag;
        pdeviceCtx->mCbParams.Protocol = pdeviceCtx->mDevParams.Protocol;
        pdeviceCtx->mCbParams.HwId = pdeviceCtx->mDevParams.HwId;
        pdeviceCtx->mCbParams.ParentDev = pdeviceCtx->mDevParams.ParentDev;
        pdeviceCtx->mCbParams.ParentLocationInfomation = pdeviceCtx->mDevParams.ParentLocationInfomation;

        QCDEV_LOCK_MUTEX_AND_UNLOCK_ON_RETURN(&qcdevCtx.mListLock);
        if (pdeviceCtx->mDevParams.DevDetails != nullptr)
        {
            pdeviceCtx->mCbParams.DevDetails = std::make_unique<DevDetailsMap>(*(pdeviceCtx->mDevParams.DevDetails));
        }
        refineDeviceHardwarePath(pdeviceCtx);

        pdeviceCtx->mContextInt = (++qcdevCtx.mDevidx);

        if (qcdevCtx.mCallbackCtx != NULL)
        {
            pdeviceCtx->mpUserContext = qcdevCtx.mCallbackCtx;
        }

        /* Notifies with the user callback*/
        qcdevCtx.mCallback(&(pdeviceCtx->mCbParams), &pdeviceCtx->mpUserContext);
    }
}

/**
 * @brief   To process the device info on remove trigger (from process_device)
 *
 * To process the devices on removal and removes the data from main Ctx
 *
 * @param   dev      udev device info
 *
 * @returns Nothing  remove the data to main Ctx if exists
 */
static void processRemoveDevice(struct udev_device *dev)
{
    QCDEV_LOG_INFO("In\n");
    PDeviceCtx pdeviceCtx;
    const char *devpath = udev_device_get_devpath(dev); /* Always returns data */
    const char *subsystem = udev_device_get_subsystem(dev);
    if (!subsystem)
        subsystem = "NULL";

    const char* devNode = NULL;
	int isQcDriver = 1;

    /* Fetch the device details from the stored device context */
    if (!(pdeviceCtx = (PDeviceCtx)fetchFromDeviceCtx((PVOID)devpath)))
    {
        //QCDEV_LOG_ERR("Not a valid device\n");
        return;
    }

    devNode = pdeviceCtx->mDevParams.DevName;

    if (strcasestr(subsystem, SUBSYSTEM_TTY))
    {
        QCDEV_LOG_INFO("Departure: %s : %s\n", subsystem, devNode);
        pdeviceCtx->mDevParams.Flag =
            QC_DEV_TYPE_PORTS << 8 | QC_DEV_STATE_DEPARTURE << 4;
    }
    else if ((strcasestr(subsystem, SUBSYSTEM_USB)))
    {
        QCDEV_LOG_INFO("Departure: %s : %s\n", subsystem, devNode);
        pdeviceCtx->mDevParams.Flag =
            QC_DEV_TYPE_USB << 8| QC_DEV_STATE_DEPARTURE << 4 | isQcDriver;
    } else if ((strcasestr(subsystem, SUBSYSTEM_QCOM_NET)) || (strcasestr(subsystem, OLDER_SUBSYSTEM_GOBINET)))
    {
        QCDEV_LOG_INFO("Departure: %s : %s\n", subsystem, devNode);
        pdeviceCtx->mDevParams.Flag =
            QC_DEV_TYPE_NET << 8| QC_DEV_STATE_DEPARTURE << 4 | isQcDriver;
    } else if ((strcasestr(subsystem, SUBSYSTEM_QCOM_USB)) || (strcasestr(subsystem, OLDER_SUBSYSTEM_GOBIUSB)))
    {
        QCDEV_LOG_INFO("Departure: %s : %s\n", subsystem, devNode);
        pdeviceCtx->mDevParams.Flag =
            QC_DEV_TYPE_USB << 8| QC_DEV_STATE_DEPARTURE << 4 | isQcDriver;
    } else if ((strcasestr(subsystem, SUBSYSTEM_QCOM_PORTS)) || (strcasestr(subsystem, OLDER_SUBSYSTEM_GOBIPORTS)))
    {
        QCDEV_LOG_INFO("Departure: %s : %s\n", subsystem, devNode);
        pdeviceCtx->mDevParams.Flag =
            QC_DEV_TYPE_PORTS << 8| QC_DEV_STATE_DEPARTURE << 4 | isQcDriver;
    }
    else if ((strcasestr(subsystem, SUBSYSTEM_MHI) || strcasestr(subsystem, SUBSYSTEM_WWAN)) &&
             (!qcdevCtx.mSetting->DeviceClass ||
              (qcdevCtx.mSetting->DeviceClass & DEV_CLASS_MDM)))
    {
        QCDEV_LOG_INFO("Departure: %s : %s\n", subsystem, devNode);
        pdeviceCtx->mDevParams.Flag =
            QC_DEV_TYPE_PORTS << 8| QC_DEV_STATE_DEPARTURE << 4 | isQcDriver;
    } else
    {
        /* This should never happen */
        QCDEV_LOG_ERR("Invalid Subsystem : %s\n", subsystem);
        popFromDeviceCtx(pdeviceCtx->mContextInt);
        return;
    }
    
    if (qcdevCtx.mCallback)
    {
        if (qcdevCtx.mCallbackCtx != NULL)
        {
            pdeviceCtx->mpUserContext = qcdevCtx.mCallbackCtx;
        }

        pdeviceCtx->mCbParams.Flag = pdeviceCtx->mDevParams.Flag;
        if (pdeviceCtx->mDevParams.DevDetails != nullptr)
        {
            pdeviceCtx->mCbParams.DevDetails = std::make_unique<DevDetailsMap>(*(pdeviceCtx->mDevParams.DevDetails));
        }
        else
        {
            pdeviceCtx->mCbParams.DevDetails = nullptr;
        }
            /* Notifies with the user callback*/
        qcdevCtx.mCallback(&(pdeviceCtx->mCbParams), &pdeviceCtx->mpUserContext);
    }
    /* Remove the device from device context */
    popFromDeviceCtx(pdeviceCtx->mContextInt);

    QCDEV_LOG_INFO("Out\n");
    return;
}

/**
 * @brief   To process the device info on state change
 *
 * To process the devices on state change,
 * processRemoveDevice/ processAddDevice are called based on the state
 *
 * @param   dev      udev device info
 *
 * @returns Nothing  updates the data in main Ctx
 */
static void process_device(struct udev_device *dev)
{

    const char *action;

    if (!dev || !udev_device_get_devpath(dev))
    {
        return;
    }

    /* The kernel action value, usually: add, remove, change, online, offline
     * NULL in case of no change in acton of if already exists
     */
    action = udev_device_get_action(dev);
    if (!action)
        action = "exists";

    if (strcasestr(action, "add") || strcasestr(action, "exists"))
    {
        /* Process the device when it is added */
        processAddDevice(dev);
    }
    else if (strcasestr(action, "remove"))
    {
        /* Process the device when it is removed */
        processRemoveDevice(dev);
    }
    return;
}

/**
 * @brief   To get the list of available devices in Subsystem
 *
 * To process the existing devices and process_device() gets
 * if the device is found
 *
 * @param   udev      udev variable
 *
 * @returns Nothing  updates the data in main Ctx
 */
static void enumerate_devices(struct udev *udev)
{
    QCDEV_LOG_DBG("In\n");

    /*  Create an enumeration context to scan /sys. */
    struct udev_enumerate *enumerate = udev_enumerate_new(udev);

    //TODO: Need to add more Subsystems based on the requirements (QDSS etc..)
    /* watch for the subsystem USB to match the incoming devices against */
    if (udev_enumerate_add_match_subsystem(enumerate, SUBSYSTEM_USB) < 0)
    {
        QCDEV_LOG_ERR("Error: unable to apply subsystem filter '%s'\n",
                      SUBSYSTEM_USB);
        udev_enumerate_unref(enumerate);
        return;
    }
    /* watch for the subsystem 'qcom_usb' to match the incoming dev*/
    if (udev_enumerate_add_match_subsystem(enumerate, SUBSYSTEM_QCOM_USB) < 0)
    {
        QCDEV_LOG_ERR("Error: unable to apply subsystem filter '%s'\n", SUBSYSTEM_QCOM_USB);
        udev_enumerate_unref(enumerate);
        return;
    }
    /* watch for the older subsystem 'GobiUSB' to match the incoming devices against */
    if (udev_enumerate_add_match_subsystem(enumerate, OLDER_SUBSYSTEM_GOBIUSB) < 0)
    {
        QCDEV_LOG_ERR("Error: unable to apply subsystem filter '%s'\n", OLDER_SUBSYSTEM_GOBIUSB);
        udev_enumerate_unref(enumerate);
        return;
    }
    /* watch for the subsystem 'qcom_ports' to match the incoming dev*/
    if (udev_enumerate_add_match_subsystem(enumerate, SUBSYSTEM_QCOM_PORTS) < 0)
    {
        QCDEV_LOG_ERR("Error: unable to apply subsystem filter '%s'\n", SUBSYSTEM_QCOM_PORTS);
        udev_enumerate_unref(enumerate);
        return;
    }
    /* watch for the older subsystem 'GobiPorts' to match the incoming devices against */
    if (udev_enumerate_add_match_subsystem(enumerate, OLDER_SUBSYSTEM_GOBIPORTS) < 0)
    {
        QCDEV_LOG_ERR("Error: unable to apply subsystem filter '%s'\n", OLDER_SUBSYSTEM_GOBIPORTS);
        udev_enumerate_unref(enumerate);
        return;
    }
    /* watch for the subsystem qcom_usbnet to match the incoming devices against */
    if (udev_enumerate_add_match_subsystem(enumerate, SUBSYSTEM_QCOM_NET) < 0)
    {
        QCDEV_LOG_ERR("Error: unable to apply subsystem filter '%s'\n", SUBSYSTEM_QCOM_NET);
        udev_enumerate_unref(enumerate);
        return;
    }
    /* watch for the older subsystem 'GobiQMI' to match the incoming devices against */
    if (udev_enumerate_add_match_subsystem(enumerate, OLDER_SUBSYSTEM_GOBINET) < 0)
    {
        QCDEV_LOG_ERR("Error: unable to apply subsystem filter '%s'\n", OLDER_SUBSYSTEM_GOBINET);
        udev_enumerate_unref(enumerate);
        return;
    }
    /* watch for the subsystem MHI to match the incoming devices against */
    if (udev_enumerate_add_match_subsystem(enumerate, SUBSYSTEM_MHI) < 0)
    {
        QCDEV_LOG_ERR("Error: unable to apply subsystem filter '%s'\n",
                      SUBSYSTEM_MHI);
        udev_enumerate_unref(enumerate);
        return;
    }
    /* watch for the subsystem MHI to match the incoming devices against */
    if (udev_enumerate_add_match_subsystem(enumerate, SUBSYSTEM_WWAN) < 0)
    {
        QCDEV_LOG_ERR("Error: unable to apply subsystem filter '%s'\n",
                      SUBSYSTEM_WWAN);
        udev_enumerate_unref(enumerate);
        return;
    }

#ifdef ALLOW_TTY_DEVICE
    /* watch for the subsystem TTY to match the incoming devices against */
    if (udev_enumerate_add_match_subsystem(enumerate, SUBSYSTEM_TTY) < 0)
    {
        QCDEV_LOG_ERR("Error: unable to apply subsystem filter '%s'\n",
                      SUBSYSTEM_TTY);
        udev_enumerate_unref(enumerate);
        return;
    }
#endif
    /* Scan /sys for all devices which match the given filters. */
    if (udev_enumerate_scan_devices(enumerate) < 0)
    {
        QCDEV_LOG_ERR("Unable to Scan /sys for all devices from filters\n");
        udev_enumerate_unref(enumerate);
        return;
    }

    /* Get the first entry of the sorted list of device paths */
    struct udev_list_entry *devices = udev_enumerate_get_list_entry(enumerate);
    struct udev_list_entry *entry;

    udev_list_entry_foreach(entry, devices)
    {
        const char *path = udev_list_entry_get_name(entry);
        /* Create new udev device, and fill in information from the sys */
        struct udev_device *dev = udev_device_new_from_syspath(udev, path);
        /* To process the devices on arrival/ departure */
        process_device(dev);
        /* resources of the device will be released. */
        udev_device_unref(dev);
    }

    /* All resources of the enumeration context will be released. */
    udev_enumerate_unref(enumerate);
    QCDEV_LOG_DBG("Out\n");
    return;
}

/**
 * @brief   To Clear all device cntxt
 *
 * @returns Nothing
 */
static void clearDeviceCnxt(void)
{
    PDeviceCtx traversal = qcdevCtx.mpDeviceCtx;
    /* Lock a mutex and unlock it on return. */
    //QCDEV_LOCK_MUTEX_AND_UNLOCK_ON_RETURN(&qcdevCtx.mListLock);
    while (qcdevCtx.mpDeviceCtx)
    {
        traversal = qcdevCtx.mpDeviceCtx;
        qcdevCtx.mpDeviceCtx = qcdevCtx.mpDeviceCtx->mNext;
        /* Destructor of _DeviceCtx handles mDevParams.DevDetails cleanup */
        delete traversal;
    }
    return;
}

#ifdef USE_SIG_ACTION
static void signalHandler(int sigNum, siginfo_t *info, void *userData)
{
    //printf("signal Handler\n");
    return;
}
#else
static void signalHandler(int sigNum)
{
    //printf("signal Handler\n");
    return;
}
#endif

static void PushtoApplication(char *ipAddr, char* flag)
{
    QCDEV_LOG_DBG("In\n");
    PDeviceCtx pdeviceCtx;
    int isQcDriver = 1;

    pdeviceCtx = new _DeviceCtx();
    if (NULL == pdeviceCtx)
    {
        QCDEV_LOG_ERR("Failed to allocate memorey\n");
        return;
    }

    snprintf(pdeviceCtx->mDevParams.DevDesc, QCDEV_MAX_VALUE_NAME, "ADB-IP: %s", ipAddr);
    snprintf(pdeviceCtx->mDevParams.DevName, QCDEV_MAX_VALUE_NAME, "ADB-IP: %s", ipAddr);
    snprintf(qcdevCtx.mpDeviceCtx->mDevParams.SerNum, QCDEV_MAX_VALUE_NAME, "%s", ipAddr);

    pdeviceCtx->mCbParams.DevDesc = pdeviceCtx->mDevParams.DevDesc;
    pdeviceCtx->mCbParams.DevName = pdeviceCtx->mDevParams.DevName;
    pdeviceCtx->mCbParams.Loc = qcdevCtx.mpDeviceCtx->mDevParams.Loc;
    pdeviceCtx->mCbParams.DevPath = qcdevCtx.mpDeviceCtx->mDevParams.DevPath;
    pdeviceCtx->mCbParams.SerNum = qcdevCtx.mpDeviceCtx->mDevParams.SerNum;
    pdeviceCtx->mCbParams.SocVer = qcdevCtx.mpDeviceCtx->mDevParams.SocVer;
    pdeviceCtx->mCbParams.SerNumMsm = qcdevCtx.mpDeviceCtx->mDevParams.SerNumMsm;
    pdeviceCtx->mCbParams.Mtu = qcdevCtx.mpDeviceCtx->mDevParams.Mtu;
    if (strncmp(flag,"UP",strlen("UP")+1) == 0) {
    pdeviceCtx->mCbParams.Flag = ((ULONG)QC_DEV_TYPE_PORTS << 8) | (QC_DEV_STATE_ARRIVAL << 4) | ((ULONG)QC_DEV_BUS_TYPE_PCIE << 16) | isQcDriver;
    }
    else if (strncmp(flag,"DOWN",strlen("DOWN")+1) == 0) {
    pdeviceCtx->mCbParams.Flag = ((ULONG)QC_DEV_TYPE_PORTS << 8) | (QC_DEV_STATE_DEPARTURE << 4) | isQcDriver;
    }
    pdeviceCtx->mCbParams.Protocol = qcdevCtx.mpDeviceCtx->mDevParams.Protocol;
    pdeviceCtx->mCbParams.HwId = qcdevCtx.mpDeviceCtx->mDevParams.HwId;
    pdeviceCtx->mCbParams.ParentDev = qcdevCtx.mpDeviceCtx->mDevParams.ParentDev;
    pdeviceCtx->mCbParams.ParentLocationInfomation = qcdevCtx.mpDeviceCtx->mDevParams.ParentLocationInfomation;
    if (pdeviceCtx->mDevParams.DevDetails != nullptr)
    {
        pdeviceCtx->mCbParams.DevDetails = std::make_unique<DevDetailsMap>(*(pdeviceCtx->mDevParams.DevDetails));
    }
    else
    {
        pdeviceCtx->mCbParams.DevDetails = nullptr;
    }

    refineDeviceHardwarePath(pdeviceCtx);

    pdeviceCtx->mContextInt = (++qcdevCtx.mDevidx);

    if (qcdevCtx.mCallbackCtx != NULL)
    {
        pdeviceCtx->mpUserContext = qcdevCtx.mCallbackCtx;
    }
    /* Notifies with the user callback*/
    qcdevCtx.mCallback(&(pdeviceCtx->mCbParams), &pdeviceCtx->mpUserContext);

    delete pdeviceCtx;
    QCDEV_LOG_DBG("Out\n");
}

void PushLibUSBToApplication(PQcomDeviceInfo devinfo)
{
    QCDEV_LOG_DBG("In\n");
    PDeviceCtx pdeviceCtx;
    int isQcDriver = 1;

    if (!IsDeviceAllowed(devinfo->protocol))
    {
        QCDEV_LOG_INFO("PushLibUSBToApplication: filtered protocol 0x%lX by DeviceClass/DeviceProtocol\n",
                       devinfo->protocol);
        return;
    }

    pdeviceCtx = new _DeviceCtx();
    if (NULL == pdeviceCtx)
    {
        QCDEV_LOG_ERR("Failed to allocate memorey\n");
        return;
    }

    pdeviceCtx->mCbParams.DevDesc = devinfo->DevDesc;
    pdeviceCtx->mCbParams.DevName = devinfo->DevName;
    pdeviceCtx->mCbParams.DevPath = devinfo->DevPath;
    pdeviceCtx->mCbParams.Protocol = devinfo->protocol;
    pdeviceCtx->mCbParams.Loc = devinfo->Loc;
    pdeviceCtx->mCbParams.SerNum = devinfo->SerNum;
    pdeviceCtx->mCbParams.SocVer = devinfo->SocVer;
    pdeviceCtx->mCbParams.SerNumMsm = devinfo->SerNumMSM;
    pdeviceCtx->mCbParams.Flag = devinfo->Flag;
    pdeviceCtx->mCbParams.HwId = devinfo->HwId;
    pdeviceCtx->mCbParams.ParentDev = devinfo->ParentDev;
    pdeviceCtx->mCbParams.ParentLocationInfomation = devinfo->ParentLocationInformation;
    if (devinfo->DevDetailsMP != nullptr)
    {
        pdeviceCtx->mCbParams.DevDetails = std::make_unique<DevDetailsMap>(*(devinfo->DevDetailsMP));
    }
    else
    {
        pdeviceCtx->mCbParams.DevDetails = nullptr;
    }

    if (qcdevCtx.mCallbackCtx != NULL)
    {
        pdeviceCtx->mpUserContext = qcdevCtx.mCallbackCtx;
    }
    /* Notifies with the user callback*/
    qcdevCtx.mCallback(&(pdeviceCtx->mCbParams), &pdeviceCtx->mpUserContext);

    delete pdeviceCtx;
    return;
}

void ScanExistingInterface(int sock) {
    
    char buffer[MAX_BUFFER_SIZE];
    struct sockaddr_nl sa;
    struct nlmsghdr *nlh;

    memset(&sa, 0, sizeof(sa));
    sa.nl_family = AF_NETLINK;
    sa.nl_groups = RTMGRP_LINK | RTMGRP_IPV4_IFADDR;

    // Send a request to get the list of existing interfaces
    nlh = (struct nlmsghdr *)buffer;
    nlh->nlmsg_len = NLMSG_LENGTH(sizeof(struct rtgenmsg));
    nlh->nlmsg_type = RTM_GETLINK;
    nlh->nlmsg_flags = NLM_F_REQUEST | NLM_F_DUMP;
    nlh->nlmsg_seq = 1;
    nlh->nlmsg_pid = getpid();

    // Send the request
    if (sendto(sock, nlh, nlh->nlmsg_len, 0, (struct sockaddr *)&sa, sizeof(sa)) < 0) {
        QCDEV_LOG_ERR("Error: sendto failed\n");
        //close(sock);
        return; 
    }
    else {
        QCDEV_LOG_INFO("Successfull sendto:RTM_GETLINK\n");
    }

}

/**
 * @brief   To Monitor on the state of devices
 *
 * Continously polls on the subsystem selected and update the
 * corresponding info (called from the thread)
 *
 * @param   userData    udev info
 *
 * @returns Nothing
 */
static void *monitor_devices(void *userData)
{
    QCDEV_LOG_DBG("In\n");

    int fd_ep = -1, fd_udev = -1;
    int fd_net = -1;
    QCDEV_LOG_INFO("======= QDS version: %s =========\n",DEV_DIS_LIB_VERSION);
    struct udev *udev = (struct udev *)userData;

    //TODO: For the time calculation
    clock_t t;
    t = clock();
    /* Load EUD library and start the USB monitor before the initial scan */
    QcEud_LoadLib();
    QcEud_StartMonitor(EudDeviceChangeCb);

    /* To list all the available devices and update the device ctx*/
    enumerate_devices(udev);
    /* Promote any EUD devices that arrived before their USB parent */
    ProcessPendingEudArrivals();

    if (IsQcomLibusbEnable()) {
        register_libusb_hotplug_callback(&qcdevCtx);
        QCDEV_LOG_INFO("libusb hotplug callback registered\n");
        start_arrival_worker();
        QCDEV_LOG_INFO("arrival worker started\n");
        g_libusb_event_running = true;
        int rc = pthread_create(&g_libusb_event_thread, NULL, libusb_event_thread_fn, get_libusb_context());
        if (rc != 0) {
            QCDEV_LOG_ERR("Failed to create libusb event thread: %d\n", rc);
            g_libusb_event_running = false;
            stop_arrival_worker();
            unregister_libusb_hotplug_callback();
        } else {
            QCDEV_LOG_INFO("libusb event thread started\n");
        }
    }
    t = clock() - t;
    double time_taken = ((double)t) / CLOCKS_PER_SEC;
    QCDEV_LOG_DBG("Took %f, seconds to discover \n", time_taken);

    /* subscribing to uevents and to create a netlink socket */
    struct udev_monitor *mon =
        udev_monitor_new_from_netlink(udev, QCDEV_UDEV_MONITOR_UDEV);
    if (mon == NULL)
    {
        QCDEV_LOG_ERR("Error: unable to create netlink socket\n");
        goto fail;
    }

    udev_monitor_set_receive_buffer_size(mon, 128 * 1024 * 1024);

    /* Retrieve the socket file descriptor associated with the monitor. */
    fd_udev = udev_monitor_get_fd(mon);
    qcdevCtx.mon = mon;
    //TODO: Need to add more Subsystems based on the requirements (QDSS etc..)
    /* watch for the subsystem USB to match the incoming devices against */
    if (udev_monitor_filter_add_match_subsystem_devtype(mon, SUBSYSTEM_USB, NULL) < 0)
    {
        QCDEV_LOG_ERR("Error: unable to apply subsystem filter '%s'\n", SUBSYSTEM_USB);
        goto fail;
    }
    /* watch for the subsystem "qcom_ports" to match the incoming devices*/
    if (udev_monitor_filter_add_match_subsystem_devtype(mon, SUBSYSTEM_QCOM_PORTS, NULL) < 0)
    {
        QCDEV_LOG_ERR("Error: unable to apply subsystem filter '%s'\n", SUBSYSTEM_QCOM_PORTS);
        goto fail;
    }
    /* watch for the older subsystem "GobiPorts" to match the incoming devices*/
    if (udev_monitor_filter_add_match_subsystem_devtype(mon, OLDER_SUBSYSTEM_GOBIPORTS, NULL) < 0)
    {
        QCDEV_LOG_ERR("Error: unable to apply subsystem filter '%s'\n", OLDER_SUBSYSTEM_GOBIPORTS);
        goto fail;
    }
    /* watch for the subsystem "qcom_usb" to match the incoming devices*/
    if (udev_monitor_filter_add_match_subsystem_devtype(mon, SUBSYSTEM_QCOM_USB, NULL) < 0)
    {
        QCDEV_LOG_ERR("Error: unable to apply subsystem filter '%s'\n", SUBSYSTEM_QCOM_USB);
        goto fail;
    }
    /* watch for the older subsystem "GobiUSB" to match the incoming devices*/
    if (udev_monitor_filter_add_match_subsystem_devtype(mon, OLDER_SUBSYSTEM_GOBIUSB, NULL) < 0)
    {
        QCDEV_LOG_ERR("Error: unable to apply subsystem filter '%s'\n", OLDER_SUBSYSTEM_GOBIUSB);
        goto fail;
    }
    /* watch for the subsystem 'qcom_net' to match the incoming devices against */
    if (udev_monitor_filter_add_match_subsystem_devtype(mon, SUBSYSTEM_QCOM_NET, NULL) < 0)
    {
        QCDEV_LOG_ERR("Error: unable to apply subsystem filter '%s'\n", SUBSYSTEM_QCOM_NET);
        goto fail;
    }
    /* watch for the older subsystem "GobiQMI" to match the incoming devices*/
    if (udev_monitor_filter_add_match_subsystem_devtype(mon, OLDER_SUBSYSTEM_GOBINET, NULL) < 0)
    {
        QCDEV_LOG_ERR("Error: unable to apply subsystem filter '%s'\n", OLDER_SUBSYSTEM_GOBINET);
        goto fail;
    }
    /* watch for the subsystem MHI to match the incoming devices against */
    if (udev_monitor_filter_add_match_subsystem_devtype(mon, SUBSYSTEM_MHI, NULL) < 0)
    {
        QCDEV_LOG_ERR("Error: unable to apply subsystem filter '%s'\n", SUBSYSTEM_MHI);
        goto fail;
    }
    /* watch for the subsystem MHI to match the incoming devices against */
    if (udev_monitor_filter_add_match_subsystem_devtype(mon, SUBSYSTEM_WWAN, NULL) < 0)
    {
        QCDEV_LOG_ERR("Error: unable to apply subsystem filter '%s'\n", SUBSYSTEM_WWAN);
        goto fail;
    }

#ifdef ALLOW_TTY_DEVICE
    /* watch for the subsystem TTY to match the incoming devices against */
    if (udev_monitor_filter_add_match_subsystem_devtype(mon, SUBSYSTEM_TTY, NULL) < 0)
    {
        QCDEV_LOG_ERR("Error: unable to apply subsystem filter '%s'\n", SUBSYSTEM_TTY);
        goto fail;
    }
#endif

    /* Binds the @udev_monitor socket to the event source */
    if (udev_monitor_enable_receiving(mon) < 0)
    {
        QCDEV_LOG_ERR("Error: unable to enable receiving\n");
        goto fail;
    }

    qcdevCtx.mMonitorFd[0] = fd_udev;

    fd_net = socket(AF_NETLINK, SOCK_RAW, NETLINK_ROUTE);   // create netlink socket

    if (fd_net < 0) {
        QCDEV_LOG_ERR("Failed to create netlink socket: %s\n", (char*)strerror(errno));
        return;
    }

    qcdevCtx.mMonitorFd[1] = fd_net;

    struct sockaddr_nl  local;  // local addr struct
    
    memset(&local, 0, sizeof(local));

    local.nl_family = AF_NETLINK;       // set protocol family
    local.nl_groups =   RTMGRP_LINK | RTMGRP_IPV4_IFADDR;   // set/subscribe groups we interested in
    local.nl_pid = getpid();    // set out id using current process id

    if (bind(fd_net, (struct sockaddr*)&local, sizeof(local)) < 0) {     // bind socket
        QCDEV_LOG_ERR("Failed to bind netlink socket: %s\n", (char*)strerror(errno));
        close(fd_net);
        return 1;
    }

    struct ifaddrs ip;
    char buf[MAX_BUFFER_SIZE];             // message buffer
    struct iovec iov;           // message structure
    iov.iov_base = buf;         // set message buffer as io
    iov.iov_len = sizeof(buf);  // set size   
    char ifAddress[QCDEV_MAX_VALUE_NAME];  // network addr

    // initialize protocol message header
    struct msghdr msg;
    {
        msg.msg_name = &local;                  // local address
        msg.msg_namelen = sizeof(local);        // address size
        msg.msg_iov = &iov;                     // io vector
        msg.msg_iovlen = 1;                     // io size
    }

    // Scan existing interfaces
    ScanExistingInterface(fd_net);

#ifdef USE_SIG_ACTION
    struct sigaction sigAction;
    sigset_t sigmask;

    sigemptyset(&sigmask);

    memset(&sigAction, 0, sizeof(sigAction));

    sigAction.sa_handler = signalHandler;
    sigAction.sa_flags = SA_SIGINFO;
    sigfillset(&sigAction.sa_mask);

    /* To handle the SIGUSR1 signal */
    if (sigaction(SIGUSR1, &sigAction, NULL))
    {
        QCDEV_LOG_ERR("Failed to set signal handler\n");
        goto fail;
    }
#else
    signal(SIGUSR1, signalHandler);
#endif

    while (qcdevCtx.mMonitorFd[0])
    {
        fd_set fds;
        FD_ZERO(&fds);
        FD_SET(fd_udev, &fds);
		FD_SET(fd_net, &fds);
        int ret = select(fd_udev + 10, &fds, NULL, NULL, NULL);
        if (ret <= 0)
        {
            if (errno != EINTR)
                QCDEV_LOG_ERR("error receiving uevent message \n");
            continue;
        }

        if (FD_ISSET(fd_net, &fds))  
        {  
            ssize_t status = recvmsg(fd_net, &msg, MSG_DONTWAIT);
            if (status < 0) {
                if (errno == EINTR || errno == EAGAIN)
                {
                    usleep(250000);
                    continue;
                }

            QCDEV_LOG_INFO("Failed to read netlink: %s\n", (char*)strerror(errno));
            continue;
            }

            if (msg.msg_namelen != sizeof(local)) { // check message length, just in case
                QCDEV_LOG_INFO("Invalid length of the sender address struct\n");
                continue;
            }

            // message parser
            struct nlmsghdr *nlh;

            for (nlh = (struct nlmsghdr*)buf; status >= (ssize_t)sizeof(*nlh); ) {   // read all messagess headers
                int len = nlh->nlmsg_len;
                int l = len - sizeof(*nlh);
                char *ifName;

                if ((l < 0) || (len > status)) {
                    QCDEV_LOG_ERR("Invalid message length: %i\n", len);
                    continue;
                }
                // now we can check message type
                if (nlh->nlmsg_type == RTM_NEWLINK) {
                    QCDEV_LOG_DATA("message: RTM_NEWLINK\n");

                    char *ifUpp;
                    char *ifRunn;
                    struct ifinfomsg *ifi;  // structure for network interface info
                    struct rtattr *tb[IFLA_MAX + 1];

                    ifi = (struct ifinfomsg*) NLMSG_DATA(nlh);    // get information about changed network interface

			    	parseRtattr(tb, IFLA_MAX, IFLA_RTA(ifi), nlh->nlmsg_len);  // get attributes

                    if (tb[IFLA_IFNAME]) {  // validation
                        ifName = (char*)RTA_DATA(tb[IFLA_IFNAME]); // get network interface name
                        QCDEV_LOG_DATA("interface Name: %s \n",ifName); 
                    }

                    if (ifi->ifi_flags & IFF_UP) { // get UP flag of the network interface
                        ifUpp = (char*)"UP";
                    } else {
                        ifUpp = (char*)"DOWN";
                    }

                    if (ifi->ifi_flags & IFF_RUNNING) { // get RUNNING flag of the network interface
                        ifRunn = (char*)"RUNNING";
                    } else {
                        ifRunn = (char*)"NOT RUNNING";
                    }
    
                if (strncmp(ifName,"mhi_swip0",strlen("mhi_swip0")+1) == 0 ) {

                        if (strncmp(ifUpp,"UP",strlen("UP")+1) == 0) {
                            memset(ifAddress,0,QCDEV_MAX_VALUE_NAME*sizeof(ifAddress[0]));
                            fetchIPAddr(ifAddress);
                            ifAddress[QCDEV_MAX_VALUE_NAME-1] = '\0';
                            PushtoApplication(ifAddress, ifUpp);
                            QCDEV_LOG_INFO("%s interface is UP: %s \n",ifName, ifAddress);
                        }
                        else if(strncmp(ifUpp,"DOWN",strlen("DOWN")+1) == 0) {
                            PushtoApplication(ifAddress, ifUpp);
                            QCDEV_LOG_INFO("%s interface is DOWN: %s \n",ifName, ifAddress);
                        } 
                        else
                        {
                            QCDEV_LOG_INFO("Link index: %d Flags: (0x%x) interface: %s\n", ifi->ifi_index, ifi->ifi_flags, ifName);
                        }

                        QCDEV_LOG_INFO("New network interface %s, state: %s %s\n",ifName, ifUpp, ifRunn);
                    }
                }
                if (nlh->nlmsg_type == RTM_DELLINK) {
                    QCDEV_LOG_DATA("message: RTM_DELLINK\n");
                }
                if (nlh->nlmsg_type == RTM_NEWADDR) {
                    QCDEV_LOG_DATA("message: RTM_NEWADDR\n");
                }
                if (nlh->nlmsg_type == RTM_DELADDR) {
                    QCDEV_LOG_DATA("message: RTM_DELADDR\n");
                }
                if (nlh->nlmsg_type == RTM_NEWROUTE) {
                    QCDEV_LOG_DATA("message: RTM_NEWROUTE\n");
                }
                if (nlh->nlmsg_type == RTM_DELROUTE) {
                    QCDEV_LOG_DATA("message: RTM_DELROUTE\n");
                }
                status -= NLMSG_ALIGN(len); // align offsets by the message length, this is important
                nlh = (struct nlmsghdr*)((char*)nlh + NLMSG_ALIGN(len));    // get next message
            }
        }

        if (FD_ISSET(fd_udev, &fds))
        {
            /* Receive data from the udev monitor socket, allocate a new
             * udev device, fill in the received data, and return the dev
             */
            struct udev_device *dev = udev_monitor_receive_device(mon);
            /* To process the devices on arrival/ departure */
            process_device(dev);
            /* Drop a reference of a udev device. If the refcount reaches
             * zero, the resources of the device will be released.
             */
            udev_device_unref(dev);
            /* Promote any EUD devices that were waiting for their USB parent */
            ProcessPendingEudArrivals();
        }
    }

fail:
    /* Clear the entire device context */
    clearDeviceCnxt();
    /* Drop a reference of a udev monitor, bound socket will be closed */
    if (mon)
    {
        udev_monitor_unref(mon);
    }
    /* Drop a reference of the udev library context,
     * the resources of the context will be released
     */
    if (udev)
    {
        udev_unref(udev);
    }

    /* To clear stored values */
    qcdevCtx.mCallback = NULL;
    qcdevCtx.mSetting = NULL;
    qcdevCtx.mUdev = NULL;

    pthread_exit(NULL);
    QCDEV_LOG_DBG("Out\n");
}

/**
 * @brief   To store the DeviceChangeCallback in main Ctx
 *
 * stores the device callback info in the main Ctx, and gets
 * triggerd on status change on the registered callback
 *
 * @param   Cb    device change callback
 *
 * @returns Nothing
 */
VOID QcDevice::SetDeviceChangeCallback(DEVICECHANGE_CALLBACK_N Cb)
{
    QCDEV_LOG_DBG("In\n");
    /* Lock a mutex and unlock it on return. */
    QCDEV_LOCK_MUTEX_AND_UNLOCK_ON_RETURN(&qcdevCtx.mListLock);

    /* Store the callback function */
    qcdevCtx.mGenericDevCB = Cb;
    return;
}

/**
 * @brief   To store the DeviceChangeCallback in main Ctx
 *
 * stores the device callback info in the main Ctx, and gets
 * triggerd on status change on the registered callback
 *
 * @param   Cb            device change callback
 * @param   AppContext    app context
 *
 * @returns Nothing
 */
VOID QcDevice::SetDeviceChangeCallback(DEVICECHANGE_CALLBACK Cb, PVOID AppContext)
{
    QCDEV_LOG_INFO("In\n");
    /* Lock a mutex and unlock it on return. */
    QCDEV_LOCK_MUTEX_AND_UNLOCK_ON_RETURN(&qcdevCtx.mListLock);

    /* Store the callback function */
    qcdevCtx.mCallback = Cb;
    qcdevCtx.mCallbackCtx = AppContext;

#ifdef ALPACA
    AlpacaDiscoveryCallback(Cb, AppContext);
#endif
    return;
}

/**
 * @brief   To update the features in main Ctx
 *
 * To update the device features in main ctx
 *
 * @param   Features    Features to store
 *
 * @returns Nothing
 */
VOID QcDevice::SetFeature(PVOID Features)
{
    QCDEV_LOG_INFO("In\n");
    /* Lock a mutex and unlock it on return. */
    QCDEV_LOCK_MUTEX_AND_UNLOCK_ON_RETURN(&qcdevCtx.mListLock);
    /* Store the Feature settings */
    qcdevCtx.mSetting = new DEV_FEATURE_SETTING();
    PDEV_FEATURE_SETTING FeaturesRef = (PDEV_FEATURE_SETTING)Features;
    qcdevCtx.mSetting->DeviceClass    = FeaturesRef->DeviceClass;
    qcdevCtx.mSetting->Settings       = FeaturesRef->Settings;
    qcdevCtx.mSetting->VID            = FeaturesRef->VID;
    qcdevCtx.mSetting->Version        = FeaturesRef->Version;
    qcdevCtx.mSetting->DeviceProtocol = FeaturesRef->DeviceProtocol;
    //qcdevCtx.mSetting = (PDEV_FEATURE_SETTING) Features;
    return;
}

/**
 * @brief   To start the device monitor
 *
 * To initiate the device monitor on the respective Subsystems
 *
 * @returns Nothing
 */
VOID QcDevice::StartDeviceMonitor(VOID)
{
    struct udev *udev;
    pthread_t thId;
    QCDEV_LOG_DBG("In\n");

    /* Lock a mutex and unlock it on return. */
    QCDEV_LOCK_MUTEX_AND_UNLOCK_ON_RETURN(&qcdevCtx.mListLock);
    /* Assuming Start device monitor willbe called only once*/
    if (qcdevCtx.mUdev != NULL)
    {
        QCDEV_LOG_ERR("Start Monitor is already initialized\n");
        return;
    }

    /* Create an udev instance */
    udev = udev_new();
    if (!udev)
    {
        QCDEV_LOG_ERR("udev_new() failed\n");
        return;
    }
    /* Store in global Cntxt */
    qcdevCtx.mUdev = udev;

    if (IsEudLibPresent()) {
        QCDEV_LOG_INFO("EUD library detected at '%s' - skipping our libusb load/init/exist\n",
                       GetEudLibPath());
    } else if (!load_libusb()) {
        QCDEV_LOG_INFO("Failed to load libusb library. Hence, libusb won't be initialized.\n");
    } else {
        initialize_libusb();
    }

    /* Unlock mutex, as mutex acquired as part of push to context in
     * enumerate_devices
     */
    pthread_mutex_unlock(&qcdevCtx.mListLock);

    /* Thread to monitor for the devices */
    if (pthread_create(&thId, NULL, monitor_devices, udev))
    {
        QCDEV_LOG_ERR("Failed to monitor_devices\n");
    }
    qcdevCtx.mThId = thId;
    QCDEV_LOG_INFO("Out\n");
    return;
}

/**
 * @brief   To stop the device monitor
 *
 * To stop the device monitor on the respective Subsystems
 *
 * @returns Nothing
 */
QCDEVLIB_API VOID QcDevice::StopDeviceMonitor(VOID)
{
    QCDEV_LOG_INFO("In\n");
    /* Lock a mutex and unlock it on return. */
    QCDEV_LOCK_MUTEX_AND_UNLOCK_ON_RETURN(&qcdevCtx.mListLock);
    if (qcdevCtx.mUdev == NULL)
    {
        QCDEV_LOG_ERR("Start Monitor is not yet initialized\n");
        return;
    }

    if (qcdevCtx.mMonitorFd[0] > 0)
    {
        /* To stop the monitor_devices loop */
        shutdown(qcdevCtx.mMonitorFd[0], SHUT_RDWR);
        qcdevCtx.mMonitorFd[0] = 0;
    }

    if (qcdevCtx.mMonitorFd[1] > 0) 
    {
        close(qcdevCtx.mMonitorFd[1]);  // close socket
        qcdevCtx.mMonitorFd[1] = 0;
    }

    pthread_cancel(qcdevCtx.mThId);
    pthread_join(qcdevCtx.mThId, NULL);

    /* Stop EUD monitor and clean up */
    QcEud_StopMonitor();
    QcEud_Cleanup();

    /* Clear any remaining pending EUD items */
    pthread_mutex_lock(&g_eudPendingLock);
    while (g_eudPendingHead)
    {
        EudPendingItem *tmp = g_eudPendingHead;
        g_eudPendingHead = g_eudPendingHead->next;
        delete tmp;
    }
    pthread_mutex_unlock(&g_eudPendingLock);

    if (IsQcomLibusbEnable()) {
        if (g_libusb_event_running) {
            g_libusb_event_running = false;
            pthread_join(g_libusb_event_thread, NULL);
            QCDEV_LOG_INFO("libusb event thread stopped\n");
            stop_arrival_worker();
            QCDEV_LOG_INFO("arrival worker stopped\n");
            unregister_libusb_hotplug_callback();
        }
    }

    /* To clear stored values */
    clearDeviceCnxt();

    if (is_libusb_loaded()) {
        deinitialize_libusb();
    }

    qcdevCtx.mpDeviceCtx = NULL;
    qcdevCtx.mCallback = NULL;
    if (qcdevCtx.mSetting)
    {
        delete qcdevCtx.mSetting;
    }
    qcdevCtx.mSetting = NULL;
    if (qcdevCtx.mon)
    {
        udev_monitor_unref(qcdevCtx.mon);
    }
    if (qcdevCtx.mUdev)
    {
        udev_unref(qcdevCtx.mUdev);
    }
    qcdevCtx.mUdev = NULL;
    qcdevCtx.mThId = NULL;
    /* To avoid deadlock, not required in case of pthread_exit(),
     * as it clears the local variables and waits for thread exit
     * but required in case of pthread_join() !!
     */
    pthread_mutex_unlock(&qcdevCtx.mListLock);
    //pthread_join(qcdevCtx.mThId, NULL);
    //pthread_exit(NULL);
    #ifdef ENABLELOCALLOGGING
    if(log_level > 0 and !fp){
        QCDEV_LOG_DBG("Closing log file\n");
        fclose(fp);
    }
    #endif
    QCDEV_LOG_INFO("Out\n");
    return;
}
#ifdef ENABLELOCALLOGGING
#ifdef QCDEV_LOGGING

/**
 * Read debugFS log_level and update the same for library
 *
 * @param   file			Path of the file to be read
 *
 * @returns                 Nothing
 */
QCDEVLIB_API void QcDevice::update_loglvl_from_fs(char* file)
{
    FILE* debug_filefd = fopen (file, "r");
    if(!debug_filefd){
        QCDEV_LOG_ERR("Failed to open file\n");
        return;
    }
    int i = 0;
    fscanf (debug_filefd, "%d", &i);
    log_level = i;
    fclose (debug_filefd); 
    pthread_cond_signal(&cond1);
}

/**
 * @brief   To read the debug level from sysfs
 *
 * @param		int		log_level
 *
 * @returns				Nothing
 */

QCDEVLIB_API void QcDevice::libqcdev_read_loglvl()
{
    QCDEV_LOG_DBG("In\n");
    //Add code to read from FS and update the debug_lvl with some logic.
    // Make sure to call this function via a thread which checks on every 5 seconds.
    int length, i, wd;
    int fd;
    char buffer[BUF_LEN];

	update_loglvl_from_fs((char*)DEBUG_F);

    /* Initialize Inotify*/
    fd = inotify_init();
    if ( fd < 0 ) {
		QCDEV_LOG_ERR("Couldn't initialize inotify\n");
		return;
    }

    /* add watch to starting directory */
    wd = inotify_add_watch(fd, PATH , IN_CREATE | IN_MODIFY | IN_DELETE);
    if (wd == -1) {
		QCDEV_LOG_ERR("Couldn't add watch to \n");
		goto err_watch;
    }

    QCDEV_LOG_DBG("Watching %s for create|modify|delete events.\n", PATH);

    while(1) {
      i = 0;
      length = read( fd, buffer, BUF_LEN );
      if ( length < 0 ) {
		QCDEV_LOG_ERR( "read failed from inotify\n" );
		goto err_read;
      }

      while ( i < length ) {
        struct inotify_event *event = (struct inotify_event *)&buffer[i];
        if (event->len) {
          if (event->mask & IN_CREATE) {
            if (event->mask & IN_ISDIR) {
				QCDEV_LOG_DBG("The directory %s was Created.\n", event->name);
			}
            else
				QCDEV_LOG_DBG("The file %s was Created with WD %d\n", event->name, event->wd);
          }

          if (event->mask & IN_MODIFY) {
            if (event->mask & IN_ISDIR) {
				QCDEV_LOG_DBG("The directory %s was modified.\n", event->name);
			}
            else
				QCDEV_LOG_DBG("The file %s was modified with WD %d\n", event->name, event->wd);
          }

		  /* Checking only for "debug" file at PATH */
		  if (((event->mask & IN_MODIFY) || (event->mask & IN_CREATE)) && !strncmp(event->name, "debug", LEN_NAME))
				update_loglvl_from_fs((char*)DEBUG_F);

          if ( event->mask & IN_DELETE) {
            if (event->mask & IN_ISDIR) {
				QCDEV_LOG_DBG("The directory %s was deleted.\n", event->name);
			}
            else
				QCDEV_LOG_DBG("The file %s was deleted with WD %d\n", event->name, event->wd);
          }
		}
		i += EVENT_SIZE + event->len;
	  }
     
	}

err_read:
	inotify_rm_watch( fd, wd );


err_watch:
	close( fd );
    

}
#endif

QCDEVLIB_API void QcDevice::logFileCreation(){
    if(log_level > 0){
    struct stat dir = {0};
    if(stat("/opt/QTI/DD", &dir) == -1)
    {
        int check = mkdir("/opt/QTI/DD", S_IRWXU | S_IRWXU | S_IRWXU);
        // check if directory is created or not
        if (check) 
        {
            QCDEV_LOG_ERR("Error: %s\n",strerror(errno));
            exit(1);
        }
    }
    if(stat("/opt/QTI/DD/Logs", &dir) == -1)
    {
        int check = mkdir("/opt/QTI/DD/Logs", S_IRWXU | S_IRWXU | S_IRWXU);
        if (check)
        {
            QCDEV_LOG_ERR("Error: %s\n",strerror(errno));
            exit(1);
        }
    }
    char file_name[1000];
    int ret = snprintf(file_name, sizeof(file_name), "/opt/QTI/DD/Logs/Logs-%i.txt", getpid());
    file_name[999] = '\0';
    if (ret < 0) {
        QCDEV_LOG_ERR("Error: buffer too small for logging\n");
        return;
    }
    fp  = fopen (file_name, "a+");
    if(!fp){
        QCDEV_LOG_ERR("Failed to open %s\n",file_name);
        return;
    }
}
}
#endif
QCDEVLIB_API BOOL QcDevice::initializeEudTrace(VOID)
{
    QCDEV_LOCK_MUTEX_AND_UNLOCK_ON_RETURN(&qcdevCtx.mListLock);

    if (!QcEud_Initialize())
    {
        QCDEV_LOG_ERR("initializeEudTrace: QcEud_Initialize failed\n");
        return FALSE;
    }
    return TRUE;
}

QCDEVLIB_API BOOL QcDevice::startAmbientMode(VOID)
{
    EUD_ERR_t err = QcEud_SpoofUsbDetach();
    if (err == EUD_SUCCESS)
    {
        QCDEV_LOG_INFO("startAmbientMode: SpoofUsbDetach succeeded\n");
    }
    else
    {
        QCDEV_LOG_ERR("startAmbientMode: SpoofUsbDetach failed (err=%d)\n", err);
    }
    return (err == EUD_SUCCESS) ? TRUE : FALSE;
}

QCDEVLIB_API BOOL QcDevice::stopAmbientMode(VOID)
{
    EUD_ERR_t err = QcEud_SpoofUsbAttach();
    if (err == EUD_SUCCESS)
    {
        QCDEV_LOG_INFO("stopAmbientMode: SpoofUsbAttach succeeded\n");
    }
    else
    {
        QCDEV_LOG_ERR("stopAmbientMode: SpoofUsbAttach failed (err=%d)\n", err);
    }
    return (err == EUD_SUCCESS) ? TRUE : FALSE;
}

QCDEVLIB_API VOID QcDevice::SetLoggingCallback(QCD_LOGGING_CALLBACK Cb)
{
    setLoggerCallback(Cb);
#ifdef ALPACA
    setAlpacaLoggingCallback((AlpacaQuts::QCD_LOGGING_CALLBACK)Cb);
#endif
}

/**
* @brief   To set the Logging Level in main Ctx
*
* stores the logging level info in the main Ctx
*
* @param   int	log_level
*
* @returns Nothing
*/
QCDEVLIB_API VOID QcDevice::SetLoggingLevel(int level)
{
    QCDEV_LOCK_MUTEX_AND_UNLOCK_ON_RETURN(&qcdevCtx.mListLock);
    QCDEV_LOG_DBG("In\n");
    // Just commenting for debugging. Do uncomment later
    // log_level = level;
	QCDEV_LOG_DBG("Updated log_level to %d\n", log_level);

    return;
}

/**
 * @brief   To get the device list
 *
 * To retieve all the scanned devices
 *
 * @param   Buffer      Buffer which is to be filled
 * @param   BufferSize  Size of the buffer
 * @param   ActualSize  Size of buffer which is filled with dev data
 *
 * @returns count       Number of elements filled in buffer
 */
QCDEVLIB_API ULONG QcDevice::GetDeviceList(PVOID Buffer, ULONG BufferSize, PULONG ActualSize)
{
    QCDEV_LOG_INFO("Inside\n");
    if (Buffer == NULL || BufferSize == 0 || ActualSize == NULL)
    {
        return 0;
    }
    QCDEV_LOCK_MUTEX_AND_UNLOCK_ON_RETURN(&qcdevCtx.mListLock);
    if (qcdevCtx.mUdev == NULL)
    {
        QCDEV_LOG_ERR("Start Monitor is not yet initialized\n");
        return 0;
    }

    PDeviceCtx traversal = qcdevCtx.mpDeviceCtx;
    ULONG count = 0;
    *ActualSize = 0;
    while (traversal && BufferSize > *ActualSize)
    {
        *ActualSize += snprintf(Buffer + *ActualSize,
                                BufferSize - *ActualSize - 1,
                                "%s%c", traversal->mDevParams.DevDesc, '\0');
        count++;
        traversal = traversal->mNext;
    }
    return count;
}

#define INVALID_HANDLE_VALUE -1

/**
 * @brief   To open the selected device
 *
 * Opens the device with O_NONBLOCK flag and returns the device handle
 * For MHI usecase, read() returns immediately if no data is available
 * (with -EAGAIN only if .read routine implementation honors the
 * O_NONBLOCK flag)
 * 
 * @param   DeviceName  Device which is to be opened
 *
 * @returns HANDLE      on success returns device handle
 */
HANDLE QcDevice::OpenDevice_(PVOID DeviceName)
{
    QCDEV_LOG_INFO("Open DeviceName:%s\n",(PCHAR)DeviceName);

    LONG appHdl = INVALID_HANDLE_VALUE;

    if (DeviceName == NULL)
    {
        QCDEV_LOG_ERR("Invalid arguements\n");
        return INVALID_HANDLE_VALUE;
    }

#ifndef QTI_ASYNC_IO
    // QCDEV_LOCK_MUTEX_AND_UNLOCK_ON_RETURN(&qcdevCtx.mListLock);
    appHdl = open(DeviceName, O_RDWR | O_NOCTTY | O_CLOEXEC | O_NONBLOCK);
#else

    QDAIO::Initialize();

    QCDEV_LOG_DBG("Initialized QADIO members\n");

    appHdl = QDAIO::OpenDevice(DeviceName);

#endif
    QCDEV_LOG_INFO("appHdl is %d \n", appHdl);

    if (appHdl < 0)
    {
        QCDEV_LOG_ERR("Failed to open device\n");
        return INVALID_HANDLE_VALUE;
    }

    return appHdl;
}

/**
 * @brief   To open the selected device
 *
 * Opens the device and returns the device handle
 *
 * @param   DeviceName  Device which is to be opened
 *
 * @returns HANDLE      on success returns device handle
 */
HANDLE QcDevice::OpenDevice(PVOID DeviceName)
{
    QCDEV_LOG_INFO("Open DeviceName:%s\n",(PCHAR)DeviceName);

    LONG appHdl = INVALID_HANDLE_VALUE;

    if (DeviceName == NULL)
    {
        QCDEV_LOG_ERR("Invalid arguements\n");
        return INVALID_HANDLE_VALUE;
    }

    /* EUD device handling: if the name contains "EUD", route to QcEud APIs */
    if (strcasestr((PCHAR)DeviceName, "EUD"))
    {
        HANDLE hEud = (HANDLE)(intptr_t)INVALID_HANDLE_VALUE;
        if (strcasestr((PCHAR)DeviceName, "Control"))
        {
            hEud = QcEud_OpenDevice(DeviceName, QC_EUD_CTL_HANDLE);
        }
        else if (strcasestr((PCHAR)DeviceName, "Trace"))
        {
            hEud = QcEud_OpenDevice(DeviceName, QC_EUD_TRC_HANDLE);
        }
        else
        {
            QCDEV_LOG_ERR("OpenDevice: EUD device %s is neither Control nor Trace\n", (PCHAR)DeviceName);
            hEud = (HANDLE)(intptr_t)INVALID_HANDLE_VALUE;
        }
        return hEud;
    }

    if (IsQcomLibusbEnable())
    {
        int retStatus = QcomLibusbDevice::qcom_libusb_open((PCHAR)DeviceName, (PCHAR)NULL, (void**)&appHdl);
        if (retStatus != LIBUSB_SUCCESS) {
            QCDEV_LOG_ERR("libusb open failed: %d\n", retStatus);
        }
    }
    else
    {
#ifndef QTI_ASYNC_IO
        // QCDEV_LOCK_MUTEX_AND_UNLOCK_ON_RETURN(&qcdevCtx.mListLock);
        appHdl = open(DeviceName, O_RDWR | O_NOCTTY | O_CLOEXEC);
#else

        QDAIO::Initialize();

        QCDEV_LOG_DBG("Initialized QADIO members\n");

        appHdl = QDAIO::OpenDevice(DeviceName);

#endif
        QCDEV_LOG_INFO("appHdl is %d \n", appHdl);

        if (appHdl < 0)
        {
            QCDEV_LOG_ERR("Failed to open device\n");
            return INVALID_HANDLE_VALUE;
        }
    }


    return appHdl;
}

/**
 * @brief   To close the selected handle
 *
 * Close the device handle
 *
 * @param   hDevice     Device handle
 *
 * @returns Nothing
 */
VOID QcDevice::CloseDevice(HANDLE hDevice)
{
    QCDEV_LOG_INFO("In \n");
    LONG appHdl = INVALID_HANDLE_VALUE;

    /* EUD device: close via QcEud APIs */
    if (QcEud_FindHandle(hDevice))
    {
        QcEud_CloseDevice();
        QcEud_UnregisterHandle(hDevice);
        return;
    }

    if (IsQcomLibusbEnable())
    {
        QcomLibusbDevice::qcom_libusb_close((void**)&hDevice);
        appHdl = hDevice;
    }
    else
    {
#ifndef QTI_ASYNC_IO
    if (hDevice < 0)
    {
        QCDEV_LOG_ERR("Invalid device handle\n");
    }
    else
    {
        // QCDEV_LOCK_MUTEX_AND_UNLOCK_ON_RETURN(&qcdevCtx.mListLock);
        appHdl = close(hDevice);
    }
#else
    QDAIO::CloseDevice(hDevice);
#endif
    }

    if (appHdl < 0)
    {
        QCDEV_LOG_ERR("Failed to close device. error:%d\n",appHdl);
        return;
    }

    QCDEV_LOG_INFO("Successfully closed dev Hdl:%d\n",appHdl);
    return;
}

/**
 * @brief   To Read from the selected handle
 *
 * Blocking I/O read
 *
 * @param   hDevice             Device handle
 * @param   RxBuffer            Receive buffer
 * @param   NumBytesToRead      Length of receieve buffer
 * @param   NumBytesReturned    Length of data Read
 *
 * @returns on success returns TRUE else FALSE
 */
BOOL QcDevice::ReadFromDevice(HANDLE hDevice, PVOID RxBuffer,
                              DWORD NumBytesToRead, LPDWORD NumBytesReturned)
{
    /* EUD trace read */
    if (QcEud_FindHandle(hDevice))
    {
        BOOL bErr = QcEud_ReadTrace(RxBuffer, NumBytesToRead, NumBytesReturned);
        QCDEV_LOG_DBG("EUD Trace ReadFromDevice: err=%d NumBytesReturned: %d\n",
                       bErr, *NumBytesReturned);
        return bErr;
    }

    if (IsQcomLibusbEnable())
    {
        BOOL bytesTransferred = 0;
        int retStatus = QcomLibusbDevice::qcom_libusb_read((void**)&hDevice, RxBuffer, NumBytesToRead, &bytesTransferred, 0);
        if (retStatus == LIBUSB_SUCCESS)
        {
            *NumBytesReturned = bytesTransferred;
            QCDEV_LOG_DBG("Handle: 0x%x, readFrom %d bytes, ActualNumBytesReturned: %d\n", hDevice, NumBytesToRead, bytesTransferred);
            return QCDEV_TRUE;
        }
        else if (retStatus == LIBUSB_ERROR_TIMEOUT)
        {
            *NumBytesReturned = 0;
            QCDEV_LOG_DBG("LIBUSB_ERROR_TIMEOUT: Handle: 0x%lx, readFrom %d bytes, ActualNumBytesReturned: %d\n",
                      (long)hDevice, NumBytesToRead, bytesTransferred);
            return QCDEV_TRUE;
        }
        else
        {
            *NumBytesReturned = 0;
            QCDEV_LOG_DBG("Failed!! Handle: 0x%x, readFrom %d bytes, ActualNumBytesReturned: %d, result: %d\n", hDevice, NumBytesToRead, bytesTransferred, retStatus);
            return QCDEV_FALSE;
        }
    }
    else
    {
#ifndef QTI_ASYNC_IO
        if (hDevice < 0 || !RxBuffer || !NumBytesToRead || !NumBytesReturned)
        {
            QCDEV_LOG_ERR("Invalid device parameters\n");
            return QCDEV_FALSE;
        }
        LONG BytesReturned = 0;
        // QCDEV_LOCK_MUTEX_AND_UNLOCK_ON_RETURN(&qcdevCtx.mListLock);

        BytesReturned = read(hDevice, RxBuffer, NumBytesToRead);
        if (BytesReturned < 0) {
            *NumBytesReturned = 0;
            QCDEV_LOG_DBG("Read failed: %ld, Error: %s\n", BytesReturned, strerror(errno));
	    	return QCDEV_FALSE;
        }
        else {
            *NumBytesReturned = BytesReturned;
        }

        return QCDEV_TRUE;
#else
        return QDAIO::ReadFromDevice(hDevice, RxBuffer, NumBytesToRead, NumBytesReturned);
#endif
    }
}

/**
 * @brief   To write to the selected handle
 *
 * Sends the data on device handle
 *
 * @param   hDevice             Device handle
 * @param   TxBuffer            Send buffer
 * @param   NumBytesToSend      Length of send buffer
 * @param   NumBytesSent        Length of data sent
 *
 * @returns on success returns TRUE else FALSE
 */
BOOL QcDevice::SendToDevice(HANDLE hDevice, PVOID TxBuffer,
                            DWORD NumBytesToSend, LPDWORD NumBytesSent)
{
    if (IsQcomLibusbEnable())
    {
        BOOL bytesTransferred = 0;
        int retStatus = QcomLibusbDevice::qcom_libusb_write((void**)&hDevice, TxBuffer, NumBytesToSend, &bytesTransferred, 0);
        if (retStatus == LIBUSB_SUCCESS)
        {
            *NumBytesSent = bytesTransferred;
            QCDEV_LOG_DBG("Handle: 0x%x, sentTo %d bytes, ActualNumBytesSent: %d\n", hDevice, NumBytesToSend, bytesTransferred);
            return QCDEV_TRUE;
        }
        else
        {
            *NumBytesSent = 0;
            QCDEV_LOG_DBG("Failed!! Handle: 0x%x, sentTo %d bytes, Actual sent: %d, result: %d\n", hDevice, NumBytesToSend, bytesTransferred, retStatus);
            return QCDEV_FALSE;
        }
    }
    else
    {
#ifndef QTI_ASYNC_IO
    if (hDevice < 0 || !TxBuffer || !NumBytesToSend || !NumBytesSent)
    {
        QCDEV_LOG_ERR("Invalid device parameters\n");
        return QCDEV_FALSE;
    }

    LONG BytesWritten = 0;
    // QCDEV_LOCK_MUTEX_AND_UNLOCK_ON_RETURN(&qcdevCtx.mListLock);
    BytesWritten = write(hDevice, TxBuffer, NumBytesToSend);
        // QCDEV_LOCK_MUTEX_AND_UNLOCK_ON_RETURN(&qcdevCtx.mListLock);
    if (BytesWritten < 0) {
        *NumBytesSent = 0;
        QCDEV_LOG_DBG("Write failed: %ld, Error: %s\n", BytesWritten, strerror(errno));
		return QCDEV_FALSE;
    }
    else {
        *NumBytesSent = BytesWritten;
        return QCDEV_TRUE;
    }

#else
    return QDAIO::SendToDevice(hDevice, TxBuffer, NumBytesToSend, NumBytesSent);
#endif
    }
}
