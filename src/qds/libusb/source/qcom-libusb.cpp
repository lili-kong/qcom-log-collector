// Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
// SPDX-License-Identifier: BSD 3-Clause Clear License
#include "qcom-libusb.h"
#ifdef TOOLS_TARGET_WINDOWS
#include "../common/utils.h"
#endif
#include <iostream>
#include <unordered_map>
#include <set>
#include <vector>
#include <string>

using namespace std;
using namespace Utils;

static struct libusb_context *g_ctx = nullptr;
#define CHECK_LIBUSB_LOADED(retval)                                              \
    do {                                                                          \
        if (!is_libusb_loaded()) {                                                \
            QCD_Printf(Info,                                                       \
                "%s: libusb DLL/SO not loaded; skipping call.\n",        \
                __func__);                                                        \
            return (retval);                                                      \
        }                                                                         \
    } while (0)

#define CHECK_LIBUSB_LOADED_VOID()                                               \
    do {                                                                          \
        if (!is_libusb_loaded()) {                                                \
            QCD_Printf(Info,                                                       \
                "%s: libusb DLL/SO not loaded; skipping call.\n",        \
                __func__);                                                        \
            return;                                                               \
        }                                                                         \
    } while (0)
#ifdef QCOM_WIN_ENV
extern std::atomic<BOOL> _IsQcomLibusbEnable;
#endif
#ifdef QCOM_LNX_ENV
#include "../../qds_lnx/inc/qdpublic.h"
#endif
static mutex qcomdevmap_mutex;
static mutex dev_handle_map_mutex;
static unordered_map<uint64_t, QcomDeviceInfo*> dev_handle_map;
using libusb_handle_map_iter = std::unordered_map<uint64_t, QcomDeviceInfo*>::iterator;
static unordered_map<string, QcomLibUSBdevCtx*> qcomdevmap;

QcomLibUSBdevCtx *get_qcomdev_ctx_mapping(char *, libusb_device*);
void remove_qcomdev_ctx_mapping(char *, libusb_device*);
QcomLibUSBdevCtx* get_qcomdev_ctx_map(string);

#ifdef QCOM_WIN_ENV

// Determine protocol based on friendlyName
BOOL get_interface_ctx(const char* DeviceName, ULONG protocol)
{
    if (strstr(DeviceName, "Diagnostics") && protocol == INTERFACE_DIAG_PROTOCOL)
        return true;
    else if (strstr(DeviceName, "Modem") && protocol == INTERFACE_MODEM_PROTOCOL)
        return true;
    else if (strstr(DeviceName, "DPL") && protocol == INTERFACE_DPL_PROTOCOL)
        return true;
    else if (strstr(DeviceName, "QDSS") && protocol == INTERFACE_QDSS_PROTOCOL)
        return true;
    else if (strstr(DeviceName, "WWAN") && protocol == INTERFACE_RMNET_PROTOCOL)
        return true;
    else if (strstr(DeviceName, "QDLoader") && protocol == INTERFACE_EDL_PROTOCOL)
        return true;
    else if (strstr(DeviceName, "Diagnostics") && protocol == INTERFACE_SAHARA_PROTOCOL)
        return true;
    else if (strstr(DeviceName, "QDLoader") && protocol == INTERFACE_SAHARA_PROTOCOL)
        return true;
    else
        return false;
}

VOID set_param_in_registry(LPCSTR driverKeyPath, const char* InterfaceName, string ports)
{
    QcomDeviceInfo* pDevInfo;
    string qcomdevkey = ports;
    QcomLibUSBdevCtx* qcom_dev = nullptr;
    qcom_dev = get_qcomdev_ctx_map(qcomdevkey);
    if (qcom_dev == nullptr) {
        QCD_Printf(Error, "%s: qcom_dev null, qcomdevkey not found\n", __func__);
        return;
    }
    LIST_ENTRY* head = &qcom_dev->deviceListHead;
    LIST_ENTRY* pos;

    if (!IsListEmpty(head))
    {
        head = &qcom_dev->deviceListHead;
        pos = head->Flink;
        while (pos != head)
        {
            pDevInfo = CONTAINING_RECORD(pos, QcomDeviceInfo, List);
            BOOL Status = get_interface_ctx(InterfaceName, pDevInfo->protocol);
            if (Status == true) {
                QCD_Printf(Info,"%s: InterfaceName: %s , Status: %d, SerNum: [%s] SerNumMsm: [%s] Protocol: [%ld]\n",
                    __func__, InterfaceName, Status, pDevInfo->SerNum, pDevInfo->SerNumMSM, pDevInfo->protocol);
                if (set_serial_num_in_registry(driverKeyPath, pDevInfo->SerNum) != true) {
                    QCD_Printf(Error, "Failed to set SerNum registry: %s\n", pDevInfo->SerNum);
                }
                else {
                    QCD_Printf(Debug, "%s: Successfully Set registry for SerNum: %s\n", __func__, pDevInfo->SerNum);
                }
                if (set_serial_num_msm_in_registry(driverKeyPath, pDevInfo->SerNumMSM) != true) {
                    QCD_Printf(Error, "Failed to set SerNumMsm registry: %s\n", pDevInfo->SerNumMSM);
                }
                else {
                    QCD_Printf(Debug, "%s: Successfully Set registry for SerNumMsm: %s\n", __func__, pDevInfo->SerNumMSM);
                }
                if (set_dev_details_in_registry(driverKeyPath, pDevInfo->DevDetails) != true) {
                    QCD_Printf(Error, "Failed to set DevDetails registry\n");
		        }
                else {
                    QCD_Printf(Debug, "Successfully Set registry for DevDetails\n");
                }
                if (set_protocol_in_registry(driverKeyPath, pDevInfo->protocol) != true) {
                    QCD_Printf(Error, "Failed to set protocol in registry: %ld\n", pDevInfo->protocol);
                }
                else {
                    QCD_Printf(Debug, "%s: InterfaceName:> [%s], Successfully set registry for protocol: %ld\n", __func__, InterfaceName, pDevInfo->protocol);
                }
            }
            else {
                QCD_Printf(Debug, "Unknown protocol, not setting protocol in registry. Status from : %ld\n", Status);
            }

            pos = pos->Flink;
        }
    }
    else {
        QCD_Printf(Error, "%s: Device list empty\n", __func__);
    }
}

BOOLEAN create_user_sw_key_path(LPCSTR SwKeyPath, LPSTR UserKeyPath)
{
    if (!SwKeyPath || !UserKeyPath)
        return FALSE;

    size_t baseLen = strlen(QC_REG_SW_KEY);

    UserKeyPath[0] = '\0';

    if (strncmp(SwKeyPath, QC_REG_SW_KEY, baseLen) == 0) {
        StringCchCopyA(UserKeyPath, QCOM_MAX_STRING_SIZE, QC_REG_SW_KEY_USER);
        StringCchCatA(UserKeyPath, QCOM_MAX_STRING_SIZE, SwKeyPath + baseLen);
    }
    else {
        StringCchCopyA(UserKeyPath, QCOM_MAX_STRING_SIZE, QC_REG_SW_KEY_USER);
        StringCchCatA(UserKeyPath, QCOM_MAX_STRING_SIZE, SwKeyPath);
    }

    return TRUE;
}

BOOLEAN set_protocol_in_registry(LPCSTR SW_KEY_PATH, DWORD protocol)
{
    HKEY  hSwKey;
    DWORD retCode;

    char userKeyPath[QCOM_MAX_STRING_SIZE];
    create_user_sw_key_path(SW_KEY_PATH, userKeyPath);

    retCode = RegCreateKeyExA(HKEY_LOCAL_MACHINE,
        SW_KEY_PATH,
        0, NULL,
        REG_OPTION_NON_VOLATILE, KEY_WRITE,
        NULL, &hSwKey, NULL);

    if (retCode != ERROR_SUCCESS) {
        retCode = RegCreateKeyExA(HKEY_CURRENT_USER,
            userKeyPath,
            0, NULL,
            REG_OPTION_NON_VOLATILE, KEY_WRITE,
            NULL, &hSwKey, NULL);

        if (retCode != ERROR_SUCCESS) {
            QCD_Printf(Debug, "%s: Failed to open/create registry key: %x\n", __func__, retCode);
            return false;
        }
        QCD_Printf(Debug, "New registry path User: %s\n", userKeyPath);
    }

    retCode = RegSetValueExA(hSwKey,
        QC_SPEC_PROTOC,
        0,
        REG_DWORD,
        (const BYTE*)&protocol,
        sizeof(DWORD));

    if (retCode != ERROR_SUCCESS) {
        QCD_Printf(Error, "%s: Failed to set registry value for Protocol: %x\n", __func__, retCode);
        RegCloseKey(hSwKey);
        return false;
    }

    RegCloseKey(hSwKey);

    return true;
}

