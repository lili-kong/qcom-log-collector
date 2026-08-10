// SPDX-License-Identifier: Apache-2.0
// Copyright 2024 Qualcomm Innovation Center, Inc.
//
// TransportManager implementation — device detection and monitor lifecycle.

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

#include "transport_manager.h"
#include "qdpublic.h"
#include "qcom-libusb.h"
#include "KL/kLogger.h"

// All protocols discovered by the device monitor callback (both platforms).
static std::vector<QC::DeviceInfo> g_monitor_devices;
static std::mutex g_monitor_devices_mutex;

#ifdef TOOLS_TARGET_WINDOWS
static std::string from_wide(const wchar_t* ws) {
    if (!ws || !ws[0]) {
        return {};
    }
    int len = WideCharToMultiByte(CP_UTF8, 0, ws, -1, nullptr, 0, nullptr, nullptr);
    if (len <= 0) {
        return {};
    }
    std::string out(static_cast<size_t>(len - 1), '\0');
    WideCharToMultiByte(CP_UTF8, 0, ws, -1, &out[0], len, nullptr, nullptr);
    return out;
}
#endif

// Map QDS DEV_PROTOCOL_* wire code to application-level ProtocolType.
static QC::ProtocolType map_wire_protocol(uint32_t wire_code) {
    switch (wire_code & QC_FLAG_MASK_DEV_PROTOCOL) {
        case DEV_PROTOCOL_DIAG:     return QC::PROT_DIAG;
        case DEV_PROTOCOL_SAHARA:   return QC::PROT_SAHARA;
        case DEV_PROTOCOL_FIREHOSE: return QC::PROT_FIREHOSE;
        case DEV_PROTOCOL_ADPL:     return QC::PROT_ADPL;
        case DEV_PROTOCOL_QDSS:     return QC::PROT_QDSS;
        case DEV_PROTOCOL_DUN:      return QC::PROT_DUN;
        case DEV_PROTOCOL_RMNET:    return QC::PROT_RMNET;
        case DEV_PROTOCOL_NMEA:     return QC::PROT_NMEA;
        default:                    return QC::PROT_UNKNOWN;
    }
}

// Resolve protocol from description when QDS reports UNKNOWN or RESERVED.
// Only resolves unambiguous cases (ADPL, QDSS, MHI DIAG).
// The ambiguous "Diagnostics"/"DIAG" case (could be DIAG or SAHARA) is deferred
// to enumerate_composite_devices() where sibling count is known.
static QC::ProtocolType resolve_protocol(uint32_t wire_code,
                                                 const std::string& desc,
                                                 const std::string& parent_desc) {
    QC::ProtocolType mapped = map_wire_protocol(wire_code);
    if (mapped != QC::PROT_UNKNOWN) {
        return mapped;
    }

    // MHI device with QCDM in description → always DIAG (unambiguous)
    if ((desc.find("QCDM") != std::string::npos || desc.find("qcdm") != std::string::npos)
        && (parent_desc.find("mhi") != std::string::npos || parent_desc.find("MHI") != std::string::npos)) {
        return QC::PROT_DIAG;
    }

    // Description contains "DPL" → ADPL (unambiguous)
    if (desc.find("DPL") != std::string::npos) {
        return QC::PROT_ADPL;
    }

    // Description contains "QDSS" → QDSS (unambiguous)
    if (desc.find("QDSS") != std::string::npos) {
        return QC::PROT_QDSS;
    }

    // "Diagnostics"/"DIAG" is NOT resolved here — depends on sibling count.
    return QC::PROT_UNKNOWN;
}

