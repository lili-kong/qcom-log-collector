// SPDX-License-Identifier: Apache-2.0
// Copyright 2024 Qualcomm Innovation Center, Inc.
//
// TransportManager implementation — device detection and monitor lifecycle.

#include "transport_manager.h"
#include "qdpublic.h"
#include "qcom-libusb.h"
#include "KL/kLogger.h"

#include <chrono>
#include <cstdio>
#include <cstring>
#include <thread>
#include <utility>
#include <vector>

// Device-change callback that stores sub-interface DevDesc strings pushed
// by PushLibUSBToApplication (libusb sub-interface enumeration).
// On Windows, MakeAnnouncement requires a non-NULL callback; on Linux,
// this captures per-interface device names from the libusb enumeration path.
static std::vector<diaglogger::DeviceInfo>* g_libusb_devices = nullptr;
static std::mutex g_libusb_devices_mutex;

#ifdef TOOLS_TARGET_WINDOWS
static VOID __stdcall device_change_callback(PCB_PARAMS pCbParams, PVOID* /*context*/)
#else
static VOID device_change_callback(PCB_PARAMS pCbParams, PVOID* /*context*/)
#endif
{
#ifdef TOOLS_TARGET_LINUX
    if (!pCbParams || !pCbParams->DevDesc)
        return;

    std::lock_guard<std::mutex> lock(g_libusb_devices_mutex);
    if (g_libusb_devices) {
        diaglogger::DeviceInfo info;
        info.device_name = pCbParams->DevDesc;
        g_libusb_devices->push_back(std::move(info));
    }
#else
    (void)pCbParams;
#endif
}

namespace diaglogger {

TransportManager& TransportManager::instance() {
    static TransportManager mgr;
    return mgr;
}

#ifdef TOOLS_TARGET_WINDOWS
std::wstring TransportManager::to_wide(const std::string& s) {
    if (s.empty()) return {};
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
   static void __stdcall logQDS(int32_t level, PCHAR pMessage)
#else
   static void logQDS(int32_t level, PCHAR pMessage)
#endif
   {
      if(pMessage)
      {
         switch(KL::Level(level))
         {
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
        if (all_digits) return desc;
    }
    return {};
}

bool TransportManager::is_com_port_in_use(const std::string& com_port) {
    if (com_port.empty()) return false;

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

std::vector<DeviceInfo> TransportManager::enumerate_interfaces() {
    std::vector<DeviceInfo> result;

#ifdef TOOLS_TARGET_LINUX
    if (IsQcomLibusbEnable() && is_libusb_loaded()) {
        std::vector<DeviceInfo> libusb_devs;
        {
            std::lock_guard<std::mutex> lock(g_libusb_devices_mutex);
            g_libusb_devices = &libusb_devs;
        }

        ::enumerate_devices();

        {
            std::lock_guard<std::mutex> lock(g_libusb_devices_mutex);
            g_libusb_devices = nullptr;
        }

        if (!libusb_devs.empty()) {
            // Collect bus-addr pairs to identify parent composite devices
            std::vector<std::pair<int,int>> bus_addrs;
            for (const auto& iface : libusb_devs) {
                auto last_us = iface.device_name.rfind('_');
                if (last_us == std::string::npos) continue;
                auto dash = iface.device_name.find('-', last_us);
                auto colon = iface.device_name.find(':', last_us);
                if (dash == std::string::npos || colon == std::string::npos) continue;
                int bus = std::stoi(iface.device_name.substr(last_us + 1, dash - last_us - 1));
                int addr = std::stoi(iface.device_name.substr(dash + 1, colon - dash - 1));
                bus_addrs.emplace_back(bus, addr);
            }

            result = std::move(libusb_devs);

            // Append single-interface devices from GetDeviceList, skipping parents
            auto all_devs = enumerate_devices();
            for (const auto& dev : all_devs) {
                bool is_parent = false;
                for (const auto& [bus, addr] : bus_addrs) {
                    char pattern[16];
                    snprintf(pattern, sizeof(pattern), "%03d/%03d", bus, addr);
                    if (dev.device_name.find(pattern) != std::string::npos) {
                        is_parent = true;
                        break;
                    }
                }
                if (!is_parent) {
                    result.push_back(dev);
                }
            }
            return result;
        }
    }
#endif

    // Fallback: return GetDeviceList results as-is
    result = enumerate_devices();

    return result;
}

} // namespace diaglogger