BOOLEAN set_serial_num_in_registry(LPCSTR SW_KEY_PATH, const char* SerNum)
{
    HKEY  hSwKey;
    DWORD retCode;

    char userKeyPath[QCOM_MAX_STRING_SIZE];
    create_user_sw_key_path(SW_KEY_PATH, userKeyPath);

    retCode = RegCreateKeyExA(HKEY_LOCAL_MACHINE,
        SW_KEY_PATH,
        0, NULL,
        REG_OPTION_NON_VOLATILE, KEY_WRITE,
        NULL, &hSwKey, NULL);

    if (retCode != ERROR_SUCCESS) {
        retCode = RegCreateKeyExA(HKEY_CURRENT_USER,
            userKeyPath,
            0, NULL,
            REG_OPTION_NON_VOLATILE, KEY_WRITE,
            NULL, &hSwKey, NULL);

        if (retCode != ERROR_SUCCESS) {
            QCD_Printf(Debug, "%s: Failed to open/create registry key: %x\n", __func__, retCode);
            return false;
        }
        QCD_Printf(Debug, "New registry path User: %s\n", userKeyPath);
    }

    retCode = RegSetValueExA(hSwKey,
        QC_SPEC_SERNUM,
        0,
        REG_SZ,
        (const BYTE*)SerNum,
        strlen(SerNum) + 1);
    
    if (retCode != ERROR_SUCCESS) {
        QCD_Printf(Error, "%s: Failed to set registry value for SerNum: %x\n", __func__, retCode);
        RegCloseKey(hSwKey);
        return false;
    }

    RegCloseKey(hSwKey);

    return true;
}

BOOLEAN set_serial_num_msm_in_registry(LPCSTR SW_KEY_PATH, const char* SerNumMsm)
{
    HKEY  hSwKey;
    DWORD retCode;

    char userKeyPath[QCOM_MAX_STRING_SIZE];
    create_user_sw_key_path(SW_KEY_PATH, userKeyPath);

    retCode = RegCreateKeyExA(HKEY_LOCAL_MACHINE,
        SW_KEY_PATH,
        0, NULL,
        REG_OPTION_NON_VOLATILE, KEY_WRITE,
        NULL, &hSwKey, NULL);

    if (retCode != ERROR_SUCCESS) {
        retCode = RegCreateKeyExA(HKEY_CURRENT_USER,
            userKeyPath,
            0, NULL,
            REG_OPTION_NON_VOLATILE, KEY_WRITE,
            NULL, &hSwKey, NULL);

        if (retCode != ERROR_SUCCESS) {
            QCD_Printf(Debug, "%s: Failed to open/create registry key: %x\n", __func__, retCode);
            return false;
        }
        QCD_Printf(Debug, "New registry path User: %s\n", userKeyPath);
    }

    retCode = RegSetValueExA(hSwKey,
        QC_SPEC_SERNUM_MSM,
        0,
        REG_SZ,
        (const BYTE*)SerNumMsm,
        strlen(SerNumMsm) + 1);

    if (retCode != ERROR_SUCCESS) {
        QCD_Printf(Error, "Failed to set registry value for SerNumMsm: %x\n", retCode);
        RegCloseKey(hSwKey);
        return false;
    }

    RegCloseKey(hSwKey);

    return true;
}

BOOLEAN set_dev_details_in_registry(LPCSTR SW_KEY_PATH, const char* DevDetails)
{
    HKEY  hSwKey;
    DWORD retCode;

    if (DevDetails == NULL || DevDetails[0] == '\0') {
        QCD_Printf(Error, "Invalid DevDetails parameter\n");
        return false;
    }

    char userKeyPath[QCOM_MAX_STRING_SIZE];
    create_user_sw_key_path(SW_KEY_PATH, userKeyPath);

    retCode = RegCreateKeyExA(HKEY_LOCAL_MACHINE,
        SW_KEY_PATH,
        0, NULL,
        REG_OPTION_NON_VOLATILE, KEY_WRITE,
        NULL, &hSwKey, NULL);

    if (retCode != ERROR_SUCCESS) {
        retCode = RegCreateKeyExA(HKEY_CURRENT_USER,
            userKeyPath,
            0, NULL,
            REG_OPTION_NON_VOLATILE, KEY_WRITE,
            NULL, &hSwKey, NULL);

        if (retCode != ERROR_SUCCESS) {
            QCD_Printf(Debug, "%s: Failed to open/create registry key: %x\n", __func__, retCode);
            return false;
        }
    }

    retCode = RegSetValueExA(hSwKey,
        QC_SPEC_DEV_NAME,
        0,
        REG_SZ,
        (const BYTE*)DevDetails,
        strlen(DevDetails) + 1);

    if (retCode != ERROR_SUCCESS) {
        QCD_Printf(Error, "Failed to set registry value for DevDetails: %x\n", retCode);
        RegCloseKey(hSwKey);
        return false;
    }

    RegCloseKey(hSwKey);

    return true;
}
#endif

VOID display_devices(QcomLibUSBdevCtx* qcom_dev)
{
     QcomDeviceInfo* pDevInfo;
    LIST_ENTRY* head = &qcom_dev->deviceListHead;
    LIST_ENTRY* pos;

    if (!IsListEmpty(head))
    {
        head = &qcom_dev->deviceListHead;
        pos = head->Flink;
        while (pos != head)
        {
            pDevInfo = CONTAINING_RECORD(pos, QcomDeviceInfo, List);
            QCD_Printf(Info, "%s: SerNum: [%s] SerNumMsm: [%s] Protocol: [%ld]\n", __func__, pDevInfo->SerNum, pDevInfo->SerNumMSM, pDevInfo->protocol);
            pos = pos->Flink;
        }
    }
    else {
        QCD_Printf(Info, "%s: deviceListHead is empty\n", __func__);
    }
}

VOID clearLibusbDeviceCtx(QcomLibUSBdevCtx* qcom_dev)
{
    PLIST_ENTRY pEntry;
    PLIST_ENTRY ItemList = &qcom_dev->deviceListHead;
    PQcomDeviceInfo pItem;

    while (!IsListEmpty(ItemList))
    {
        pEntry = RemoveHeadList(ItemList);
        pItem = CONTAINING_RECORD(pEntry, QcomDeviceInfo, List);
        if (pItem->DevDetailsMP) {
            delete pItem->DevDetailsMP;
            pItem->DevDetailsMP = nullptr;
        }
        free(pItem);
    }

    return;
}

VOID clearLibusbArrivalDeviceCtx(QcomLibUSBdevCtx* qcom_dev)
{
    PLIST_ENTRY pEntry;
    PLIST_ENTRY ItemList = &qcom_dev->ArrivalListLibusb;
    PQcomDeviceInfo pItem;

    while (!IsListEmpty(ItemList))
    {
        pEntry = RemoveHeadList(ItemList);
        pItem = CONTAINING_RECORD(pEntry, QcomDeviceInfo, List);
        if (pItem->DevDetailsMP) {
            delete pItem->DevDetailsMP;
            pItem->DevDetailsMP = nullptr;
        }
        free(pItem);
    }

    return;
}

#ifdef QCOM_LNX_ENV
BOOL IsQcomLibusbEnable()
{
    if (access(LIBUSB_CONFIG_FILE, R_OK) != 0) {
        return 0;
    }

    FILE *file = fopen(LIBUSB_CONFIG_FILE, "r");
    if (!file)
        return 0;

    char read_string[QCOM_MAX_STRING_SIZE];
    while(fgets(read_string, sizeof(read_string), file)) {
        if (strstr(read_string, "QCOM_LIBUSB_SUPPORT=1")) {
            fclose(file);
            return 1;
        }
    }

    fclose(file);
    return 0;
}

BOOL create_libusb_devnode(PQcomDeviceInfo newDeviceInf,
                           const char *devName,
                           char *productID,
                           int busNum,
                           int devNum,
                           int config,
                           int interfaceNum)
{
    char devnode_path[QCOM_MAX_STRING_SIZE];
    memset(devnode_path, 0, QCOM_MAX_STRING_SIZE);
    
    snprintf(newDeviceInf->DevDesc, QCOM_MAX_STRING_SIZE, "%s_%s_%d-%d:%d.%d",
            devName, productID, busNum, devNum, config, interfaceNum);
    
    snprintf(devnode_path, QCOM_MAX_STRING_SIZE, "/dev/%s", newDeviceInf->DevDesc);

    strncpy(newDeviceInf->DevDesc, devnode_path, QCOM_MAX_STRING_SIZE - 1);
    newDeviceInf->DevDesc[QCOM_MAX_STRING_SIZE - 1] = '\0';
    QCD_Printf(Info, "%s: devdesc: %s\n",  __func__, newDeviceInf->DevDesc);

    if (mknod(devnode_path, S_IFCHR | 0666, 0) < 0) {
        QCD_Printf(Debug, "%s: device nodes error: %s\n", __func__, strerror(errno));
    }

    return EXIT_SUCCESS;
}

const char *fetchFriendlyDeviceName(int protocol, const char *pid)
{
    if (protocol == INTERFACE_DIAG_PROTOCOL)
        return QCOM_DIAG_DEV_NAME;
    else if (protocol == INTERFACE_MODEM_PROTOCOL)
        return QCOM_MODEM_DEV_NAME;
    else if (protocol == INTERFACE_DPL_PROTOCOL)
        return QCOM_DPL_DEV_NAME;
    else if (protocol == INTERFACE_QDSS_PROTOCOL)
        return QCOM_QDSS_DEV_NAME;
    else if (protocol == INTERFACE_EDL_PROTOCOL || (pid && strncmp(pid, QCOM_EDL_PID, 4) == 0))
        return QCOM_EDL_DEV_NAME;
    else if (protocol == INTERFACE_SAHARA_PROTOCOL)
        return QCOM_DIAG_DEV_NAME;
    else
        return nullptr;
}

