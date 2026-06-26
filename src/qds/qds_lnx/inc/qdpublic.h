// Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
// SPDX-License-Identifier: BSD 3-Clause Clear License
#ifndef QDPUB_H
#define QDPUB_H

#include <stdio.h>
#define QCDEV_LINUX
#ifdef QCDEV_LINUX
#include <stdlib.h>
#include <pthread.h>
#include <string.h>
#include "windows_compatible.h"     /**< Local header to have compatibility */
#include <errno.h>
//#include <qdutils.h>
#include <unistd.h>
#include <dirent.h>
#include <fcntl.h>
#include <sys/socket.h>
#include <unordered_map>
#include <string>

#else
#include <windows.h>
#endif

#ifdef QCDEV_LINUX
#define QCDEVLIB_API
#else
#ifdef QCDEVLIB_EXPORTS
#define QCDEVLIB_API __declspec(dllexport)
#elif QCDEVLIB_STATIC_LINK
#define QCDEVLIB_API
#else
#define QCDEVLIB_API __declspec(dllimport)
#endif
#endif

#ifdef QCDEV_LINUX

#define QCDEV_PORT_DIR              "/dev/"         /**< Device loaction            */
#define QCDEV_DEV_DESC              "Qualcomm"      /**< Device description         */
#define QCDEV_TTY_NAME              "ttyUSB"        /**< Default name for tty USB dev*/
#define QCDEV_QMI_NAME              "usb"           /**< Default name for NET dev   */

#define QCDEV_MAX_VALUE_NAME        256            /**< Maximum length of string */
#define QCDEV_PORT_NAME_LEN         256             /**< Maximum length of port name */
#define QCDEV_UDEV_MONITOR_UDEV     "udev"  /**< Select the uevents from udev*/

#define SUBSYSTEM_TTY               "tty"           /**< TTY Subsystem for udev     */
#define SUBSYSTEM_USB               "usb"           /**< USB Subsystem for udev     */
#define SUBSYSTEM_QCOM_NET          "qcom_usbnet"    /**< qcom net Subsystem for udev    */
#define SUBSYSTEM_QCOM_USB          "qcom_usb"       /**< qcom usb Subsystem for udev*/
#define SUBSYSTEM_QCOM_PORTS        "qcom_ports"    /**< qcom ports for Diag Subsystem for udev    */
#define OLDER_SUBSYSTEM_GOBINET     "GobiQMI"    /**< GobiNet Subsystem for udev    */
#define OLDER_SUBSYSTEM_GOBIUSB     "GobiUSB"       /**< GobiUSB Subsystem for udev*/
#define OLDER_SUBSYSTEM_GOBIPORTS   "GobiPorts"    /**< GobiPorts for Diag Subsystem for udev    */
#define SUBSYSTEM_MHI               "mhi_uci"       /**< PCIe dev subsusyem    */
#define SUBSYSTEM_WWAN              "wwan"          /**< PCIe dev subsusyem    */
#define VID_STR_LEN                 4               /**< To extract VID ex: VID_05C6*/
#define PID_STR_LEN	                4

#define QCDEV_INTERFACE_PROTOCOL_ADB  0x01  /**< ADB protocol    */
#define QCDEV_INTERFACE_CLASS_ADB     0xFF  /**< ADB class       */
#define QCDEV_INTERFACE_SUBCLASS_ADB  0x42  /**< ADB subclass    */
#define QCDEV_ALTERNATE_SETTING_ADB   0x00  /**< ADB alt setting */

#define QCDEV_INTERFACE_PROTOCOL_FASTBOOT  0x03  /**< FASTBOOT protocol    */
#define QCDEV_INTERFACE_CLASS_FASTBOOT     0xFF  /**< FASTBOOT class       */
#define QCDEV_INTERFACE_SUBCLASS_FASTBOOT  0x42  /**< FASTBOOT subclass    */

#define QCDEV_INTERFACE_PROTOCOL_RNDIS    0x01  /**< RNDIS protocol     */
#define QCDEV_INTERFACE_CLASS_RNDIS       0xEF  /**< RNDIS class        */
#define QCDEV_INTERFACE_SUBCLASS_RNDIS    0x04  /**< RNDIS subclass     */
#define QCDEV_ALTERNATE_SETTING_RNDIS     0x00  /**< RNDISalt seting    */

