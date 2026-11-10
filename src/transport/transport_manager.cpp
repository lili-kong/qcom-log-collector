// Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
// SPDX-License-Identifier: BSD 3-Clause Clear License
//
// TransportManager implementation — device detection and monitor lifecycle.

#include "transport_manager.h"
#include "qdpublic.h"
#include "qcom-libusb.h"
#include "KL/kLogger.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <list>
#include <thread>
#include <tuple>
#include <unordered_map>
#include <utility>
#include <vector>

// One physical device, as tracked by the device monitor callback.
struct DeviceRecord {
    // Device-level view. `info.protocols` holds the protocols currently
    // present. Adopted from the seed the monitor callback built on the
    // device's first appearance, and cached from then on.
    QC::DeviceInfo info;

    // Protocols that have departed from this device, keyed by description.
    // Same rule as devices: a protocol seen once is never forgotten, it just
    // moves between here and `info.protocols`.
    std::unordered_map<std::string, QC::ProtocolInfo> disconnected_protocols;
};

// Device registry. Every device seen since the process started lives in
// exactly one of these two maps, keyed by device_group_key():
//   g_available_devices    — at least one of its protocols is present
//   g_disconnected_devices — its last protocol has departed (cache)
// A reconnecting device is fetched back out of the disconnected map rather
// than rebuilt, so its handles, resolved protocol types and any device fields
// QDS reported only once survive the round trip.
static std::unordered_map<std::string, DeviceRecord> g_available_devices;
static std::unordered_map<std::string, DeviceRecord> g_disconnected_devices;
// Group keys in first-presence order, so enumeration is stable across
// disconnect/reconnect cycles. Never pruned — mirrors the registry invariant.
static std::vector<std::string> g_device_order;
static std::mutex g_devices_mutex;

// Synthetic handle bookkeeping. device_handle uses only the high 32 bits
// (low 32 bits zero); protocol_handle shares its owning device's high bits
// and uses the low 32 bits to distinguish sibling protocols. Handles are
// stable for the lifetime of the process once minted for a given device
// group / protocol description (so departure events reuse the same handle
// that was assigned on arrival).
struct DeviceHandleEntry {
    uint64_t device_handle = 0;
    uint32_t next_protocol_low = 1;
    std::unordered_map<std::string, uint64_t> protocol_handles; // keyed by protocol description
};
static std::unordered_map<std::string, DeviceHandleEntry> g_device_handles;
static uint32_t g_next_device_high = 1;
static std::mutex g_device_handles_mutex;

// Grouping key shared with enumerate_composite_devices(): protocols with the
// same parent_device + parent_location belong to the same physical device.
static std::string device_group_key(const std::string& parent_device,
                                     const std::string& parent_location,
                                     const std::string& description) {
    return parent_device.empty() ? description : parent_device + "|" + parent_location;
}

// Assign (or reuse) info.device_handle and info.protocol_handle in place.
static void assign_handles(const std::string& group_key, QC::ProtocolInfo& info) {
    std::lock_guard<std::mutex> lock(g_device_handles_mutex);
    auto& entry = g_device_handles[group_key];
    if (entry.device_handle == 0) {
        entry.device_handle = static_cast<uint64_t>(g_next_device_high++) << 32;
    }
    info.device_handle = entry.device_handle;

    auto it = entry.protocol_handles.find(info.description);
    if (it == entry.protocol_handles.end()) {
        uint64_t handle = entry.device_handle | entry.next_protocol_low++;
        entry.protocol_handles.emplace(info.description, handle);
        info.protocol_handle = handle;
    } else {
        info.protocol_handle = it->second;
    }
}