BOOL libusb_device_change_callback(libusb_context* ctx,
    libusb_device* dev,
    libusb_hotplug_event event,
    void* user_data)
{
    DeviceCtx* device_ctx = static_cast<DeviceCtx*>(user_data);

    if (device_ctx) {
        QCD_Printf(Info, "%s: device_ctx: 0x%p, DevDesc: %s, SerNumMsm: %s\n",
                   __func__, device_ctx, device_ctx->mDevParams.DevDesc,
                   device_ctx->mDevParams.SerNumMsm);
    }

    if (event == LIBUSB_HOTPLUG_EVENT_DEVICE_ARRIVED) {
        QCD_Printf(Info, "%s: LIBUSB Arrival: libusb_device dev: 0x%p\n", __func__, dev);
        process_device(dev, device_ctx);
    }
    else if (event == LIBUSB_HOTPLUG_EVENT_DEVICE_LEFT) {
        QCD_Printf(Info, "%s: LIBUSB Departure: libusb_device dev: 0x%p\n", __func__, dev);
        remove_qcomdev_ctx_mapping(nullptr, dev);
    }

    return EXIT_SUCCESS;
}

VOID register_libusb_hotplug_callback(void* device_ctx) {
    CHECK_LIBUSB_LOADED_VOID();
    if (fn_libusb_has_capability(LIBUSB_CAP_HAS_HOTPLUG)) {
        QCD_Printf(Info, "libusb hotplug feature is available\n");

        DeviceCtx* deviceCtx = static_cast<DeviceCtx*>(device_ctx);
        if (deviceCtx) {
            QCD_Printf(Info, "%s: device_ctx: 0x%p, DevDesc: %s\n",
                __func__, deviceCtx, deviceCtx->mDevParams.DevDesc);
        }

        fn_libusb_hotplug_register_callback(g_ctx,
            (libusb_hotplug_event)
            LIBUSB_HOTPLUG_EVENT_DEVICE_ARRIVED |
            LIBUSB_HOTPLUG_EVENT_DEVICE_LEFT,
            LIBUSB_HOTPLUG_ENUMERATE,
            QCOM_VENDOR_ID,
            LIBUSB_HOTPLUG_MATCH_ANY,
            LIBUSB_HOTPLUG_MATCH_ANY,
            libusb_device_change_callback,
            deviceCtx,
            NULL);
    }
    else {
        QCD_Printf(Info, "libusb hotplug feature is not available\n");
    }

    return;
}

#endif

BOOL remove_device(QcomLibUSBdevCtx* qcom_dev, libusb_device* dev, void* deviceCtx)
{
#ifdef QCOM_WIN_ENV
    PLIST_ENTRY head = &qcom_dev->ArrivalListLibusb;
#else
    PLIST_ENTRY head = &qcom_dev->deviceListHead;
#endif
    PLIST_ENTRY pos;
    PQcomDeviceInfo pDevInfo;
    int isQcDriver = 1;

    QCD_Printf(Info, "%s happens\n", __func__);

    if (!IsListEmpty(head))
    {
#ifdef QCOM_WIN_ENV
        head = &qcom_dev->ArrivalListLibusb;
#else
        head = &qcom_dev->deviceListHead;
#endif
        pos = head->Flink;
        while (pos != head)
        {
            PLIST_ENTRY next = pos->Flink;
            pDevInfo = CONTAINING_RECORD(pos, QcomDeviceInfo, List);
            QCD_Printf(Info, "%s device desc: %s\n", __func__, pDevInfo->DevDesc);
#ifdef QCOM_LNX_ENV
            pDevInfo->Flag = ((ULONG)QC_DEV_TYPE_USB << 8) | QC_DEV_STATE_DEPARTURE << 4 | isQcDriver;
            PushLibUSBToApplication(pDevInfo);
            unlink(pDevInfo->DevDesc);
#endif
            if (pDevInfo->DevDetailsMP) {
                delete pDevInfo->DevDetailsMP;
                pDevInfo->DevDetailsMP = nullptr;
            }
            RemoveEntryList(pos);
            free(pDevInfo);
            pos = next;
        }
        //system("rm -rf /dev/QCOM_LIBUSB*");  //Added temporarily
    }
    else
    {
#ifdef QCOM_WIN_ENV
        QCD_Printf(Info, "%s: Win: ArrivalListLibusb is empty\n", __func__);
#else
        QCD_Printf(Info, "%s: lnx: deviceListHead is empty\n", __func__);
#endif
    }
    return 0;
}

string create_qcomdev_key(char* serialMsm, libusb_device* dev)
{
    uint8_t busno = fn_libusb_get_bus_number(dev);
    uint8_t devaddr = fn_libusb_get_device_address(dev);
    uint8_t ports[8];
    string qcomdevkey = "";

#ifdef QCOM_WIN_ENV
    if (serialMsm !=  nullptr) {
      //qcomdevkey += string(serialMsm);
    }
    int portcount = fn_libusb_get_port_numbers(dev, ports, sizeof(ports));
    if (portcount > 0) {
        ostringstream oss;
        for (int i = 0; i < portcount; i++) {
            if (i > 0)
                oss << "-";
            oss << static_cast<int>(ports[i]);
        }
        qcomdevkey = oss.str();
        // QCD_Printf(Info, "Port Path: %s", qcomdevkey.c_str());
    }
#else
    qcomdevkey = to_string(busno) + "-" + to_string(devaddr);
#endif
    //QCD_Printf(Info, "%s: qcomdevkey: %s\n", __func__, qcomdevkey.c_str());
    return qcomdevkey;
}

void remove_qcomdev_ctx_mapping(char *serialNumMsm, libusb_device* dev)
{
    QcomLibUSBdevCtx* qcom_dev = nullptr;
    string qcomdevkey = "";

    qcomdevkey = create_qcomdev_key(serialNumMsm, dev);
    unique_lock<mutex> lock(qcomdevmap_mutex);
    auto iter = qcomdevmap.find(qcomdevkey);
    if (iter != qcomdevmap.end()) {
        qcom_dev = iter->second;
        QCD_Printf(Info, "%s: found qcomDevKey=%s, qcom_dev->dev: 0x%p, libusb dev: 0x%p\n",
            __func__, qcom_dev->qcomDevKey, qcom_dev->dev, dev);
        // ensure to free all QcomDeviceInfo entries, iterate deviceListHead
        if (qcom_dev->dev != nullptr && qcom_dev->dev == dev) {
            remove_device(qcom_dev, qcom_dev->dev, nullptr);
            fn_libusb_unref_device(qcom_dev->dev);
            QCD_Printf(Info, "%s: unref libusb_device *dev: 0x%p (qcomDevKey=%s)\n",
                __func__, qcom_dev->dev, qcom_dev->qcomDevKey);
            qcom_dev->dev = nullptr;
            qcomdevmap.erase(iter);
            /* qcom_dev is allocated with new operator, free memory using delete which run the 
            destructors internally for (mutex,condition_variable,atomic) */
            delete qcom_dev;
            qcom_dev = nullptr;
        }
        //system("rm -rf /dev/QCOM_LIBUSB*");
    } else{
        QCD_Printf(Info, "%s: qcom_dev is null:  %s\n", __func__, qcomdevkey.c_str());
    }
    lock.unlock();
}

bool parse_bus_devaddr_from_devdesc(char *devdesc, uint8_t* busno, uint8_t* devaddr)
{
    if (!devdesc || !busno || !devaddr)
        return false;
    
    const char* p = strrchr(devdesc, '_');
    if (!p)
        return false;

    ++p;
    char* end = nullptr;
    long b = strtol(p, &end, 10);
    if (!end || *end != '-')
        return false;

    long d = strtol(end+1, &end, 10);
    if (!end || *end != ':')
        return false;

    if (b < 0 || b > 255 || d < 0 || d > 255)
        return false;
    
    *busno = static_cast<uint8_t>(b);
    *devaddr = static_cast<uint8_t>(d);

    return true;
}