#endif

/* constants for the Flag in Device-change callback */
#define QC_DEV_TYPE_NONE   0x00             /**< Unknown Device type    */
#define QC_DEV_TYPE_NET    0x01             /**< Network Adapter        */
#define QC_DEV_TYPE_PORTS  0x02             /**< Diag, Modem devices    */
#define QC_DEV_TYPE_USB    0x03             /**< Gen-USB like QDSS/DPL  */
#define QC_DEV_TYPE_MDM    0x04             /**< For Modem devices      */
#define QC_DEV_TYPE_ADB    0x05             /**< For Adb devices        */
#define QC_DEV_TYPE_MBIM   0x06             /**< For Microsoft MBIM     */
#define QC_DEV_TYPE_RNDIS  0x07             /**< For Remote NDIS     */
#define QC_DEV_TYPE_TAC    0x08             /**< For TAC devices     */
#define QC_DEV_TYPE_EPM    0x09             /**< For EPM devices     */
//#define QC_DEV_TYPE_LIBUSB 0x0A             /**< For LIBUSB based devices  */

/* State of the device */
#define QC_DEV_STATE_DEPARTURE 0x00         /**< Device removed/exited  */
#define QC_DEV_STATE_ARRIVAL   0x01         /**< Device Added/inserted  */

/* constants for the Flag in Device-change callback */
#define QC_FLAG_MASK_QC_DRIVER    0x0000000F/**< Is from Qualcomm driver*/
#define QC_FLAG_MASK_DEV_STATE    0x000000F0/**< Mask for dev state     */
#define QC_FLAG_MASK_DEV_TYPE     0x0000FF00/**< Mask for dev type      */
#define QC_FLAG_MASK_DEV_BUS      0x000F0000/**< Mask for bus type      */
#define QC_FLAG_MASK_DEV_PROTOCOL 0x000000FF/**< Mask for protocol type */
#define QC_FLAG_MASK_DEV_PROTOCOL_CATEGORY   0x000000F0/**< Mask for protocol category */

/* Type of bus on which device connected */
#define QC_DEV_BUS_TYPE_NONE 0              /**<< Unknown bus           */
#define QC_DEV_BUS_TYPE_USB  1              /**<< Device is on USB bus  */
#define QC_DEV_BUS_TYPE_PCI  2              /**<< Device is on PCI bus  */
#define QC_DEV_BUS_TYPE_PCIE 3              /**<< Device is on QC bus   */

/* configurable settings */
#define DEV_FEATURE_INCLUDE_NONE_QC_PORTS 0x00000001 /**< Inclulde no QC ports*/
#define DEV_FEATURE_SCAN_USB_WITH_VID     0x00010000 /**< Scan with VID given */

/* To select the Mask for device discovery */
#define DEV_CLASS_NET   0x00000001          /**< To select Network class*/
#define DEV_CLASS_PORTS 0x00000002          /**< To select ports class  */
#define DEV_CLASS_USB   0x00000004          /**< To select gen-usb class*/
#define DEV_CLASS_MDM   0x00000008          /**< To select modem class  */
#define DEV_CLASS_ADB   0x00000010          /**< To select ADB Class    */