#ifdef TOOLS_TARGET_WINDOWS
static VOID __stdcall device_change_callback(PCB_PARAMS pCbParams, PVOID* /*context*/) {
#else
static VOID device_change_callback(PCB_PARAMS pCbParams, PVOID* /*context*/) {
#endif
    if (!pCbParams) {
        return;
    }

#ifdef TOOLS_TARGET_WINDOWS
    if (!pCbParams->DevDesc) {
        return;
    }

    UCHAR devState = static_cast<UCHAR>((pCbParams->Flag & 0x00F0) >> 4);

    QC::DeviceInfo info;
    info.device_name = from_wide(pCbParams->DevDesc);
    info.com_port = QC::TransportManager::extract_com_port(info.device_name);
    info.parent_device = from_wide(pCbParams->ParentDev);
    info.parent_location = from_wide(pCbParams->ParentLocationInfomation);
    info.adb_serial_number = from_wide(pCbParams->SerNum);
    info.msm_serial_number = from_wide(pCbParams->SerNumMsm);
    info.protocol = resolve_protocol(pCbParams->Protocol, info.device_name, info.parent_device);
    info.flags = pCbParams->Flag;

    if (devState == 1) { // QC_DEV_STATE_ARRIVAL
        std::lock_guard<std::mutex> lock(g_monitor_devices_mutex);
        if (!info.parent_device.empty()) {
            bool has_siblings = false;
            for (auto& existing : g_monitor_devices) {
                if (existing.parent_device != info.parent_device
                    || existing.parent_location != info.parent_location) {
                    continue;
                }
                has_siblings = true;
                if (existing.protocol == QC::PROT_UNKNOWN
                    && (existing.device_name.find("Diagnostics") != std::string::npos
                        || existing.device_name.find("DIAG") != std::string::npos)) {
                    existing.protocol = QC::PROT_DIAG;
                }
            }
            if (has_siblings && info.protocol == QC::PROT_UNKNOWN
                && (info.device_name.find("Diagnostics") != std::string::npos
                    || info.device_name.find("DIAG") != std::string::npos)) {
                info.protocol = QC::PROT_DIAG;
            }
        }
        g_monitor_devices.push_back(std::move(info));
    } else if (devState == 2) { // QC_DEV_STATE_DEPARTURE
        std::lock_guard<std::mutex> lock(g_monitor_devices_mutex);
        auto it = std::remove_if(g_monitor_devices.begin(), g_monitor_devices.end(),
            [&](const QC::DeviceInfo& d) {
                return d.device_name == info.device_name;
            });
        g_monitor_devices.erase(it, g_monitor_devices.end());
    }
#else
    if (!pCbParams->DevDesc) {
        return;
    }

    UCHAR devState = static_cast<UCHAR>((pCbParams->Flag & 0x00F0) >> 4);

    QC::DeviceInfo info;
    info.device_name = pCbParams->DevDesc;
    info.com_port = QC::TransportManager::extract_com_port(info.device_name);
    info.parent_device = pCbParams->ParentDev ? pCbParams->ParentDev : "";
    info.parent_location = pCbParams->ParentLocationInfomation ? pCbParams->ParentLocationInfomation : "";
    info.adb_serial_number = pCbParams->SerNum ? pCbParams->SerNum : "";
    info.msm_serial_number = pCbParams->SerNumMsm ? pCbParams->SerNumMsm : "";
    info.protocol = resolve_protocol(pCbParams->Protocol, info.device_name, info.parent_device);
    info.flags = pCbParams->Flag;

    if (devState == 1) { // QC_DEV_STATE_ARRIVAL
        std::lock_guard<std::mutex> lock(g_monitor_devices_mutex);
        if (!info.parent_device.empty()) {
            bool has_siblings = false;
            for (auto& existing : g_monitor_devices) {
                if (existing.parent_device != info.parent_device
                    || existing.parent_location != info.parent_location) {
                    continue;
                }
                has_siblings = true;
                if (existing.protocol == QC::PROT_UNKNOWN
                    && (existing.device_name.find("Diagnostics") != std::string::npos
                        || existing.device_name.find("DIAG") != std::string::npos)) {
                    existing.protocol = QC::PROT_DIAG;
                }
            }
            if (has_siblings && info.protocol == QC::PROT_UNKNOWN
                && (info.device_name.find("Diagnostics") != std::string::npos
                    || info.device_name.find("DIAG") != std::string::npos)) {
                info.protocol = QC::PROT_DIAG;
            }
        }
        g_monitor_devices.push_back(info);
    } else if (devState == 2) { // QC_DEV_STATE_DEPARTURE
        std::lock_guard<std::mutex> lock(g_monitor_devices_mutex);
        auto it = std::remove_if(g_monitor_devices.begin(), g_monitor_devices.end(),
            [&](const QC::DeviceInfo& d) {
                return d.device_name == info.device_name;
            });
        g_monitor_devices.erase(it, g_monitor_devices.end());
    }

#endif
}

namespace QC {

TransportManager& TransportManager::instance() {
    static TransportManager mgr;
    return mgr;
}

#ifdef TOOLS_TARGET_WINDOWS
std::wstring TransportManager::to_wide(const std::string& s) {
    if (s.empty()) {
        return {};
    }
    int sz = MultiByteToWideChar(CP_ACP, 0, s.data(),
                                 static_cast<int>(s.size()), nullptr, 0);
    std::wstring out(static_cast<size_t>(sz), L'\0');
    MultiByteToWideChar(CP_ACP, 0, s.data(),
                        static_cast<int>(s.size()), &out[0], sz);
    return out;
}
#endif

// ----------------------------------------------------------------------------
// logQDS
//
/// Prints QC Device Discovery logs to QLC Debug log files
// ----------------------------------------------------------------------------
#ifdef TOOLS_TARGET_WINDOWS
static void __stdcall logQDS(int32_t level, PCHAR pMessage) {
#else
static void logQDS(int32_t level, PCHAR pMessage) {
#endif
    if (pMessage) {
        switch (KL::Level(level)) {
            case KL::Level::Data:
                FLOG_INFO(("QDS: " + std::string(pMessage)).c_str());
                break;

            case KL::Level::Debug:
                FLOG_DEBUG(("QDS: " + std::string(pMessage)).c_str());
                break;

            case KL::Level::Warn:
                FLOG_WARNING(("QDS: " + std::string(pMessage)).c_str());
                break;

            case KL::Level::Error:
                FLOG_ERROR(("QDS: " + std::string(pMessage)).c_str());
                break;

            case KL::Level::Info:
            default:
                FLOG_INFO(("QDS: " + std::string(pMessage)).c_str());
                break;
        }
    }
}

void TransportManager::configure_features() {
    // SetFeature MUST run on every platform: QDS's Linux processAddDevice
    // dereferences qcdevCtx.mSetting->DeviceClass without a NULL guard
    // (qcdev.cpp:771/831/885/939). If SetFeature is never called, mSetting is
    // NULL and the first ttyUSB/diag port segfaults the QDS monitor thread.
    // TUF (Manager.cpp:3368) calls it unconditionally.
    static DEV_FEATURE_SETTING feature = {};
    feature.Version = 1;
    feature.Settings = DEV_FEATURE_INCLUDE_NONE_QC_PORTS;
    feature.DeviceClass = DEV_CLASS_NET | DEV_CLASS_PORTS |
                          DEV_CLASS_USB | DEV_CLASS_MDM
#ifdef TOOLS_TARGET_WINDOWS
                          // Windows also routes a libusb device class; this
                          // macro is not defined in the Linux QDS headers.
                          | DEV_CLASS_LIBUSB
#endif
                          ;
    feature.VID = NULL;
    QcDevice::SetLoggingCallback(&logQDS);
    QcDevice::SetFeature(reinterpret_cast<PVOID>(&feature));
}

void TransportManager::start_monitor(uint32_t wait_ms) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (monitor_running_) {
        return;  // Already running
    }

    configure_features();

    // Register callback BEFORE StartDeviceMonitor so the QDS callback pointer
    // is non-NULL when scan results are processed.
    QcDevice::SetDeviceChangeCallback(device_change_callback, nullptr);

    // StartDeviceMonitor initializes internal critical sections (opLock, etc.)
    // and spawns the device scan thread.
    QcDevice::StartDeviceMonitor();
    monitor_running_ = true;

    // Give the monitor time to discover devices.  StartDeviceMonitor signals
    // hMonitorStartedEvt BEFORE the first scan, so we must wait for
    // ScanDevices() + TryToAnnounce() to finish populating ArrivalList.
    if (wait_ms > 0) {
        std::this_thread::sleep_for(std::chrono::milliseconds(wait_ms));
    }
}

void TransportManager::stop_monitor() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!monitor_running_) {
        return;
    }

    // StopDeviceMonitor resets _IsQcomLibusbEnable = FALSE, ensuring
    // subsequent I/O uses the correct Win32 overlapped path instead of
    // qcom_libusb_read/write/close.
    try {
        QcDevice::StopDeviceMonitor();
        //CFLOG_INFO("TransportManager: StopDeviceMonitor completed (libusb routing reset)");
    } catch (...) {
        CFLOG_WARNING("TransportManager: StopDeviceMonitor threw (non-fatal)", true);
    }

    {
        std::lock_guard<std::mutex> lock2(g_monitor_devices_mutex);
        g_monitor_devices.clear();
    }

    monitor_running_ = false;
}