QcomLibUSBdevCtx *get_qcomdev_ctx_mapping(char* serialNumMsm, libusb_device* dev)
{
    QcomLibUSBdevCtx* qcom_dev = nullptr;
    //string serialKey(serialNumMsm);
    string qcomdevkey = create_qcomdev_key(serialNumMsm, dev);

    auto iter = qcomdevmap.find(qcomdevkey);
    if (iter == qcomdevmap.end()) {
        // new invokes default constructors safely for non-trivially-constructible members (mutex, condition_variable, atomic<int>).
        qcom_dev = new (std::nothrow) QcomLibUSBdevCtx;
        if (!qcom_dev) {
            QCD_Printf(Error, "%s: Failed to allocate memory for qcom_dev\n", __func__);
            return nullptr;
        }
        qcom_dev->dev_handle  = nullptr;
        qcom_dev->dev         = nullptr;
        qcom_dev->num_devices = 0;
        qcom_dev->ref_count   = 0;
        qcom_dev->active_transfer.store(0);
        qcom_dev->SerNumMSM[0]  = '\0';
        qcom_dev->qcomDevKey[0] = '\0';
#ifdef QCOM_WIN_ENV
        StringCchCopyA(qcom_dev->qcomDevKey, sizeof(qcom_dev->qcomDevKey), qcomdevkey.c_str());
#else
        strncpy(qcom_dev->qcomDevKey, qcomdevkey.c_str(), sizeof(qcom_dev->qcomDevKey) - 1);
        qcom_dev->qcomDevKey[sizeof(qcom_dev->qcomDevKey) - 1] = '\0';
#endif
        InitializeListHead(&qcom_dev->deviceListHead);
        InitializeListHead(&qcom_dev->ArrivalListLibusb);

        qcom_dev->num_devices++;
#ifdef QCOM_WIN_ENV
        if (serialNumMsm != nullptr) {
            StringCchCopyA(qcom_dev->SerNumMSM, 128, serialNumMsm);
        }
#else
        if (serialNumMsm != nullptr) {
            strncpy(qcom_dev->SerNumMSM, serialNumMsm, QCOM_MAX_STRING_SIZE - 1);
            qcom_dev->SerNumMSM[QCOM_MAX_STRING_SIZE - 1] = '\0';
        }
#endif
        qcomdevmap.emplace(qcomdevkey, qcom_dev);
        QCD_Printf(Info, "%s: New qcom_dev allocated: 0x%p and mapped with qcomdevkey: %s, SerNumMSM: %s\n",
            __func__, qcom_dev, qcomdevkey.c_str(), (char *)qcom_dev->SerNumMSM);
    } else {
        qcom_dev = iter->second;
        QCD_Printf(Info, "%s: found qcom_dev: 0x%p ,mapped with qcomdevkey: %s, SerNumMSM: %s\n",
            __func__, qcom_dev, qcomdevkey.c_str(), (char *)qcom_dev->SerNumMSM);
    }

    if (qcom_dev->dev)
        clearLibusbDeviceCtx(qcom_dev);

    if (qcom_dev && qcom_dev->dev != dev) {
        if (qcom_dev->active_transfer.load() != 0) {
            QCD_Printf(Info,
                "%s: deferring dev swap at key=%s active_transfer=%d old=0x%p new=0x%p\n",
                __func__, qcom_dev->qcomDevKey,
                qcom_dev->active_transfer.load(), qcom_dev->dev, dev);
            return qcom_dev;
        }

        QCD_Printf(Info, "%s: dev swap at key=%s old=0x%p new=0x%p\n",
            __func__, qcom_dev->qcomDevKey, qcom_dev->dev, dev);

        if (qcom_dev->dev != nullptr) {
            fn_libusb_unref_device(qcom_dev->dev);
            QCD_Printf(Info, "%s: unref previous libusb_device *dev: 0x%p (key=%s)\n",
                __func__, qcom_dev->dev, qcom_dev->qcomDevKey);
        }
        qcom_dev->dev = dev;
        fn_libusb_ref_device(dev);
    }

    return qcom_dev;
}