/* USB Protocol Number Assignment */
#define DEV_PROTOCOL_UNKNOWN   0x00         /**< Unknown protocol   */
#define DEV_PROTOCOL_SAHARA    0x10         /**< Sahara protocol    */
#define DEV_PROTOCOL_SAHARA_PBL_EMERGENCY_DOWNLOAD 0x11
#define DEV_PROTOCOL_SAHARA_PBL_FLASHLESS_BOOT     0x12
#define DEV_PROTOCOL_SAHARA_SBL_XBL_RAMDUMP        0x13
#define DEV_PROTOCOL_SAHARA_TN_APPS_Remote_EFS     0x14
#define DEV_PROTOCOL_FIREHOSE  0x20         /**< Firehose protocol  */
#define DEV_PROTOCOL_DIAG      0x30         /**< Diag protocol      */
#define DEV_PROTOCOL_DUN       0x40         /**< DUN protocol       */
#define DEV_PROTOCOL_RMNET     0x50         /**< RmNet protocol     */
#define DEV_PROTOCOL_NMEA      0x60         /**< NMEA protocol      */
#define DEV_PROTOCOL_QDSS      0x70         /**< QDSS protocol      */
#define DEV_PROTOCOL_ADPL      0x80         /**< ADPL protocol      */
#define DEV_PROTOCOL_RESERVED  0xFF         /**< Reserved protocol  */
#define DEV_PROTOCOL_MAJOR_REVISION_MASK           0xF0

#define DevNode "/dev/"

struct device_info;

/* Align structures to 8 bytes packing. */
#pragma pack(push, 8)

#define VOID void

/**
 *  @brief          Device features, need to set
 */
typedef struct _DEV_FEATURE_SETTING
{
    ULONG Version;       /**< 1                      */
    ULONG Settings;      /**< OR'ed feature masks    */
    ULONG DeviceClass;   /**< 0 - all classes        */
    PTSTR VID;
} DEV_FEATURE_SETTING, *PDEV_FEATURE_SETTING;

/**
 *  @brief          Device information, which is obtained as part of device
 *                  Arrival / Departure
 */
typedef struct _CB_PARAMS
{
    PCHAR DevDesc;  /**< device description for display in unicode     */
    PCHAR  DevName; /**< device name for I/O
                         (normally the symbolic name)                  */
    PCHAR IfName;   /**< interface name for network adapter            */
    PCHAR Loc;      /**< device's location on its bus                  */
    PCHAR DevPath;  /**< device's connection path bus                  */
    PCHAR SerNum;   /**< serial number from device                     */
    PCHAR SerNumMsm;/**< serial number from device (Product name)      */
    PCHAR SocVer;   /**< Soc version                                   */
    ULONG  Mtu;     /**< MTU for network connection (data call)        */
    ULONG  Flag;    /**< flag to indicate device type, state, and
                          availability of QC driver support            */
    ULONG  Protocol;/**< protocol code (low 8 bits)of the USB function,
                          0 means unknown protocol                     */
    PCHAR HwId;     /**< device hardware ID                            */
    PCHAR ParentDev;/**< name of parent device if present              */
    PCHAR ParentLocationInfomation;/**< physical location of parent device if present */
    std::unordered_map<std::string, std::string>* DevDetails; // pointer-based: contains fields obtained from parsed device description string
} CB_PARAMS, *PCB_PARAMS;

/**
 *  @brief          Device error info
 */
typedef enum _DEV_INFO_ERRNO
{
    DEV_INFO_OK = 0,
    DEV_INFO_DEV_DESC_LEN = 1,
    DEV_INFO_DEV_NAME_LEN = 2,
    DEV_INFO_IF_NAME_LEN  = 3,
    DEV_INFO_LOC_LEN      = 4,
    DEV_INFO_SER_NUM_LEN  = 5,
    DEV_INFO_END          = 255
} DEV_INFO_ERRNO;

/**
 *  @brief          Device change callback
 */
typedef VOID (_stdcall *DEVICECHANGE_CALLBACK)(PCB_PARAMS CbParams, PVOID *Context);
typedef VOID (_stdcall *DEVICECHANGE_CALLBACK_N)(VOID);

/**
 *  @brief          Logging callback with NULL-terminated ANSI string
 */
typedef VOID (_stdcall *QCD_LOGGING_CALLBACK)(int log_level, PCHAR Message);

/**
 *  @brief          Device information, which is obtained as part of device
 *                  Arrival / Departure
 */