// Fill in any device-level field still empty from a later protocol. QDS does
// not report every field on every interface, so a sibling arriving later may
// carry a serial number or SoC version the first one lacked.
static void merge_device_fields(QC::DeviceInfo& dev, const QC::DeviceInfo& seed) {
    if (dev.adb_serial_number.empty() && !seed.adb_serial_number.empty()) {
        dev.adb_serial_number = seed.adb_serial_number;
    }
    if (dev.serial_number.empty() && !seed.serial_number.empty()) {
        dev.serial_number = seed.serial_number;
    }
    if (dev.vid.empty() && !seed.vid.empty()) {
        dev.vid = seed.vid;
    }
    if (dev.pid.empty() && !seed.pid.empty()) {
        dev.pid = seed.pid;
    }
    if (dev.edl_chip_id.empty() && !seed.edl_chip_id.empty()) {
        dev.edl_chip_id = seed.edl_chip_id;
    }
    if (dev.soc_info.version.empty() && !seed.soc_info.version.empty()) {
        dev.soc_info = seed.soc_info;
    }
}

// Fetch the record for `group_key`, moving it out of the disconnected cache if
// that is where it currently lives, or creating it on genuine first presence.
// `seed` is the DeviceInfo built at the callback boundary; it is adopted only
// when the record is created, and ignored otherwise.
// Caller must hold g_devices_mutex.
static DeviceRecord& acquire_device_locked(const std::string& group_key,
                                           const QC::DeviceInfo& seed) {
    auto avail = g_available_devices.find(group_key);
    if (avail != g_available_devices.end()) {
        return avail->second;
    }

    auto cached = g_disconnected_devices.find(group_key);
    if (cached != g_disconnected_devices.end()) {
        // Reconnect: move the cached record back into the available map,
        // preserving its handles and previously-learned fields. The seed is
        // discarded — the cached record is authoritative.
        auto inserted = g_available_devices.emplace(group_key,
                                                   std::move(cached->second));
        g_disconnected_devices.erase(cached);
        return inserted.first->second;
    }

    // First presence of this device: adopt the seed as its DeviceInfo.
    DeviceRecord record;
    record.info = seed;
    auto inserted = g_available_devices.emplace(group_key, std::move(record));
    g_device_order.push_back(group_key);
    return inserted.first->second;
}

// Add `info` to its device's present-protocol list, reusing the cached
// ProtocolInfo if this protocol has been seen before. Returns a reference to
// the stored protocol, whose fields the caller may still adjust (protocol_type
// resolution). Sets `*device_connected` when this arrival is what made the
// device available — its first protocol, whether on first presence or on a
// reconnect. Caller must hold g_devices_mutex.
static QC::ProtocolInfo& add_protocol_locked(const std::string& group_key,
                                             const QC::ProtocolInfo& info,
                                             const QC::DeviceInfo& seed,
                                             bool* device_connected) {
    *device_connected = g_available_devices.find(group_key) == g_available_devices.end();
    DeviceRecord& record = acquire_device_locked(group_key, seed);

    // Already present (duplicate arrival) — refresh in place, do not add twice.
    for (auto& present : record.info.protocols) {
        if (present.description == info.description) {
            present = info;
            merge_device_fields(record.info, seed);
            return present;
        }
    }

    auto cached = record.disconnected_protocols.find(info.description);
    if (cached != record.disconnected_protocols.end()) {
        // Reconnect: recover the cached protocol, then refresh the volatile
        // fields from this arrival while keeping a previously resolved
        // protocol_type that QDS is reporting as UNKNOWN this time around.
        QC::ProtocolInfo restored = cached->second;
        record.disconnected_protocols.erase(cached);
        QC::ProtocolType known = restored.protocol_type;
        restored = info;
        if (restored.protocol_type == QC::PROT_UNKNOWN && known != QC::PROT_UNKNOWN) {
            restored.protocol_type = known;
        }
        record.info.protocols.push_back(std::move(restored));
    } else {
        record.info.protocols.push_back(info);
    }

    QC::ProtocolInfo& stored = record.info.protocols.back();
    merge_device_fields(record.info, seed);
    return stored;
}