BOOL process_device(libusb_device* dev, void* deviceCtx) {
    QcomLibUSBdevCtx* qcom_dev = nullptr;
    struct libusb_device_descriptor desc;
    struct libusb_config_descriptor* config = nullptr;
    struct libusb_device_handle* handle = nullptr;
    const struct libusb_interface_descriptor* altsetting = nullptr;
    const struct libusb_endpoint_descriptor* endpoint = nullptr;
    CHAR serialNoMSM[QCOM_MAX_STRING_SIZE];
    char epm_device[QCOM_MAX_STRING_SIZE];
    char node_path[QCOM_MAX_STRING_SIZE];
    CHAR serialNo[QCOM_MAX_STRING_SIZE];
    const int max_open_tries = 3;
    PCHAR value = nullptr;
    int isQcDriver = 1;
    int open_tries = 0;
    int tmp_count = 0;
    int ret = -1;

    // Get device descriptor
    ret = fn_libusb_get_device_descriptor(dev, &desc);
    if (ret < 0) {
        QCD_Printf(Error, "%s: (%s) Failed to get device descriptor\n", __func__, create_qcomdev_key(nullptr, dev).c_str());
        return ret;
    }

    // Match Vendor ID
    if (desc.idVendor != QCOM_VENDOR_ID) {
        return LIBUSB_ERROR_NOT_SUPPORTED;
    }

#ifdef QCOM_WIN_ENV
    ZeroMemory(serialNo, QCOM_MAX_STRING_SIZE);
    ZeroMemory(serialNoMSM, QCOM_MAX_STRING_SIZE);
    ZeroMemory(epm_device, QCOM_MAX_STRING_SIZE);
#else
    PDeviceCtx pdeviceCtx = (PDeviceCtx)deviceCtx;
    memset(serialNo, 0, QCOM_MAX_STRING_SIZE);
    memset(serialNoMSM, 0, QCOM_MAX_STRING_SIZE);
    memset(epm_device, 0, QCOM_MAX_STRING_SIZE);
#endif
//    QCD_Printf(Info, "%s: VID:PID: %04X:%04X\n", __func__, desc.idVendor, desc.idProduct);
//    QCD_Printf(Info, "%s: bus: %d and device_addr: %d, portNo: %d\n", __func__,
    //    libusb_get_bus_number(dev), libusb_get_device_address(dev), libusb_get_port_number(dev));

    char VidString[5];
    char PidString[5];
    snprintf(VidString, sizeof(VidString), "%04X", desc.idVendor);
    snprintf(PidString, sizeof(PidString), "%04X", desc.idProduct);

    if (strstr(PidString, "9301") != nullptr) {
        QCD_Printf(Info, "%s: enumeration of %s is not supported\n", __func__, PidString);
        return LIBUSB_ERROR_NOT_SUPPORTED;
    }

    if (strstr(PidString, "9302") != nullptr) {
        QCD_Printf(Info, "%s: enumeration of %s is not supported\n", __func__, PidString);
        return LIBUSB_ERROR_NOT_SUPPORTED;
    }

    while ((ret = fn_libusb_open(dev, &handle)) < 0) {
        if (ret != LIBUSB_ERROR_ACCESS && ret != LIBUSB_ERROR_BUSY) {
            break;
        }
        if (++open_tries >= max_open_tries) {
            break;
        }
        QCD_Printf(Info, "%s: (%s) fn_libusb_open retry %d/%d ret=%s\n", __func__,
            create_qcomdev_key(nullptr, dev).c_str(), open_tries, max_open_tries, fn_libusb_error_name(ret));
#ifdef QCOM_WIN_ENV
            Sleep(100);
#else
            usleep(100000);
#endif
    }

    if (ret < 0) {
        QCD_Printf(Error, "%s: Device is in use! failed to open device: (%s): %s : libusb *dev ctx: 0x%p, desc: 0x%p\n",
            __func__, create_qcomdev_key(nullptr, dev).c_str(), fn_libusb_error_name(ret), dev, desc);
        return ret;
    }
    
    QCD_Printf(Info, "%s: (%s) libusb_device *dev: 0x%p, idVendor: 0x%04X, idProduct: 0x%04X\n", __func__, create_qcomdev_key(nullptr, dev).c_str(),
    dev, desc.idVendor, desc.idProduct);

    if (desc.iSerialNumber) {
        ret = fn_libusb_get_string_descriptor_ascii(handle, desc.iSerialNumber, (PUCHAR)serialNo, sizeof(serialNo));
        if (ret < 0) {
            QCD_Printf(Error, "%s: Failed to get iSerialNumber descriptor: %s\n", __func__, fn_libusb_strerror((libusb_error)ret));
            fn_libusb_close(handle);
            return ret;
        }
    }
    // QCD_Printf(Info,"serNum (adbID) = %s\n", serialNo);
    // set serNum value in registry for QC_SPEC_SERNUM

    if (desc.iProduct) {
        ret = fn_libusb_get_string_descriptor_ascii(handle, desc.iProduct, (PUCHAR)serialNoMSM, sizeof(serialNoMSM));
        if (ret < 0) {
            QCD_Printf(Error, "%s: Failed to get iProduct descriptor: %s\n", __func__, fn_libusb_strerror((libusb_error)ret));
            fn_libusb_close(handle);
            return ret;
        }
    }
    value = strstr(serialNoMSM, "_SN:");
    
    strncpy(epm_device, serialNoMSM, QCOM_MAX_STRING_SIZE - 1);
    epm_device[QCOM_MAX_STRING_SIZE - 1] = '\0';
    if (value) {
        char delimiter[] = "_ ";
        value += strlen("_SN:");
        /* To extract the product serial info */
        if (value = strtok_r(value, delimiter, &value)) {
            //QCD_Printf(Info, "serNumMSM = %s\n", value);
        }
    }
    // set serNumMSM value in registry for QC_SPEC_SERNUM_MSM

    fn_libusb_close(handle);
    
    unique_lock<mutex> lock(qcomdevmap_mutex);
    qcom_dev = get_qcomdev_ctx_mapping(value, dev);
    lock.unlock();

    if (!qcom_dev) {
        QCD_Printf(Error, "%s: Failed: allocated memory for qcom_dev is null\n", __func__);
        return LIBUSB_ERROR_NO_MEM;
    }

    if (strstr(epm_device, "Embedded Power Measurement (EPM) device") != nullptr) {
       QCD_Printf(Info, "%s: enumeration of %s is not supported\n", __func__, epm_device);
        return LIBUSB_ERROR_NOT_SUPPORTED;
    }

    if (strstr(epm_device, "DEBUG BOARD") != nullptr) {
        QCD_Printf(Info, "%s: enumeration of %s is not supported\n", __func__, epm_device);
        return LIBUSB_ERROR_NOT_SUPPORTED;
    }

    // Get active configuration descriptor
    ret = fn_libusb_get_active_config_descriptor(dev, &config);
    if (ret < 0) {
        fprintf(stderr, "Failed to get configuration descriptor: %s\n", fn_libusb_error_name(ret));
        return ret;
    }

    // Iterate through interfaces
    for (int i = 0; i < config->bNumInterfaces; i++) {
        // Iterate through alternate settings
        altsetting = config->interface[i].altsetting;

        PQcomDeviceInfo NewDeviceInf;

        NewDeviceInf = (PQcomDeviceInfo)malloc(sizeof(QcomDeviceInfo));
        if (!NewDeviceInf)
        {
            QCD_Printf(Fatal, "Malloc failed\n");
            fn_libusb_free_config_descriptor(config);
            return LIBUSB_ERROR_NO_MEM;
        }
        InitializeListHead(&NewDeviceInf->List);

#ifdef QCOM_WIN_ENV
        ZeroMemory(NewDeviceInf, sizeof(QcomDeviceInfo));
#else
        memset(NewDeviceInf, 0, sizeof(QcomDeviceInfo));
#endif

        NewDeviceInf->interfaceNumber = altsetting->bInterfaceNumber;
        NewDeviceInf->devCtx = qcom_dev;
        for (int j = 0; j < altsetting->bNumEndpoints; j++) {
            endpoint = &altsetting->endpoint[j];
            if ((endpoint->bmAttributes & LIBUSB_TRANSFER_TYPE_MASK) == LIBUSB_TRANSFER_TYPE_BULK) {
                if (endpoint->bEndpointAddress & LIBUSB_ENDPOINT_IN) {
                    NewDeviceInf->bulk_in_endpointAddr = endpoint->bEndpointAddress;
                    NewDeviceInf->bulk_in_size = endpoint->wMaxPacketSize;
                    QCD_Printf(Debug,"Bulk In Endpoint: %X, Max packet size: %d\n", endpoint->bEndpointAddress, endpoint->wMaxPacketSize);
                } else {
                    NewDeviceInf->bulk_out_endpointAddr = endpoint->bEndpointAddress;
                    NewDeviceInf->bulk_out_size = endpoint->wMaxPacketSize;
                    QCD_Printf(Debug,"Bulk Out Endpoint: %X, Max packet size: %d\n", endpoint->bEndpointAddress, endpoint->wMaxPacketSize);
                }
            } else if ((endpoint->bmAttributes & LIBUSB_TRANSFER_TYPE_MASK) == LIBUSB_TRANSFER_TYPE_INTERRUPT) {
                 //QCD_Printf(Info,"Interrupt Endpoint: %X, Max packet size: %d\n", endpoint->bEndpointAddress, endpoint->wMaxPacketSize);
            }
        }

#ifdef QCOM_WIN_ENV
	    if(serialNo != NULL) {
	    	StringCchCopy((PTSTR)NewDeviceInf->SerNum, 128, (PTSTR)serialNo);
	    }
        if(value != NULL) {
	    	StringCchCopy((PTSTR)NewDeviceInf->SerNumMSM, 128, (PTSTR)value);
	    }
        // Copy full iProduct descriptor string to DevDetails
        if(epm_device[0] != '\0') {
            StringCchCopy((PTSTR)NewDeviceInf->DevDetails, QCOM_MAX_STRING_SIZE, (PTSTR)epm_device);
        }
        NewDeviceInf->protocol = altsetting->bInterfaceProtocol;
        InsertTailList(&qcom_dev->deviceListHead, &NewDeviceInf->List);
#else
    std::unordered_map<std::string, std::string> devDetailsMP;
    devDetailsMP.clear();
    if (serialNo != NULL) {
        strncpy(NewDeviceInf->SerNum, serialNo, QCOM_MAX_STRING_SIZE - 1);
        NewDeviceInf->SerNum[QCOM_MAX_STRING_SIZE - 1] = '\0';
    }
    if (value != NULL) {
        strncpy(NewDeviceInf->SerNumMSM, value, QCOM_MAX_STRING_SIZE - 1);
        NewDeviceInf->SerNumMSM[QCOM_MAX_STRING_SIZE - 1] = '\0';
    }
    // Copy full iProduct descriptor string to DevDetails
    if (epm_device[0] != '\0') {
        strncpy(NewDeviceInf->DevDetails, epm_device, QCOM_MAX_STRING_SIZE - 1);
        NewDeviceInf->DevDetails[QCOM_MAX_STRING_SIZE - 1] = '\0';
    }
    if (pdeviceCtx != NULL) {
        if (pdeviceCtx->mDevParams.DevPath) {
            strncpy(NewDeviceInf->DevPath, pdeviceCtx->mDevParams.DevPath, QCOM_MAX_STRING_SIZE - 1);
            NewDeviceInf->DevPath[QCOM_MAX_STRING_SIZE - 1] = '\0';
        }
        if (pdeviceCtx->mDevParams.Loc) {
            strncpy(NewDeviceInf->Loc, pdeviceCtx->mDevParams.Loc, QCOM_MAX_STRING_SIZE - 1);
            NewDeviceInf->Loc[QCOM_MAX_STRING_SIZE - 1] = '\0';
        }
        if (pdeviceCtx->mDevParams.HwId) {
            strncpy(NewDeviceInf->HwId, pdeviceCtx->mDevParams.HwId, QCOM_MAX_STRING_SIZE - 1);
            NewDeviceInf->HwId[QCOM_MAX_STRING_SIZE - 1] = '\0';
        }
        if (pdeviceCtx->mDevParams.ParentDev) {
            strncpy(NewDeviceInf->ParentDev, pdeviceCtx->mDevParams.ParentDev, QCOM_MAX_STRING_SIZE - 1);
            NewDeviceInf->ParentDev[QCOM_MAX_STRING_SIZE - 1] = '\0';
        }
        if (pdeviceCtx->mDevParams.ParentLocationInfomation) {
            strncpy(NewDeviceInf->ParentLocationInformation, pdeviceCtx->mDevParams.ParentLocationInfomation,   QCOM_MAX_STRING_SIZE - 1);
            NewDeviceInf->ParentLocationInformation[QCOM_MAX_STRING_SIZE - 1] = '\0';
        }
        NewDeviceInf->DevDetailsMP = new std::unordered_map<std::string, std::string>();
        ParseDevDesc(NewDeviceInf->DevDetails, devDetailsMP);
        if(NewDeviceInf->DevDetailsMP)
        {
            *(NewDeviceInf->DevDetailsMP) = devDetailsMP;
        }
    }
    NewDeviceInf->protocol = altsetting->bInterfaceProtocol |
                             altsetting->bInterfaceClass << 8 |
                             altsetting->bAlternateSetting << 16 |
                             altsetting->bInterfaceNumber << 24;
    NewDeviceInf->Flag = ((ULONG)QC_DEV_TYPE_USB << 8) | (QC_DEV_STATE_ARRIVAL << 4) | ((ULONG)QC_DEV_BUS_TYPE_USB << 16) | (isQcDriver);

        const char *devname = fetchFriendlyDeviceName(altsetting->bInterfaceProtocol, PidString);
        if (devname != nullptr &&
            strstr(epm_device, "Embedded Power Measurement (EPM) device") == nullptr) {
                //QCD_Printf(Info, "Check count: %d\n", ++tmp_count);
            create_libusb_devnode(NewDeviceInf, devname, PidString,
                                  fn_libusb_get_bus_number(dev),
                                  fn_libusb_get_device_address(dev),
                                  config->bConfigurationValue,
                                  altsetting->bInterfaceNumber);
            if (NewDeviceInf->DevDesc[0] != NULL) {
                    strncpy(NewDeviceInf->DevName, NewDeviceInf->DevDesc, QCOM_MAX_STRING_SIZE - 1);
                    NewDeviceInf->DevName[QCOM_MAX_STRING_SIZE - 1] = '\0';
            }
        
            PushLibUSBToApplication(NewDeviceInf);
            InsertTailList(&qcom_dev->deviceListHead, &NewDeviceInf->List);
        }
#endif

        //QCD_Printf(Info,"%s: p:%d, b:%d-%d\n", __func__, altsetting->bInterfaceProtocol, libusb_get_bus_number(dev), libusb_get_device_address(dev));

    }

    display_devices(qcom_dev);

#ifdef QCOM_WIN_ENV
    update_arrival_list_libusb(qcom_dev);
#endif

    fn_libusb_free_config_descriptor(config);

    return 0;
}