typedef struct _DEV_PARAMS_N
{
    CHAR  DevDesc[QCDEV_MAX_VALUE_NAME];
    /**< device description for display in unicode  */
    ULONG  DevDescBufLen;
    /**< in bytes                                   */
    CHAR  DevName[QCDEV_MAX_VALUE_NAME];
    /**< device name for I/O (normally the symbolic name), ANSI string  */
    ULONG  DevnameBufLen;
    /**< in bytes                                   */
    CHAR  IfName[QCDEV_MAX_VALUE_NAME];
    /**< interface name for network adapter, in unicode */
    ULONG  IfNameBufLen;
    /**< in bytes                                   */
    CHAR  Loc[QCDEV_MAX_VALUE_NAME];
    /**< device's location on its bus, in unicode   */
    ULONG  LocBufLen;
    /**< in bytes                                   */
    CHAR  DevPath[QCDEV_MAX_VALUE_NAME];
    /**< device's connection path                   */
    ULONG  DevPathBufLen;
    /**< in bytes                                   */
    CHAR  SerNum[QCDEV_MAX_VALUE_NAME];
    /**< serial number from device, in unicode      */
    ULONG  SerNumBufLen;
    /**< in bytes                                   */
    CHAR  SerNumMsm[QCDEV_MAX_VALUE_NAME];
    /**< serial number from device, in unicode      */
    ULONG  SerNumMsmBufLen; /**< in bytes                                   */
    ULONG  Mtu;             /**< MTU for network connection (data call)     */
    ULONG  Flag;            /**< flag to indicate device type, state,
                                 and availability of QC driver support      */
    ULONG  Protocol;        /**< protocol code of the USB function,
                                 0 means unknown protocol                   */
    CHAR   HwId[QCDEV_MAX_VALUE_NAME];

    ULONG HwIdBufLen;

    CHAR   ParentDev[QCDEV_MAX_VALUE_NAME];
    ULONG  ParentDevBufLen;

    CHAR SocVer[QCDEV_MAX_VALUE_NAME];
    ULONG SocVerLen;

    CHAR  ParentLocationInfomation[QCDEV_MAX_VALUE_NAME];
    ULONG  ParentLocationInfomationBufLen;
    std::unordered_map<std::string, std::string>* DevDetails; // pointer-based: contains fields obtained from parsed device description string
} DEV_PARAMS_N, *PDEV_PARAMS_N;

/**
 *  @brief          Device info, which is obtained from device discovery
 */
typedef struct _DeviceCtx{
    DEV_PARAMS_N   mDevParams;          /**< Device information, for callback */
    CB_PARAMS   mCbParams;
    CHAR       mProductName[QCDEV_MAX_VALUE_NAME];/**< Device name/path     */
    CHAR        mDevNode[QCDEV_MAX_VALUE_NAME];
    CHAR        mSubsystem[QCDEV_MAX_VALUE_NAME];
    ULONG       mProductNameLen;        /**< Device name len in bytes       */
    PVOID       mContextInt;            /**< Device context modified in app */
    PVOID       mpUserContext;
    struct  _DeviceCtx *mNext;          /**< Pointer to next dev info       */
}DeviceCtx, *PDeviceCtx;

/**
 *  @brief          Device discovery main context
 */
typedef struct _QcdevCtx{
    DEVICECHANGE_CALLBACK   mCallback;      /**< device change callback */
    DEVICECHANGE_CALLBACK_N mGenericDevCB;  /**< device change callback */
    PVOID                   mCallbackCtx;   /**< appCtx to store        */
    QCD_LOGGING_CALLBACK    mLogCallback;   /**< logging callback       */
    PDEV_FEATURE_SETTING    mSetting;       /**< Device features        */
    struct udev*            mUdev;          /**< udev instance          */
    PDeviceCtx              mpDeviceCtx;    /**< To store the dev info  */
    pthread_mutex_t         mListLock;      /**< Lock for the ctx       */
    pthread_t               mThId;       /**< Thread Id, created at start discovery */
    int                     mMonitorFd[2];     /**< udev fd to monitor dev */
    int                     mDevidx;        /**< Index to monitor dev's */
    struct udev_monitor*    mon;            /**< udev monitor */
} QcdevCtx, *PQcdevCtx;

#ifdef __cplusplus
//extern "C" {
#endif
/**
 *  @brief          APIs for device monitor/update
 */
namespace QcDevice
{
    