// Move `description`'s protocol out of its device's present list and into the
// device's disconnected cache. If that empties the device, the whole record
// moves to g_disconnected_devices and `*device_disconnected` is set. Returns
// the device snapshot to report, which always describes the device even after
// its last protocol departs — falling back to `seed` for a device we have no
// record of. Caller must hold g_devices_mutex.
static QC::DeviceInfo remove_protocol_locked(const std::string& group_key,
                                             const QC::ProtocolInfo& info,
                                             const QC::DeviceInfo& seed,
                                             bool* device_disconnected) {
    *device_disconnected = false;
    auto avail = g_available_devices.find(group_key);
    if (avail == g_available_devices.end()) {
        // Departure for a device we never saw arrive, or already fully gone.
        auto cached = g_disconnected_devices.find(group_key);
        if (cached != g_disconnected_devices.end()) {
            cached->second.disconnected_protocols[info.description] = info;
            return cached->second.info;
        }
        return seed;
    }

    DeviceRecord& record = avail->second;
    for (auto it = record.info.protocols.begin();
         it != record.info.protocols.end(); ++it) {
        if (it->description == info.description) {
            record.disconnected_protocols[info.description] = *it;
            record.info.protocols.erase(it);
            break;
        }
    }

    if (record.info.protocols.empty()) {
        // Last protocol gone — the device itself is now disconnected. Cache
        // it so a reconnect restores the same record.
        QC::DeviceInfo snapshot = record.info;
        g_disconnected_devices.emplace(group_key, std::move(record));
        g_available_devices.erase(avail);
        *device_disconnected = true;
        return snapshot;
    }
    return record.info;
}

// Registered listeners, one list per edge. Protected by their own mutex so a
// listener callback does not need to hold g_devices_mutex. All lists draw from
// a single token counter, so a token identifies its entry uniquely across them.
struct ListenerEntry {
    QC::TransportManager::ListenerToken token;
    QC::ProtocolConnectedCallback callback;
};
struct DeviceListenerEntry {
    QC::TransportManager::ListenerToken token;
    QC::DeviceConnectedCallback callback;
};
struct StateListenerEntry {
    QC::TransportManager::ListenerToken token;
    QC::ProtocolStateCallback callback;
};
static std::vector<ListenerEntry> g_added_listeners;
static std::vector<ListenerEntry> g_removed_listeners;
static std::vector<DeviceListenerEntry> g_device_connected_listeners;
static std::vector<DeviceListenerEntry> g_device_disconnected_listeners;
static std::vector<StateListenerEntry> g_protocol_state_listeners;
static std::mutex g_listeners_mutex;
static QC::TransportManager::ListenerToken g_next_listener_token = 1;

// Copy a listener list so we can dispatch outside the lock.
template <typename Entry>
static std::vector<Entry> snapshot_listeners(const std::vector<Entry>& listeners) {
    std::lock_guard<std::mutex> lock(g_listeners_mutex);
    return listeners;
}

static void dispatch(const std::vector<ListenerEntry>& listeners,
                     const QC::DeviceInfo& device,
                     const QC::ProtocolInfo& protocol) {
    for (const auto& e : snapshot_listeners(listeners)) {
        if (!e.callback) continue;
        try {
            e.callback(device, protocol);
        } catch (...) {
            // Listeners must not throw across the QDS callback boundary.
        }
    }
}

static void dispatch(const std::vector<DeviceListenerEntry>& listeners,
                     const QC::DeviceInfo& device) {
    for (const auto& e : snapshot_listeners(listeners)) {
        if (!e.callback) continue;
        try {
            e.callback(device);
        } catch (...) {
            // Listeners must not throw across the QDS callback boundary.
        }
    }
}

static void dispatch(const std::vector<StateListenerEntry>& listeners,
                     int64_t handle, QC::ProtocolState state) {
    for (const auto& e : snapshot_listeners(listeners)) {
        if (!e.callback) continue;
        try {
            e.callback(handle, state);
        } catch (...) {
            // Listeners must not throw across this callback boundary.
        }
    }
}

static void dispatch_protocol_added(const QC::DeviceInfo& device,
                                    const QC::ProtocolInfo& protocol) {
    dispatch(g_added_listeners, device, protocol);
}

static void dispatch_protocol_removed(const QC::DeviceInfo& device,
                                      const QC::ProtocolInfo& protocol) {
    dispatch(g_removed_listeners, device, protocol);
}