BOOL enumerate_devices()
{
    CHECK_LIBUSB_LOADED(EXIT_FAILURE);
    struct libusb_device** dev_list = nullptr;
    ssize_t cnt;
    int i;

    // Get the list of USB devices
    cnt = fn_libusb_get_device_list(g_ctx, &dev_list);
    if (cnt < 0) {
        QCD_Printf(Error, "Error getting USB device list\n");
        fn_libusb_exit(g_ctx);
        return EXIT_FAILURE;
    }
    QCD_Printf(Info, "%s: count = %d\n", __func__, cnt);
    
    // Iterate through the devices
    for (i = 0; i < cnt; i++) {
        process_device(dev_list[i], nullptr);
    }

    // Cleanup
    fn_libusb_free_device_list(dev_list, 1);

    return 0;
}

/* libusb core device communication API's */

#ifdef QCOM_LNX_ENV
PQcomDeviceInfo find_device(PQcomLibUSBdevCtx devContext, const char* devDesc)
{
    PLIST_ENTRY head = &devContext->deviceListHead;
    PLIST_ENTRY peek = head->Flink;
    while (peek != head)
    {
        QcomDeviceInfo* device = CONTAINING_RECORD(peek, QcomDeviceInfo, List);
        if (strncmp(devDesc, device->DevDesc, QCOM_MAX_STRING_SIZE) == 0) {
            QCD_Printf(Info, "%s: lnx: device is return: %s\n", __func__, devDesc);
            return device;
        }
        peek = peek->Flink;
    }
    return nullptr;
}
#endif

#ifdef QCOM_WIN_ENV
VOID update_arrival_list_libusb(QcomLibUSBdevCtx* qcom_dev)
{
    PLIST_ENTRY head = &qcom_dev->deviceListHead;
    PLIST_ENTRY head_arrival = &qcom_dev->ArrivalListLibusb;
    PLIST_ENTRY pos_arrival = head_arrival->Flink;
    PLIST_ENTRY pos = head->Flink;
    PQcomDeviceInfo NewDevArrival = nullptr;
    QcomDeviceInfo* pDevInfo = nullptr;
    int count = 0;

    if (IsListEmpty(head)) {
        if (!IsListEmpty(head_arrival)) {
    /* Cleanup arrival list if main list is empty */
            pos_arrival = head_arrival->Flink;
            while (pos_arrival != head_arrival) {
                PLIST_ENTRY next = pos_arrival->Flink;
                QcomDeviceInfo* pDevInfoFree = CONTAINING_RECORD(pos_arrival, QcomDeviceInfo, List);
                if (pDevInfoFree->DevDetailsMP) {
                    delete pDevInfoFree->DevDetailsMP;
                    pDevInfoFree->DevDetailsMP = nullptr;
                }
                RemoveEntryList(pos_arrival);
                free(pDevInfoFree);
                pos_arrival = next;
            }
        QCD_Printf(Info, "%s: Cleared ArrivalListLibusb\n", __func__);
        }
        return;
    }

    pos = head->Flink;
    pos_arrival = head_arrival->Flink;
    bool isFirstTimeArrival = IsListEmpty(head_arrival);

    while (pos != head)
    {
        pDevInfo = CONTAINING_RECORD(pos, QcomDeviceInfo, List);
        pos = pos->Flink;
        bool modeChangeDetected = false;
        bool skipSameData = false;
        //pos_arrival = head_arrival->Flink;

        if (!isFirstTimeArrival) {
            pos_arrival = head_arrival->Flink;
            while (pos_arrival != head_arrival) {
                QcomDeviceInfo* pDevExist = CONTAINING_RECORD(pos_arrival, QcomDeviceInfo, List);
                PLIST_ENTRY next = pos_arrival->Flink;

                if (strncmp(pDevExist->SerNumMSM, pDevInfo->SerNumMSM, QCOM_MAX_STRING_SIZE) == 0) {

                    bool isOldCrash = (pDevExist->protocol == INTERFACE_SAHARA_PROTOCOL || pDevExist->protocol == INTERFACE_EDL_PROTOCOL);
                    bool isNewCrash = (pDevInfo->protocol == INTERFACE_SAHARA_PROTOCOL || pDevInfo->protocol == INTERFACE_EDL_PROTOCOL);
                    bool isOldNormal = (pDevExist->protocol != INTERFACE_SAHARA_PROTOCOL && pDevExist->protocol != INTERFACE_EDL_PROTOCOL);
                    bool isNewNormal = (pDevInfo->protocol != INTERFACE_SAHARA_PROTOCOL && pDevInfo->protocol != INTERFACE_EDL_PROTOCOL);

                    if ((isOldCrash && isNewNormal) || (isOldNormal && isNewCrash))
                    {
                        QCD_Printf(Info, "%s: Mode change detected!! Updating ArrivalListLibusb\n", __func__);
                        QCD_Printf(Info, "%s: SerNumMSM: %s, new protocol: %d, removing old protocol: %d\n",
                            __func__, pDevExist->SerNumMSM, pDevInfo->protocol, pDevExist->protocol);
                        //PLIST_ENTRY next = pos_arrival->Flink;
                        if (pDevExist->DevDetailsMP) {
                            delete pDevExist->DevDetailsMP;
                            pDevExist->DevDetailsMP = nullptr;
                        }
                        RemoveEntryList(pos_arrival);
                        free(pDevExist);
                        pos_arrival = next;
                        modeChangeDetected = true;
                        continue;
                    }
                    if (pDevExist->protocol == pDevInfo->protocol || pDevExist->interfaceNumber == pDevInfo->interfaceNumber) {
                        //QCD_Printf(Info, "%s: skip same data\n", __func__);
                        skipSameData = true;
                        break;
                    }
                }
                pos_arrival = next;
            }
            if (!modeChangeDetected && skipSameData)
                continue;
        }

        NewDevArrival = (PQcomDeviceInfo)malloc(sizeof(QcomDeviceInfo));
        if (NewDevArrival) {
            memcpy(NewDevArrival, pDevInfo, sizeof(QcomDeviceInfo));
            InitializeListHead(&NewDevArrival->List);
            InsertTailList(&qcom_dev->ArrivalListLibusb, &NewDevArrival->List);
            count++;
            QCD_Printf(Info, "%s: cn:%d pDevInfo: 0x%p, NewDevArrival: 0x%p, SerNumMsm: %s, InfNo: %d, protocol: %d\n",
                __func__, count, pDevInfo, NewDevArrival, NewDevArrival->SerNumMSM, NewDevArrival->interfaceNumber, NewDevArrival->protocol);
        }
    }
}

PQcomDeviceInfo find_device(PQcomLibUSBdevCtx devContext, const char *SernumMsm, unsigned int bInterfaceNumber)
{
    PLIST_ENTRY head = &devContext->ArrivalListLibusb;
    PLIST_ENTRY peek = head->Flink;

    while (peek != head)
    {
        QcomDeviceInfo* device = CONTAINING_RECORD(peek, QcomDeviceInfo, List);
        if (strncmp(SernumMsm, device->SerNumMSM, QCOM_MAX_STRING_SIZE) == 0) {
            if (bInterfaceNumber == device->interfaceNumber)
            {
                return device;
            }
        }
        peek = peek->Flink;
    }
    return nullptr;
}

PQcomDeviceInfo find_device(PQcomLibUSBdevCtx devContext, const char *SernumMsm, const char* devDesc)
{
    PLIST_ENTRY head = &devContext->ArrivalListLibusb;
    PLIST_ENTRY peek = head->Flink;

    while (peek != head)
    {
        QcomDeviceInfo* device = CONTAINING_RECORD(peek, QcomDeviceInfo, List);

        if (strncmp(SernumMsm, device->SerNumMSM, QCOM_MAX_STRING_SIZE) == 0) {
            QCD_Printf(Info, "%s: %s:%s, serialNumMSM:scandev %s:%s\n",__func__, devDesc, device->DevDesc, device->SerNumMSM, SernumMsm);
            if (get_interface_ctx(devDesc, device->protocol))
            {
                QCD_Printf(Info, "%s: returned device interface ctx: %s\n", __func__, devDesc);
                return device;
            }
        }
        peek = peek->Flink;
    }
    return nullptr;
}

BOOL QcomLibusbDevice::qcom_libusb_open(char *SernumMsm, unsigned int bInterfaceNumber, void** devHandle)
{
    CHECK_LIBUSB_LOADED(LIBUSB_ERROR_OTHER);
    QcomLibUSBdevCtx* qcom_dev = nullptr;
    string qcomdevkey = "";
    qcomdevkey = SernumMsm;

    qcom_dev = get_qcomdev_ctx_map(qcomdevkey);
    QCD_Printf(Info, "%s _bInterfaceNumber: SernumMsm= %s, qcom_dev: 0x%p\n", __func__, SernumMsm, qcom_dev);
    if (qcom_dev == nullptr) {
        return LIBUSB_ERROR_NOT_FOUND;
    }

    QcomDeviceInfo* qcomDevInfo = find_device(qcom_dev, qcom_dev->SerNumMSM, bInterfaceNumber);
    return qcom_libusb_open_internal(qcomDevInfo, devHandle);
}
#endif