    /**
     * Read debugFS log_level and update the same for library
     *
     * @param   file			Path of the file to be read
     *
     * @returns                 Nothing
     */
    void update_loglvl_from_fs(char* file);

    /**
     * @brief   To read the debug level from sysfs
     *
     * @param   int	    log_level
     *
     * @returns         Nothing
     */
    void libqcdev_read_loglvl();
    /**
     * @brief   To create log files based on log_level value
     *
     * @param   int	    log_level
     *
     * @returns         Nothing
     */
    void logFileCreation();
    
    /**
     * @brief   To store the LoggingCallback in main Ctx
     *
     * stores the logging callback info in the main Ctx, and gets
     * triggerd on calling logs on the registered callback
     *
     * @param   Cb    logging callback
     *
     * @returns Nothing
     */
    VOID SetLoggingCallback(QCD_LOGGING_CALLBACK Cb);

    /**
     * @brief   To set the Logging Level in main Ctx
     *
     * stores the logging level info in the main Ctx
     *
     * @param   int	log_level
     *
     * @returns Nothing
     */
    VOID SetLoggingLevel(int log_level);

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
    QCDEVLIB_API VOID SetDeviceChangeCallback(DEVICECHANGE_CALLBACK Cb);

    /**
     * @brief   To store the DeviceChangeCallback and application Ctx in main Ctx
     *
     * stores the device callback info in the main Ctx and application Ctx, and gets
     * triggerd on status change on the registered callback
     *
     * @param   Cb    device change callback
     *
     * @returns Nothing
     */
    QCDEVLIB_API VOID SetDeviceChangeCallback(DEVICECHANGE_CALLBACK Cb, PVOID AppContext);

    /**
     * @brief   To store the DeviceChangeCallback_N in main Ctx
     *
     * stores the device callback info in the main Ctx and application Ctx, and gets
     * triggerd on status change on the registered callback
     *
     * @param   Cb    device change callback
     *
     * @returns Nothing
     */
    QCDEVLIB_API VOID SetDeviceChangeCallback(DEVICECHANGE_CALLBACK_N Cb);

    /**
     * @brief   To update the features in main Ctx
     *
     * To update the device features in main ctx
     *
     * @param   Features    Features to store
     *
     * @returns Nothing
     */
    QCDEVLIB_API VOID SetFeature(PVOID Features);

    /**
     * @brief   To start the device monitor
     *
     * To initiate the device monitor on the respective Subsystems
     *
     * @returns Nothing
     */
    QCDEVLIB_API VOID StartDeviceMonitor(VOID);

    /**
     * @brief   To stop the device monitor
     *
     * To stop the device monitor on the respective Subsystems
     *
     * @returns Nothing
     */
    QCDEVLIB_API VOID StopDeviceMonitor(VOID);

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
    QCDEVLIB_API ULONG GetDeviceList
        (
         PVOID Buffer,
         ULONG BufferSize,
         PULONG ActualSize
         );

    /**
     * @brief   To open the selected device
     *
     * Opens the device and returns the device handle
     *
     * @param   DeviceName  Device which is to be opened
     *
     * @returns HANDLE      on success returns device handle
     */
    QCDEVLIB_API HANDLE OpenDevice(PVOID DeviceName);
    QCDEVLIB_API HANDLE OpenDevice_(PVOID DeviceName);

    /**
     * @brief   To close the selected handle
     *
     * Close the device handle
     *
     * @param   hDevice     Device handle
     *
     * @returns Nothing
     */
    QCDEVLIB_API VOID CloseDevice(HANDLE hDevice);

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
    QCDEVLIB_API BOOL ReadFromDevice
        (
         HANDLE hDevice,
         PVOID  RxBuffer,
         DWORD  NumBytesToRead,
         LPDWORD NumBytesReturned
        );

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
    QCDEVLIB_API BOOL SendToDevice
        (
         HANDLE hDevice,
         PVOID  TxBuffer,
         DWORD  NumBytesToSend,
         LPDWORD NumBytesSent
        );
}

#ifdef __cplusplus
//}
#endif

#pragma pack(pop)

#endif // QDPUB_H
