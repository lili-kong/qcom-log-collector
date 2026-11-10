// Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
// SPDX-License-Identifier: BSD 3-Clause Clear License
#include <string>
#include "../../common/utils.h"
#include "libusb-dynamic-loader.h"


#ifdef QCOM_WIN_ENV
#include <windows.h>
#include <cstdio>
#define LOAD_SYM(sym)                                                                \
    do {                                                                             \
        fn_##sym = (sym##_t)GetProcAddress(g_libusb_handle, #sym);                   \
        if (!fn_##sym) {                                                             \
             Utils::QCD_Printf(Utils::Error, "Failed to load symbol %s (err=%lu)\n", \
                #sym, GetLastError());                                               \
            unload_libusb();                                                         \
            return false;                                                            \
        }                                                                            \
    } while (0)

static HMODULE g_libusb_handle = nullptr;
#else
#include <dlfcn.h>
#define LOAD_SYM(sym)                                                                                 \
    do {                                                                                              \
	    dlerror();                                                                                    \
	    fn_##sym = (sym##_t)dlsym(g_libusb_handle, #sym);                                             \
	    const char* dlsym_error = dlerror();                                                          \
        if (dlsym_error || !fn_##sym) {                                                               \
		    Utils::QCD_Printf(Utils::Error, "Failed to load symbol %s: %s\n", #sym,                   \
                        dlsym_error ? dlsym_error : "unknown error");                                 \
            unload_libusb();                                                                          \
            return false;                                                                             \
	    }                                                                                             \
    } while(0)                                                                                        

static void* g_libusb_handle = nullptr;
#endif // QCOM_WIN_ENV

libusb_init_t                 fn_libusb_init = nullptr;
libusb_exit_t                 fn_libusb_exit = nullptr;
libusb_open_t                 fn_libusb_open = nullptr;
libusb_close_t                fn_libusb_close = nullptr;
libusb_bulk_transfer_t        fn_libusb_bulk_transfer = nullptr;
libusb_get_device_list_t      fn_libusb_get_device_list = nullptr;
libusb_free_device_list_t     fn_libusb_free_device_list = nullptr;
libusb_get_device_descriptor_t fn_libusb_get_device_descriptor = nullptr;
libusb_get_string_descriptor_ascii_t
fn_libusb_get_string_descriptor_ascii = nullptr;
libusb_get_active_config_descriptor_t
fn_libusb_get_active_config_descriptor = nullptr;
libusb_get_bus_number_t       fn_libusb_get_bus_number = nullptr;
libusb_get_device_address_t   fn_libusb_get_device_address = nullptr;
libusb_unref_device_t         fn_libusb_unref_device = nullptr;
libusb_ref_device_t           fn_libusb_ref_device = nullptr;
libusb_has_capability_t       fn_libusb_has_capability = nullptr;
libusb_hotplug_register_callback_t
fn_libusb_hotplug_register_callback = nullptr;
libusb_hotplug_deregister_callback_t fn_libusb_hotplug_deregister_callback = nullptr;
libusb_error_name_t           fn_libusb_error_name = nullptr;
libusb_strerror_t             fn_libusb_strerror = nullptr;
libusb_free_config_descriptor_t
fn_libusb_free_config_descriptor = nullptr;

libusb_kernel_driver_active_t fn_libusb_kernel_driver_active = nullptr;
libusb_detach_kernel_driver_t fn_libusb_detach_kernel_driver = nullptr;
libusb_claim_interface_t      fn_libusb_claim_interface = nullptr;
libusb_release_interface_t    fn_libusb_release_interface = nullptr;
libusb_clear_halt_t           fn_libusb_clear_halt = nullptr;
libusb_get_port_numbers_t                   fn_libusb_get_port_numbers = nullptr;
libusb_handle_events_timeout_completed_t    fn_libusb_handle_events_timeout_completed = nullptr;


bool load_libusb()
{
#ifdef QCOM_WIN_ENV
    if (g_libusb_handle)
        return true;

    const char* dll_file_name = "libusb-1.0.dll";
#if defined(_M_ARM64)
    const char* arch_subdir = "ARM64";
#elif defined(_M_IX86)
    const char* arch_subdir = "x86";
#else
    const char* arch_subdir = "x64"; // fallback
#endif
    std::string base = "C:\\Program Files (x86)\\Qualcomm\\QUD-Userspace\\DriverPackage\\libusb\\";
    std::string full_path = base + arch_subdir + "\\" + dll_file_name;
    g_libusb_handle = LoadLibraryA(full_path.c_str());
    if (!g_libusb_handle) {
        Utils::QCD_Printf(Utils::Error, "Failed to load %s (err=%lu)\n", full_path.c_str(), GetLastError());
		Utils::QCD_Printf(Utils::Error, "Trying other locations...\n");
#if defined(_M_IX86)
        base = "C:\\Program Files (x86)\\Qualcomm\\Qualcomm USB Drivers\\libusb\\"; // Compatible with userspace driver 1.0.2.2 and later
#else
        base = "C:\\Program Files\\Qualcomm\\Qualcomm USB Drivers\\libusb\\"; // Compatible with userspace driver 1.0.2.2 and later
#endif
		full_path = base + dll_file_name;
		g_libusb_handle = LoadLibraryA(full_path.c_str());
		if (!g_libusb_handle)
		{
            Utils::QCD_Printf(Utils::Error, "Failed to load %s (err=%lu)\n", full_path.c_str(), GetLastError());
            return false;
		}
    }
#else
    if (g_libusb_handle)
        return true;
    /* Try RTLD_NOLOAD first: if libusb is already mapped (e.g. because the
     * caller's dylib was linked directly with -lusb-1.0), reuse that handle
     * instead of opening a second conflicting copy with RTLD_LOCAL.
     * On both macOS and Linux dlopen(path, RTLD_NOLOAD) returns the existing
     * handle (and increments its ref-count) without remapping the library. */
    const char* noload_libs[] = {
        "/Applications/Qualcomm/QUTS/QUTS.app/Contents/Frameworks/libusb-1.0.0.dylib",
        /* macOS — Intel Homebrew (present on the test machine) */
        "/usr/local/opt/libusb/lib/libusb-1.0.0.dylib",
        /* macOS — Apple Silicon Homebrew */
        "/opt/homebrew/opt/libusb/lib/libusb-1.0.0.dylib",
        /* macOS bare name */
        "libusb-1.0.0.dylib",
        /* Linux */
        "libusb-1.0.so.0",
        "libusb-1.0.so",
    };
    for (auto lib : noload_libs) {
        void *probe = dlopen(lib, RTLD_NOLOAD | RTLD_NOW | RTLD_GLOBAL);
        if (probe) {
            /* Library already mapped.  Use RTLD_DEFAULT so that dlsym
             * resolves through the process-global symbol table — the same
             * binding the dylib's own direct-link references use.  Using
             * the RTLD_LOCAL/NOLOAD handle here gives a different address
             * than the compile-time stub, causing a crash on first call. */
            dlclose(probe); /* release the NOLOAD ref-count increment */
            g_libusb_handle = RTLD_DEFAULT;
            fprintf(stderr, "[load_libusb] libusb already mapped; using RTLD_DEFAULT for dlsym\n");
            break;
        }
    }
    /* Only do a fresh dlopen if RTLD_NOLOAD found nothing. */
    if (!g_libusb_handle) {
    const char* libs[] = {
        "/Applications/Qualcomm/QUTS/QUTS.app/Contents/Frameworks/libusb-1.0.0.dylib",
        /* Linux */
        "libusb-1.0.so.0",
        "libusb-1.0.so",
        "/usr/lib/libusb-1.0.so.0",
        "/usr/lib64/libusb-1.0.so.0",
        /* macOS — Apple Silicon (Homebrew) */
        "/opt/homebrew/opt/libusb/lib/libusb-1.0.0.dylib",
        /* macOS — Intel (Homebrew) */
        "/usr/local/opt/libusb/lib/libusb-1.0.0.dylib",
        /* macOS — MacPorts / system fallback */
        "/opt/local/lib/libusb-1.0.0.dylib",
        "libusb-1.0.0.dylib",
    };
    for (auto lib : libs) {
        g_libusb_handle = dlopen(lib, RTLD_NOW | RTLD_LOCAL);
        if (g_libusb_handle)
        {
            break;
        }
    }
    if (!g_libusb_handle) {
        Utils::QCD_Printf(Utils::Error, "Failed to load libusb-1.0.so.0: %s\n", dlerror());
        return false;
    }
    else {
        Utils::QCD_Printf(Utils::Info, "Successfully loaded libusb-1.0.so.0.\n");
    }
    } /* end if (!g_libusb_handle) — fresh dlopen block */
#endif // QCOM_WIN_ENV
    LOAD_SYM(libusb_init);
    LOAD_SYM(libusb_exit);
    LOAD_SYM(libusb_open);
    LOAD_SYM(libusb_close);
    LOAD_SYM(libusb_bulk_transfer);
    LOAD_SYM(libusb_get_device_list);
    LOAD_SYM(libusb_free_device_list);
    LOAD_SYM(libusb_get_device_descriptor);
    LOAD_SYM(libusb_get_string_descriptor_ascii);
    LOAD_SYM(libusb_get_active_config_descriptor);
    LOAD_SYM(libusb_get_bus_number);
    LOAD_SYM(libusb_get_device_address);
    LOAD_SYM(libusb_unref_device);
    LOAD_SYM(libusb_ref_device);
    LOAD_SYM(libusb_has_capability);
    LOAD_SYM(libusb_hotplug_register_callback);
    LOAD_SYM(libusb_hotplug_deregister_callback);
    LOAD_SYM(libusb_error_name);
    LOAD_SYM(libusb_strerror);
    LOAD_SYM(libusb_free_config_descriptor);
    LOAD_SYM(libusb_kernel_driver_active);
    LOAD_SYM(libusb_detach_kernel_driver);
    LOAD_SYM(libusb_claim_interface);
    LOAD_SYM(libusb_release_interface);
    LOAD_SYM(libusb_clear_halt);
    LOAD_SYM(libusb_get_port_numbers);
    LOAD_SYM(libusb_handle_events_timeout_completed);
    return true;
}

bool is_libusb_loaded() {
    return g_libusb_handle != nullptr;
}

void unload_libusb()
{
#ifdef QCOM_WIN_ENV
    if (!FreeLibrary(g_libusb_handle)) {
        DWORD err = GetLastError();
        Utils::QCD_Printf(Utils::Error,
            "FreeLibrary failed (err=%lu)\n", err);
    }
    else {
        Utils::QCD_Printf(Utils::Info,
            "FreeLibrary succeeded\n");
    }
    g_libusb_handle = nullptr;
#else
    /* RTLD_DEFAULT is a sentinel, not a real handle — never pass it to dlclose. */
    if (g_libusb_handle && g_libusb_handle != RTLD_DEFAULT) {
        if (dlclose(g_libusb_handle) != 0) {
            const char* err = dlerror();
            Utils::QCD_Printf(Utils::Error,
                "dlclose failed: %s\n", err ? err : "unknown error");
        } else {
            Utils::QCD_Printf(Utils::Info, "dlclose succeeded\n");
        }
    }
    g_libusb_handle = nullptr;
#endif // QCOM_WIN_ENV
    fn_libusb_init = nullptr;
    fn_libusb_exit = nullptr;
    fn_libusb_open = nullptr;
    fn_libusb_close = nullptr;
    fn_libusb_bulk_transfer = nullptr;
    fn_libusb_get_device_list = nullptr;
    fn_libusb_free_device_list = nullptr;
    fn_libusb_get_device_descriptor = nullptr;
    fn_libusb_get_string_descriptor_ascii = nullptr;
    fn_libusb_get_active_config_descriptor = nullptr;
    fn_libusb_get_bus_number = nullptr;
    fn_libusb_get_device_address = nullptr;
    fn_libusb_unref_device = nullptr;
    fn_libusb_ref_device = nullptr;
    fn_libusb_has_capability = nullptr;
    fn_libusb_hotplug_register_callback = nullptr;
    fn_libusb_hotplug_deregister_callback = nullptr;
    fn_libusb_error_name = nullptr;
    fn_libusb_strerror = nullptr;
    fn_libusb_free_config_descriptor = nullptr;
    fn_libusb_kernel_driver_active = nullptr;
    fn_libusb_detach_kernel_driver = nullptr;
    fn_libusb_claim_interface = nullptr;
    fn_libusb_release_interface = nullptr;
    fn_libusb_clear_halt = nullptr;
    fn_libusb_get_port_numbers = nullptr;
    fn_libusb_handle_events_timeout_completed = nullptr;
}