BOOL QcomLibusbDevice::qcom_libusb_open(char* DeviceName, char* SernumMsm, void** devHandle)
{
    CHECK_LIBUSB_LOADED(LIBUSB_ERROR_OTHER);
    QcomLibUSBdevCtx* qcom_dev = nullptr;
    string qcomdevkey = "";

#ifdef QCOM_WIN_ENV
    qcomdevkey = SernumMsm;
    qcom_dev = get_qcomdev_ctx_map(qcomdevkey);
#else
    uint8_t busno, devaddr;
    parse_bus_devaddr_from_devdesc(DeviceName, &busno, &devaddr);
    qcomdevkey = to_string(busno) + "-" + to_string(devaddr);
    qcom_dev = get_qcomdev_ctx_map(qcomdevkey);
#endif
    QCD_Printf(Info, "%s: (key=%s) DeviceName= %s, qcom_dev: 0x%p\n", __func__, qcomdevkey.c_str(), DeviceName, qcom_dev);
    if (qcom_dev == nullptr) {
        return LIBUSB_ERROR_NOT_FOUND;
    }
#ifdef QCOM_WIN_ENV
    QcomDeviceInfo* qcomDevInfo = find_device(qcom_dev, qcom_dev->SerNumMSM, DeviceName);
#else
    QcomDeviceInfo* qcomDevInfo = find_device(qcom_dev, DeviceName);
#endif
    return qcom_libusb_open_internal(qcomDevInfo, devHandle);
}

BOOL qcom_libusb_open_internal(QcomDeviceInfo* qcomDevInfo, void** devHandle)
{
    if (qcomDevInfo == nullptr) {
        QCD_Printf(Info, "%s: qcomDevInfo is NULL\n", __func__);
        return LIBUSB_ERROR_NOT_FOUND;
    }
    
    PQcomLibUSBdevCtx qcom_dev = qcomDevInfo->devCtx;
    int ret = LIBUSB_ERROR_OTHER;

    if (qcom_dev == nullptr || qcomDevInfo->devCtx == nullptr)
    {
       QCD_Printf(Info, "%s: qcom_dev = 0x%p, or devCtx = 0x%p is NULL\n", __func__, qcom_dev, qcomDevInfo->devCtx);
        return LIBUSB_ERROR_INVALID_PARAM;
    }

    QCD_Printf(Info, "%s: (key=%s) libusb_dev: 0x%p, interface number: %d\n", __func__, qcom_dev->qcomDevKey, qcom_dev->dev, qcomDevInfo->interfaceNumber);

    if (qcom_dev->dev == nullptr) {
        QCD_Printf(Info, "%s: libusb_dev is NULL, wait! device gone\n", __func__);
        return LIBUSB_ERROR_NO_DEVICE;
    }

    if (qcom_dev->dev_handle == nullptr) {
        QCD_Printf(Info, "%s: Open started..\n", __func__);
        ret = fn_libusb_open(qcom_dev->dev, &(qcom_dev->dev_handle));
        if (ret != LIBUSB_SUCCESS) {
            QCD_Printf(Error, "%s: failed to open interface. status: %s\n", __func__, fn_libusb_error_name(ret));
            return ret;
        }
        else {
            QCD_Printf(Info, "%s: ret-status: %s and dev_handle: %p, libusb_dev: 0x%p\n", __func__, fn_libusb_error_name(ret), qcom_dev->dev_handle, qcom_dev->dev);
        }
    }

    if (fn_libusb_kernel_driver_active(qcom_dev->dev_handle, qcomDevInfo->interfaceNumber) == 1) {
        QCD_Printf(Info, "%s: kernel driver is active\n", __func__);
        ret = fn_libusb_detach_kernel_driver(qcom_dev->dev_handle, qcomDevInfo->interfaceNumber);
        if (ret < 0) {
            QCD_Printf(Error, "%s: failed to detach kernel driver: %s\n", __func__, fn_libusb_error_name(ret));
            fn_libusb_close(qcom_dev->dev_handle);
            *devHandle = nullptr;
            qcom_dev->dev_handle = nullptr;
            return ret;
        } else {
            QCD_Printf(Info, "%s: detached active kernel driver\n", __func__);
        }
    }

       // Now, we claim the interface
    ret = fn_libusb_claim_interface(qcom_dev->dev_handle, qcomDevInfo->interfaceNumber);
    if (ret < 0) {
        QCD_Printf(Error, "%s: failed to claim interface: %s\n", __func__, fn_libusb_error_name(ret));
        //fn_libusb_close(qcom_dev->dev_handle);
        *devHandle = nullptr;
        //qcom_dev->dev_handle = nullptr;
        return ret;
    }
    
    qcomDevInfo->virtual_handle = static_cast<uint64_t>((uintptr_t)qcom_dev->dev_handle + (4 * qcomDevInfo->interfaceNumber) + 4);
    unique_lock<mutex> maplock(dev_handle_map_mutex);
    dev_handle_map.insert(make_pair(qcomDevInfo->virtual_handle, qcomDevInfo));
    maplock.unlock();
    *devHandle = reinterpret_cast<void *>(qcomDevInfo->virtual_handle);
    unique_lock<mutex> lock(qcom_dev->dev_handle_mutex);
    qcom_dev->ref_count++; // this need mutex protection or atomicity
    lock.unlock();

    QCD_Printf(Info, "%s: (key=%s) device opened and interface claimed successfully. ref_count: %d\n",
        __func__, qcom_dev->qcomDevKey, qcom_dev->ref_count);
    QCD_Printf(Info, "%s: (key=%s) dev: 0x%p, qcomDevInfo: 0x%p, IntfNo: %d, VirtualHandle : 0x%llx\n",
        __func__, qcom_dev->qcomDevKey, qcom_dev->dev, qcomDevInfo, qcomDevInfo->interfaceNumber, qcomDevInfo->virtual_handle);

    return ret;
}

QcomLibUSBdevCtx* get_qcomdev_ctx_map(string qcomDevKey)
{
    QcomLibUSBdevCtx* qcom_dev = nullptr;

    auto iter = qcomdevmap.find(qcomDevKey);
    if (iter != qcomdevmap.end()) {
        QCD_Printf(Debug, "%s: found Serial/bus key: %s\n", __func__, qcomDevKey.c_str());
        qcom_dev = iter->second;
    } else {
        QCD_Printf(Error, "%s: Serial/bus key Null\n", __func__);
    }

    return qcom_dev;
}

/* dev_handle_map_mutex must be held by the caller while the returned
 * iterator is dereferenced or used to erase(). Helpers that only read the
 * mapped value copy it out before returning.
 */
libusb_handle_map_iter fetchDeviceCtxfromMap(uint64_t Handle)
{
    auto iter = dev_handle_map.find(Handle);
    if (iter == dev_handle_map.end()) {
        QCD_Printf(Info, "Invalid device handle: 0x%llx\n", Handle);
    }
    return iter;
}

VOID QcomLibusbDevice::qcom_libusb_close(void** devHandle)
{
    CHECK_LIBUSB_LOADED_VOID();
    int ret = 0;

    if (*devHandle == nullptr)
    {
        QCD_Printf(Error, "%s: Error: invalid devHandle\n", __func__);
        return;
    }

    uint64_t virtualHandle = reinterpret_cast<uint64_t>(*devHandle);

    QCD_Printf(Info, "%s: Verify virtual Handle: 0x%llx\n", __func__, virtualHandle);
    if (virtualHandle == 0) {
        QCD_Printf(Error, "Error: invalid virtual Handle: 0x%llx\n", virtualHandle);
        return;
    }

    unique_lock<mutex> maplock(dev_handle_map_mutex);
    auto iter = fetchDeviceCtxfromMap(virtualHandle);
    if (iter == dev_handle_map.end()) {
        maplock.unlock();
        return;
    }

    QcomDeviceInfo* qcomDevInfo = iter->second;
    PQcomLibUSBdevCtx qcom_dev = qcomDevInfo->devCtx;
    maplock.unlock();

    QCD_Printf(Info, "%s: Close virtual Handle: 0x%llx, active_transfer: %d (key=%s)\n",
        __func__, virtualHandle, qcom_dev->active_transfer.load(), qcom_dev->qcomDevKey);

    unique_lock<mutex> lock(qcom_dev->dev_handle_mutex);
    qcom_dev->active_transfer_cv.wait(lock, [qcom_dev]{
        return qcom_dev->active_transfer.load() == 0;
    });

    if (qcomDevInfo != nullptr)
    {
            // PQcomLibUSBdevCtx devCtx = qcomDevInfo->devCtx;
            QCD_Printf(Info, "%s: (key=%s) dev: 0x%p, qcomDevInfo: 0x%p, InterfaceNo: %d\n", __func__, qcom_dev->qcomDevKey, qcom_dev->dev, qcomDevInfo, qcomDevInfo->interfaceNumber);
            ret = fn_libusb_release_interface(qcom_dev->dev_handle, qcomDevInfo->interfaceNumber); // need to verify
            if (ret != LIBUSB_SUCCESS) {
                QCD_Printf(Error, "failed to release interface: %s\n", fn_libusb_error_name(ret));
            }
            
            /* remove virtual handle entry from dev_handle_map*/
            maplock.lock();
            dev_handle_map.erase(virtualHandle);
            maplock.unlock();
            
            //qcomDevInfo->virtual_handle = 0;
            qcom_dev->ref_count--;

            if (qcom_dev->ref_count == 0) {
                QCD_Printf(Info, "%s: closing real libusb dev_handle: %p\n", __func__, qcom_dev->dev_handle);
                fn_libusb_close(qcom_dev->dev_handle);
                qcom_dev->dev_handle = nullptr;
#ifdef QCOM_WIN_ENV
                if (qcom_dev->dev) {
                    fn_libusb_unref_device(qcom_dev->dev);
                    QCD_Printf(Info, "%s: unref libusb_device *dev - qcom_dev->dev: 0x%p\n", __func__, qcom_dev->dev);
                    qcom_dev->dev = nullptr;
                }
#endif
            }
            QCD_Printf(Info, "%s: (key=%s) close virtual Handle: 0x%llx, remaining open interfaces: %d\n", __func__, qcom_dev->qcomDevKey, virtualHandle, qcom_dev->ref_count);
            *devHandle = nullptr;
    }
    else {
        QCD_Printf(Info, "QcomDeviceInfo context not available\n");
    }

    return;
}