static void dispatch_device_connected(const QC::DeviceInfo& device) {
    dispatch(g_device_connected_listeners, device);
}

static void dispatch_device_disconnected(const QC::DeviceInfo& device) {
    dispatch(g_device_disconnected_listeners, device);
}

#ifdef TOOLS_TARGET_WINDOWS
static std::string from_wide(const wchar_t* ws) {
    if (!ws || !ws[0]) return {};
    int len = WideCharToMultiByte(CP_UTF8, 0, ws, -1, nullptr, 0, nullptr, nullptr);
    if (len <= 0) return {};
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
    if (mapped != QC::PROT_UNKNOWN)
        return mapped;

    // MHI device with QCDM in description → always DIAG (unambiguous)
    if ((desc.find("QCDM") != std::string::npos || desc.find("qcdm") != std::string::npos)
        && (parent_desc.find("mhi") != std::string::npos || parent_desc.find("MHI") != std::string::npos))
        return QC::PROT_DIAG;

    // Description contains "DPL" → ADPL (unambiguous)
    if (desc.find("DPL") != std::string::npos)
        return QC::PROT_ADPL;

    // Description contains "QDSS" → QDSS (unambiguous)
    if (desc.find("QDSS") != std::string::npos)
        return QC::PROT_QDSS;

    // "Diagnostics"/"DIAG" is NOT resolved here — depends on sibling count.
    return QC::PROT_UNKNOWN;
}

// Split a HwId string (e.g. "USB\VID_05C6&PID_90DB&REV_0504&CID_042A") into
// its "&"-separated KEY_VALUE segments, keyed by KEY.
static std::unordered_map<std::string, std::string> parse_hw_id_segments(const std::string& hw_id) {
    std::unordered_map<std::string, std::string> segments;
    auto backslash = hw_id.find('\\');
    if (backslash == std::string::npos)
        return segments;

    std::string rest = hw_id.substr(backslash + 1);
    size_t start = 0;
    while (start <= rest.size()) {
        auto amp = rest.find('&', start);
        std::string entry = rest.substr(start, amp == std::string::npos ? amp : amp - start);
        auto underscore = entry.find('_');
        if (underscore != std::string::npos) {
            segments[entry.substr(0, underscore)] = entry.substr(underscore + 1);
        }
        if (amp == std::string::npos)
            break;
        start = amp + 1;
    }
    return segments;
}

// Extract USB (or PCI) vendor/product IDs from a device HwId string.
static std::pair<std::string, std::string> extract_vid_pid(const std::string& hw_id) {
    auto segments = parse_hw_id_segments(hw_id);
    bool is_pci = hw_id.find("PCI\\") != std::string::npos;
    const std::string& vid_key = is_pci ? "VEN" : "VID";
    const std::string& pid_key = is_pci ? "DEV" : "PID";

    std::string vid, pid;
    auto vid_it = segments.find(vid_key);
    if (vid_it != segments.end())
        vid = vid_it->second;
    auto pid_it = segments.find(pid_key);
    if (pid_it != segments.end())
        pid = pid_it->second;
    return {vid, pid};
}

// Extract the EDL chip ID (CID_ segment) from a device HwId string.
static std::string extract_edl_chip_id(const std::string& hw_id) {
    auto segments = parse_hw_id_segments(hw_id);
    auto it = segments.find("CID");
    return it != segments.end() ? it->second : std::string();
}

// Build device metadata directly from QDS; ProtocolInfo contains only the
// public interface fields. Later arrivals can fill gaps in the cached device.
static QC::DeviceInfo build_device_info(const CB_PARAMS& params, const std::string& description,
                                        const std::string& parent_device) {
    QC::DeviceInfo dev{};
    dev.description = parent_device.empty() ? description : parent_device;
#ifdef TOOLS_TARGET_WINDOWS
    const std::string hw_id = from_wide(params.HwId);
    dev.location = from_wide(params.ParentLocationInfomation);
    dev.adb_serial_number = from_wide(params.SerNum);
    dev.serial_number = from_wide(params.SerNumMsm);
    dev.soc_info.version = from_wide(params.SocVer);
#else
    const std::string hw_id = params.HwId ? params.HwId : "";
    dev.location = params.ParentLocationInfomation ? params.ParentLocationInfomation : "";
    dev.adb_serial_number = params.SerNum ? params.SerNum : "";
    dev.serial_number = params.SerNumMsm ? params.SerNumMsm : "";
    dev.soc_info.version = params.SocVer ? params.SocVer : "";
#endif
    dev.device_physical_location = dev.location;
    dev.soc_info.chip_name = dev.soc_info.version;
    std::tie(dev.vid, dev.pid) = extract_vid_pid(hw_id);
    dev.edl_chip_id = extract_edl_chip_id(hw_id);
    return dev;
}