bool TransportManager::is_monitor_running() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return monitor_running_;
}

std::vector<DeviceInfo> TransportManager::enumerate_devices() {
    std::vector<DeviceInfo> result;

    static constexpr ULONG BUF_SIZE = 64 * 1024;
    std::vector<char> buf(BUF_SIZE, '\0');
    ULONG actual_size = 0;
    ULONG count = QcDevice::GetDeviceList(buf.data(), BUF_SIZE, &actual_size);

    CFLOG_INFO("TransportManager: GetDeviceList returned " + std::to_string(count) +
               " device(s), " + std::to_string(actual_size) + " bytes", true);

    if (count > 0 && actual_size > 0) {
        const char* ptr = buf.data();
        const char* end = buf.data() + actual_size;
        int idx = 0;

        while (ptr < end && *ptr != '\0') {
            DeviceInfo info;
            info.device_name = ptr;
            info.com_port = extract_com_port(info.device_name);

            std::string log_msg = "  [" + std::to_string(idx++) + "] \"" + info.device_name + "\"";
            if (!info.com_port.empty()) {
                log_msg += " (" + info.com_port + ")";
            }
            CFLOG_DEBUG(log_msg, true);

            result.push_back(std::move(info));
            ptr += std::strlen(ptr) + 1;
        }
    }

    return result;
}