BOOL QcomLibusbDevice::qcom_libusb_read
(
    void **devHandle,
    void* buffer,
    int bufferLen,
    int* bytesRead,
    unsigned timeOut
)
{
    int ret = LIBUSB_ERROR_OTHER;
    const int max_retries = 3;
    int attempt = 0;
    
    if (timeOut == 0)
	    timeOut = 2000;

    if (*devHandle == nullptr || buffer == nullptr || bufferLen == 0)
    {
        return LIBUSB_ERROR_INVALID_PARAM;
    }

    uint64_t virtualHandle = reinterpret_cast<uint64_t>(*devHandle);

    unique_lock<mutex> maplock(dev_handle_map_mutex);
    auto iter = fetchDeviceCtxfromMap(virtualHandle);
    if (iter == dev_handle_map.end()) {
        return LIBUSB_ERROR_NOT_FOUND;
    }

    QcomDeviceInfo* qcomDevInfo = iter->second;
    PQcomLibUSBdevCtx qcom_dev  = qcomDevInfo->devCtx;
    maplock.unlock();

    unique_lock<mutex> lock(qcom_dev->dev_handle_mutex);
    ++qcom_dev->active_transfer;
    lock.unlock();

    do {
        ret = fn_libusb_bulk_transfer(qcom_dev->dev_handle, qcomDevInfo->bulk_in_endpointAddr,
                                   reinterpret_cast<unsigned char*>(buffer), bufferLen, bytesRead, timeOut);
        QCD_Printf(Debug, "%s: key=(%s) Received bytes: %d/%d, ret: %s\n",
            __func__, qcom_dev->qcomDevKey, *bytesRead, bufferLen, fn_libusb_error_name(ret));
        if (ret != LIBUSB_SUCCESS)
        {
            QCD_Printf(Error, "%s: key=(%s) failed to read bulk transfer: %s\n", __func__, qcom_dev->qcomDevKey, fn_libusb_error_name(ret));
            if (ret == LIBUSB_TRANSFER_TIMED_OUT || ret == LIBUSB_ERROR_PIPE ||
                ret == LIBUSB_TRANSFER_CANCELLED || ret == LIBUSB_ERROR_TIMEOUT) {
                QCD_Printf(Info, "%s: key=(%s) retrying read (attempt %d/%d)\n", __func__, qcom_dev->qcomDevKey, attempt + 1, max_retries);
		if (ret == LIBUSB_ERROR_PIPE)
			fn_libusb_clear_halt(qcomDevInfo->devCtx->dev_handle, qcomDevInfo->bulk_in_endpointAddr);
#ifdef QCOM_WIN_ENV
                Sleep(500);
#else
                usleep(500000); /* 500ms delay between retries */
#endif
            } else {
                goto cleanup;
            }
        } else {
            break;
        }
    } while (++attempt < max_retries);

cleanup:
    lock.lock();
    --qcom_dev->active_transfer;
    lock.unlock();
    qcom_dev->active_transfer_cv.notify_all();
    return ret;
}

BOOL QcomLibusbDevice::qcom_libusb_write
(
    void** devHandle,
    void* buffer,
    int bufferLen,
    int* bytesSent,
    unsigned timeOut
)
{
    int ret = LIBUSB_ERROR_OTHER;
    const int max_retries = 3;
    int attempt = 0;
    
    if (timeOut == 0)
	    timeOut = 2000;
    
    if (*devHandle == nullptr || buffer == nullptr || bufferLen == 0)
    {
        return LIBUSB_ERROR_INVALID_PARAM;
    }

    uint64_t virtualHandle = reinterpret_cast<uint64_t>(*devHandle);
    
    unique_lock<mutex> maplock(dev_handle_map_mutex);
    auto iter = fetchDeviceCtxfromMap(virtualHandle);
    if (iter == dev_handle_map.end()) {
        maplock.unlock();
        return LIBUSB_ERROR_NOT_FOUND;
    }
    
    QcomDeviceInfo* qcomDevInfo = iter->second;
    PQcomLibUSBdevCtx qcom_dev  = qcomDevInfo->devCtx;
    maplock.unlock();

    unique_lock<mutex> lock(qcom_dev->dev_handle_mutex);
    ++qcom_dev->active_transfer;
    lock.unlock();

    do {
        ret = fn_libusb_bulk_transfer(qcom_dev->dev_handle, qcomDevInfo->bulk_out_endpointAddr,
                                   reinterpret_cast<unsigned char*>(buffer), bufferLen, bytesSent, timeOut);
        QCD_Printf(Debug, "%s: key=(%s) Sent bytes: %d/%d, ret: %s\n",
            __func__, qcom_dev->qcomDevKey, *bytesSent, bufferLen, fn_libusb_error_name(ret));
        if (ret != LIBUSB_SUCCESS)
        {
            QCD_Printf(Error, "%s: key=(%s) failed to write bulk transfer: %s\n", __func__, qcom_dev->qcomDevKey, fn_libusb_error_name(ret));
            if (ret == LIBUSB_TRANSFER_TIMED_OUT || ret == LIBUSB_ERROR_PIPE ||
                ret == LIBUSB_TRANSFER_CANCELLED || ret == LIBUSB_ERROR_TIMEOUT) {
                QCD_Printf(Info, "%s: key=(%s) retrying write (attempt %d/%d)\n", __func__, qcom_dev->qcomDevKey, attempt + 1, max_retries);
		if (ret == LIBUSB_ERROR_PIPE)
			fn_libusb_clear_halt(qcomDevInfo->devCtx->dev_handle, qcomDevInfo->bulk_out_endpointAddr);
#ifdef QCOM_WIN_ENV
                Sleep(500);
#else
                usleep(500000); /* 500ms delay between retries */
#endif
            } else {
                goto cleanup;
            }
        } else {
            break;
        }
    } while (++attempt < max_retries);

    if (ret == LIBUSB_SUCCESS && (bufferLen % qcomDevInfo->bulk_out_size) == 0) {
        int zlpSent = 0;
        int ret_zlp = fn_libusb_bulk_transfer(qcomDevInfo->devCtx->dev_handle, qcomDevInfo->bulk_out_endpointAddr, nullptr, 0, &zlpSent, timeOut);
        if (ret_zlp != LIBUSB_SUCCESS)
        {
            QCD_Printf(Error, "%s: failed to send ZLP bulk transfer: %s\n", __func__, fn_libusb_error_name(ret_zlp));
            ret = ret_zlp;
            goto cleanup;
        }
        QCD_Printf(Debug, "%s: ZLP sent successfully. bytes: %d/%d, ret: %s\n", __func__, zlpSent, 0, fn_libusb_error_name(ret_zlp));
    }

cleanup:
    lock.lock();
    --qcom_dev->active_transfer;
    lock.unlock();
    qcom_dev->active_transfer_cv.notify_all();
    return ret;
}

BOOL initialize_libusb()
{
    // Initialize libusb
    QCD_Printf(Info, "%s\n", __func__);
    if (fn_libusb_init(&g_ctx) < 0) {
        QCD_Printf(Error, "Failed to initialize libusb\n");
        unload_libusb();
        return EXIT_FAILURE;
    }

    //libusb_set_option(g_ctx, LIBUSB_OPTION_LOG_LEVEL, LIBUSB_LOG_LEVEL_DEBUG); /* Enable libusb library logs */

    return LIBUSB_SUCCESS;
}

VOID deinitialize_libusb()
{
#ifdef QCOM_LNX_ENV
    system("rm -rf /dev/QCOM_LIBUSB*");
#endif
    for(auto iter = qcomdevmap.begin(); iter != qcomdevmap.end();) {
        QcomLibUSBdevCtx *qcom_dev = iter->second;
        if (qcom_dev) {
            remove_device(qcom_dev, qcom_dev->dev, nullptr);
            if (qcom_dev->dev != nullptr) {
                fn_libusb_unref_device(qcom_dev->dev);
                qcom_dev->dev = nullptr;
            }
            iter = qcomdevmap.erase(iter);
            /* Use delete for non-trivially-destructible members (mutex, condition_variable, atomic<int>) */
            delete qcom_dev;
            qcom_dev = nullptr;
        } else {
            iter = qcomdevmap.erase(iter);
        }
    }
    fn_libusb_exit(g_ctx);
    g_ctx = nullptr;
    unload_libusb();
    QCD_Printf(Info, "%s\n", __func__);
}