// Whether a description names an interface QDS may report ambiguously: both
// DIAG and SAHARA surface as "Diagnostics"/"DIAG".
static bool is_ambiguous_diag(const std::string& desc) {
    return desc.find("Diagnostics") != std::string::npos
           || desc.find("DIAG") != std::string::npos;
}

// Resolve the ambiguous "Diagnostics"/"DIAG" interface to DIAG once we know
// the device has more than one protocol — a lone such interface may be SAHARA,
// but a sibling proves it is a composite DIAG device.
// Caller must hold g_devices_mutex.
static void resolve_diag_ambiguity_locked(QC::DeviceInfo& dev) {
    if (dev.protocols.size() < 2)
        return;
    for (auto& proto : dev.protocols) {
        if (proto.protocol_type == QC::PROT_UNKNOWN && is_ambiguous_diag(proto.description)) {
            proto.protocol_type = QC::PROT_DIAG;
        }
    }
}

// Record a protocol arrival and dispatch to the listeners. `seed` is the
// device view built at the callback boundary; the registry adopts it only if
// this is the device's first presence. When this is the device's first
// protocol, the device-connected edge fires first, so a listener learns about
// the device before hearing about its protocols.
static void handle_protocol_arrival(const std::string& group_key, const QC::ProtocolInfo& info,
                                    const QC::DeviceInfo& seed, bool has_parent) {
    QC::DeviceInfo device;
    QC::ProtocolInfo reported;
    bool device_connected = false;
    {
        std::lock_guard<std::mutex> lock(g_devices_mutex);
        QC::ProtocolInfo& stored = add_protocol_locked(group_key, info, seed,
                                                      &device_connected);
        // Standalone (non-composite) interfaces have no parent to group under,
        // so sibling count tells us nothing about them.
        if (has_parent) {
            resolve_diag_ambiguity_locked(g_available_devices[group_key].info);
        }
        device = g_available_devices[group_key].info;
        // Re-read from the snapshot: resolve_diag_ambiguity_locked may have
        // upgraded this protocol's type after it was stored.
        reported = stored;
    }
    if (device_connected) {
        dispatch_device_connected(device);
    }
    dispatch_protocol_added(device, reported);
}

// Record a protocol departure and dispatch to the listeners. When this was the
// device's last protocol, the device-disconnected edge fires after the
// protocol edge, so a listener hears about the protocols before losing the
// device.
static void handle_protocol_departure(const std::string& group_key,
                                      const QC::ProtocolInfo& info,
                                      const QC::DeviceInfo& seed) {
    QC::DeviceInfo device;
    bool device_disconnected = false;
    {
        std::lock_guard<std::mutex> lock(g_devices_mutex);
        device = remove_protocol_locked(group_key, info, seed,
                                        &device_disconnected);
    }
    dispatch_protocol_removed(device, info);
    if (device_disconnected) {
        dispatch_device_disconnected(device);
    }
}