std::string TransportManager::resolve_device_name(const std::string& short_name) {
    auto devices = enumerate_devices();

    for (const auto& dev : devices) {
        // Exact match
        if (dev.device_name == short_name) {
            return dev.device_name;
        }
        // Partial match (e.g. "COM3" inside the full description)
        if (dev.device_name.find(short_name) != std::string::npos) {
            CFLOG_INFO("TransportManager: Resolved \"" + short_name +
                       "\" to \"" + dev.device_name + "\"", true);
            return dev.device_name;
        }
    }

    CFLOG_WARNING("TransportManager: Warning: \"" + short_name +
                  "\" not found in discovered devices", true);
    return short_name;
}

std::string TransportManager::extract_com_port(const std::string& desc) {
    // Look for "(COMxx)" at end of string
    auto paren = desc.rfind('(');
    if (paren != std::string::npos) {
        auto close = desc.find(')', paren);
        if (close != std::string::npos) {
            std::string inside = desc.substr(paren + 1, close - paren - 1);
            if (inside.size() >= 4 && inside.substr(0, 3) == "COM") {
                return inside;
            }
        }
    }
    // Also handle bare "COM3" input
    if (desc.size() >= 4 && desc.substr(0, 3) == "COM") {
        bool all_digits = true;
        for (size_t i = 3; i < desc.size(); ++i) {
            if (!isdigit(static_cast<unsigned char>(desc[i]))) {
                all_digits = false;
                break;
            }
        }
        if (all_digits) {
            return desc;
        }
    }
    return {};
}