#ifdef TOOLS_TARGET_WINDOWS
static VOID __stdcall device_change_callback(PCB_PARAMS pCbParams, PVOID* /*context*/)
#else
static VOID device_change_callback(PCB_PARAMS pCbParams, PVOID* /*context*/)
#endif
{
    if (!pCbParams)
        return;

    if (!pCbParams->DevDesc)
        return;

    UCHAR devState = (UCHAR)((pCbParams->Flag & 0x00F0) >> 4);

    QC::ProtocolInfo info{};
#ifdef TOOLS_TARGET_WINDOWS
    info.description = from_wide(pCbParams->DevDesc);
    const std::string parent_device = from_wide(pCbParams->ParentDev);
#else
    info.description = pCbParams->DevDesc;
    const std::string parent_device = pCbParams->ParentDev ? pCbParams->ParentDev : "";
#endif
    info.protocol_type = resolve_protocol(pCbParams->Protocol, info.description, parent_device);

    auto seed = build_device_info(*pCbParams, info.description, parent_device);
    const std::string group_key = device_group_key(parent_device, seed.location, info.description);
    assign_handles(group_key, info);
    seed.device_handle = info.device_handle;

    if (devState == 1) {        // QC_DEV_STATE_ARRIVAL
        handle_protocol_arrival(group_key, info, seed, !parent_device.empty());
    } else if (devState == 2) { // QC_DEV_STATE_DEPARTURE
        handle_protocol_departure(group_key, info, seed);
    }
}

namespace QC {

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
    static DEV_FEATURE_SETTING feature = {};
    feature.Version = 1;
    feature.Settings = DEV_FEATURE_INCLUDE_NONE_QC_PORTS;
    feature.DeviceClass = DEV_CLASS_NET | DEV_CLASS_PORTS |
                          DEV_CLASS_USB | DEV_CLASS_MDM
#ifdef TOOLS_TARGET_WINDOWS
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
        return;
    }

    configure_features();
    QcDevice::SetDeviceChangeCallback(device_change_callback, nullptr);
    QcDevice::StartDeviceMonitor();
    monitor_running_ = true;

    if (wait_ms > 0) {
        std::this_thread::sleep_for(std::chrono::milliseconds(wait_ms));
    }
}

void TransportManager::stop_monitor() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!monitor_running_) {
        return;
    }

    try {
        QcDevice::StopDeviceMonitor();
    } catch (...) {
        CFLOG_WARNING("TransportManager: StopDeviceMonitor threw (non-fatal)", true);
    }

    {
        // Nothing is reachable with the monitor stopped, but a device seen
        // once must stay in the registry — move the available ones to the
        // disconnected cache rather than dropping them, so a later
        // start_monitor() restores their handles and learned fields.
        // No disconnect callbacks are dispatched here: this teardown is
        // caller-initiated, and firing listeners while holding mutex_ would
        // let a listener re-enter TransportManager and deadlock.
        std::lock_guard<std::mutex> lock2(g_devices_mutex);
        for (auto& entry : g_available_devices) {
            DeviceRecord& record = entry.second;
            for (auto& proto : record.info.protocols) {
                record.disconnected_protocols[proto.description] = proto;
            }
            record.info.protocols.clear();
            g_disconnected_devices[entry.first] = std::move(record);
        }
        g_available_devices.clear();
    }

    monitor_running_ = false;
}

bool TransportManager::is_monitor_running() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return monitor_running_;
}

std::vector<ProtocolInfo> TransportManager::enumerate_devices() {
    std::vector<ProtocolInfo> result;

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
            ProtocolInfo info{};
            info.description = ptr;
            const std::string com_port = extract_com_port(info.description);

            std::string log_msg = "  [" + std::to_string(idx++) + "] \"" + info.description + "\"";
            if (!com_port.empty()) {
                log_msg += " (" + com_port + ")";
            }
            CFLOG_DEBUG(log_msg, true);

            result.push_back(std::move(info));
            ptr += std::strlen(ptr) + 1;
        }
    }

    return result;
}

std::vector<DeviceInfo> TransportManager::enumerate_composite_devices(
    const std::vector<ProtocolType>& filter) {
    // Devices are already grouped and their DeviceInfo already built (on first
    // presence), so this just snapshots the available ones in discovery order.
    // Disconnected devices are intentionally excluded — this reports what is
    // reachable now, not everything ever seen.
    std::vector<DeviceInfo> snapshot;
    {
        std::lock_guard<std::mutex> lock(g_devices_mutex);
        for (const auto& key : g_device_order) {
            auto it = g_available_devices.find(key);
            if (it != g_available_devices.end()) {
                snapshot.push_back(it->second.info);
            }
        }
    }

    std::vector<DeviceInfo> result;
    for (auto& dev : snapshot) {
        if (!filter.empty()) {
            std::list<ProtocolInfo> filtered;
            for (auto& proto : dev.protocols) {
                if (std::find(filter.begin(), filter.end(), proto.protocol_type) != filter.end()) {
                    filtered.push_back(std::move(proto));
                }
            }
            dev.protocols = std::move(filtered);
        }
        if (!dev.protocols.empty()) {
            result.push_back(std::move(dev));
        }
    }
    return result;
}

std::string TransportManager::resolve_device_name(const std::string& short_name) {
    auto devices = enumerate_devices();

    for (const auto& dev : devices) {
        if (dev.description == short_name) {
            return dev.description;
        }
        if (dev.description.find(short_name) != std::string::npos) {
            CFLOG_INFO("TransportManager: Resolved \"" + short_name +
                       "\" to \"" + dev.description + "\"", true);
            return dev.description;
        }
    }

    CFLOG_WARNING("TransportManager: Warning: \"" + short_name +
                  "\" not found in discovered devices", true);
    return short_name;
}

std::string TransportManager::extract_com_port(const std::string& desc) {
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
        0,
        NULL,
        OPEN_EXISTING,
        0,
        NULL);

    if (h != INVALID_HANDLE_VALUE) {
        CloseHandle(h);
        return false;
    }

    return (GetLastError() == ERROR_SHARING_VIOLATION);
#else
    (void)com_port;
    return false;
#endif
}

TransportManager::ListenerToken TransportManager::set_protocol_added_listener(
    ProtocolConnectedCallback callback) {
    if (!callback) return 0;
    std::lock_guard<std::mutex> lock(g_listeners_mutex);
    ListenerToken token = g_next_listener_token++;
    g_added_listeners.push_back({token, callback});
    return token;
}

TransportManager::ListenerToken TransportManager::set_protocol_removed_listener(
    ProtocolConnectedCallback callback) {
    if (!callback) return 0;
    std::lock_guard<std::mutex> lock(g_listeners_mutex);
    ListenerToken token = g_next_listener_token++;
    g_removed_listeners.push_back({token, callback});
    return token;
}

TransportManager::ListenerToken TransportManager::set_device_connected_listener(
    DeviceConnectedCallback callback) {
    if (!callback) return 0;
    std::lock_guard<std::mutex> lock(g_listeners_mutex);
    ListenerToken token = g_next_listener_token++;
    g_device_connected_listeners.push_back({token, callback});
    return token;
}

TransportManager::ListenerToken TransportManager::set_device_disconnected_listener(
    DeviceConnectedCallback callback) {
    if (!callback) return 0;
    std::lock_guard<std::mutex> lock(g_listeners_mutex);
    ListenerToken token = g_next_listener_token++;
    g_device_disconnected_listeners.push_back({token, callback});
    return token;
}

TransportManager::ListenerToken TransportManager::set_protocol_state_listener(
    ProtocolStateCallback callback) {
    if (!callback) return 0;
    std::lock_guard<std::mutex> lock(g_listeners_mutex);
    ListenerToken token = g_next_listener_token++;
    g_protocol_state_listeners.push_back({token, callback});
    return token;
}

void TransportManager::unsubscribe(ListenerToken token) {
    if (token == 0) return;
    auto drop = [token](auto& listeners) {
        auto it = std::remove_if(listeners.begin(), listeners.end(),
            [token](const auto& e) { return e.token == token; });
        listeners.erase(it, listeners.end());
    };
    std::lock_guard<std::mutex> lock(g_listeners_mutex);
    drop(g_added_listeners);
    drop(g_removed_listeners);
    drop(g_device_connected_listeners);
    drop(g_device_disconnected_listeners);
    drop(g_protocol_state_listeners);
}

void TransportManager::notify_protocol_state_changed(int64_t handle, ProtocolState state) {
    dispatch(g_protocol_state_listeners, handle, state);
}

} // namespace QC