bool TransportManager::is_com_port_in_use(const std::string& com_port) {
    if (com_port.empty()) {
        return false;
    }

#ifdef TOOLS_TARGET_WINDOWS
    std::string path = "\\\\.\\" + com_port;
    std::wstring wpath = to_wide(path);

    HANDLE h = CreateFileW(
        wpath.c_str(),
        GENERIC_READ | GENERIC_WRITE,
        0,  // no sharing - exclusive access test
        NULL,
        OPEN_EXISTING,
        0,
        NULL);

    if (h != INVALID_HANDLE_VALUE) {
        // Port is not in use by anyone - close and return false
        CloseHandle(h);
        return false;
    }

    return (GetLastError() == ERROR_SHARING_VIOLATION);
#else
    (void)com_port;
    return false;
#endif
}

std::vector<CompositeDevice> TransportManager::enumerate_composite_devices(
    const std::vector<ProtocolType>& filter) {
    std::vector<DeviceInfo> all_protocols;
    {
        std::lock_guard<std::mutex> lock(g_monitor_devices_mutex);
        all_protocols = g_monitor_devices;
    }

    // Group protocols by parent_device + parent_location.
    // Use concatenation as the grouping key (like QUTS uses parentDescription).
    std::unordered_map<std::string, CompositeDevice> groups;
    std::vector<std::string> group_order;

    // First pass: group ALL protocols by parent device
    for (auto& proto : all_protocols) {
        std::string key;
        if (!proto.parent_device.empty()) {
            key = proto.parent_device + "|" + proto.parent_location;
        } else {
            key = proto.device_name;
        }

        if (groups.find(key) == groups.end()) {
            CompositeDevice dev;
            dev.description = proto.parent_device.empty()
                ? proto.device_name : proto.parent_device;
            dev.location = proto.parent_location;
            dev.adb_serial_number = proto.adb_serial_number;
            dev.msm_serial_number = proto.msm_serial_number;
            groups[key] = std::move(dev);
            group_order.push_back(key);
        } else {
            auto& dev = groups[key];
            if (dev.adb_serial_number.empty() && !proto.adb_serial_number.empty()) {
                dev.adb_serial_number = proto.adb_serial_number;
            }
            if (dev.msm_serial_number.empty() && !proto.msm_serial_number.empty()) {
                dev.msm_serial_number = proto.msm_serial_number;
            }
        }
        groups[key].protocols.push_back(std::move(proto));
    }

    // Second pass: resolve ambiguous "Diagnostics"/"DIAG" protocol.
    // If protocol is UNKNOWN and description contains "Diagnostics" or "DIAG":
    //   - Device has other sibling protocols → DIAG (normal operation)
    //   - Device has NO other protocols → SAHARA (crash/ramdump mode, not usable)
    for (auto& [key, dev] : groups) {
        size_t total_protocols = dev.protocols.size();
        for (auto& proto : dev.protocols) {
            if (proto.protocol != PROT_UNKNOWN) {
                continue;
            }
            if (proto.device_name.find("Diagnostics") == std::string::npos
                && proto.device_name.find("DIAG") == std::string::npos) {
                continue;
            }

            if (total_protocols > 1) {
                proto.protocol = PROT_DIAG;
            }
        }
    }

    // Third pass: filter by requested protocol types and remove empty devices
    if (!filter.empty()) {
        for (auto& [key, dev] : groups) {
            std::vector<DeviceInfo> filtered;
            for (auto& proto : dev.protocols) {
                if (std::find(filter.begin(), filter.end(), proto.protocol) != filter.end()) {
                    filtered.push_back(std::move(proto));
                }
            }
            dev.protocols = std::move(filtered);
        }
    }

    std::vector<CompositeDevice> result;
    for (const auto& key : group_order) {
        if (!groups[key].protocols.empty()) {
            result.push_back(std::move(groups[key]));
        }
    }
    return result;
}

} // namespace QC
