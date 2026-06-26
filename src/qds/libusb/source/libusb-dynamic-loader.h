// Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
// SPDX-License-Identifier: BSD 3-Clause Clear License
#ifndef LIBUSB_DYNAMIC_LOADER
#define LIBUSB_DYNAMIC_LOADER

#include "../lib-1.0.27/include/libusb-1.0/libusb.h"
#include <stdint.h>
#include <stdio.h>
#if defined(_WIN32) || defined(_WIN64)
#define QCOM_WIN_ENV 1
#endif
#if (defined(__GNUC__) && defined(__unix__))
#define QCOM_LNX_ENV 1
#endif
typedef int  (*libusb_init_t)(libusb_context** ctx);

typedef void (*libusb_exit_t)(libusb_context* ctx);

typedef int  (*libusb_open_t)(libusb_device*, libusb_device_handle**);

typedef void (*libusb_close_t)(libusb_device_handle*);

typedef int  (*libusb_bulk_transfer_t)(
        libusb_device_handle*,
        unsigned char,
        unsigned char*,
        int,
        int*,
        unsigned int
        );

typedef int  (*libusb_get_device_list_t)(
        libusb_context*,
        libusb_device***
        );

typedef void (*libusb_free_device_list_t)(libusb_device**, int);

typedef int  (*libusb_get_device_descriptor_t)(
        libusb_device*,
        libusb_device_descriptor*
        );

typedef int (*libusb_get_string_descriptor_ascii_t)(
        libusb_device_handle* dev_handle,
        uint8_t desc_index,
        unsigned char* data,
        int length
        );

typedef int (*libusb_get_active_config_descriptor_t)(
        libusb_device* dev,
        libusb_config_descriptor** config
        );

typedef uint8_t (*libusb_get_bus_number_t)(
        libusb_device* dev
        );

typedef uint8_t (*libusb_get_device_address_t)(
        libusb_device* dev
        );

typedef void (*libusb_unref_device_t)(libusb_device*);
typedef libusb_device* (*libusb_ref_device_t)(libusb_device*);

typedef int (*libusb_has_capability_t)(uint32_t capability);

typedef int (*libusb_hotplug_register_callback_t)(
        libusb_context* ctx,
        int events,
        int flags,
        int vendor_id,
        int product_id,
        int dev_class,
        libusb_hotplug_callback_fn cb_fn,
        void* user_data,
        libusb_hotplug_callback_handle* callback_handle
        );

typedef const char* (*libusb_error_name_t)(int errcode);
typedef const char* (*libusb_strerror_t)(enum libusb_error errcode);

typedef void (*libusb_free_config_descriptor_t)(
        libusb_config_descriptor* config
        );


typedef int (*libusb_kernel_driver_active_t)(
        libusb_device_handle* dev_handle,
        int interface_number
        );

typedef int (*libusb_detach_kernel_driver_t)(
        libusb_device_handle* dev_handle,
        int interface_number
        );

typedef int (*libusb_claim_interface_t)(
        libusb_device_handle* dev_handle,
        int interface_number
        );

typedef int (*libusb_release_interface_t)(
        libusb_device_handle* dev_handle,
        int interface_number
        );

typedef int (*libusb_clear_halt_t)(
        libusb_device_handle* dev_handle,
        unsigned char endpoint
        );

        
typedef int (*libusb_get_port_numbers_t)(
    libusb_device* dev,
    uint8_t* port_numbers,
    int port_numbers_len
);

extern libusb_init_t                 fn_libusb_init;
extern libusb_exit_t                 fn_libusb_exit;
extern libusb_open_t                 fn_libusb_open;
extern libusb_close_t                fn_libusb_close;
extern libusb_bulk_transfer_t        fn_libusb_bulk_transfer;
extern libusb_get_device_list_t      fn_libusb_get_device_list;
extern libusb_free_device_list_t     fn_libusb_free_device_list;
extern libusb_get_device_descriptor_t fn_libusb_get_device_descriptor;
extern libusb_get_string_descriptor_ascii_t
fn_libusb_get_string_descriptor_ascii;
extern libusb_get_active_config_descriptor_t
fn_libusb_get_active_config_descriptor;
extern libusb_get_bus_number_t       fn_libusb_get_bus_number;
extern libusb_get_device_address_t   fn_libusb_get_device_address;
extern libusb_unref_device_t         fn_libusb_unref_device;
extern libusb_ref_device_t           fn_libusb_ref_device;
extern libusb_has_capability_t       fn_libusb_has_capability;
extern libusb_hotplug_register_callback_t
fn_libusb_hotplug_register_callback;
extern libusb_error_name_t           fn_libusb_error_name;
extern libusb_strerror_t             fn_libusb_strerror;
extern libusb_free_config_descriptor_t
fn_libusb_free_config_descriptor;

extern libusb_kernel_driver_active_t fn_libusb_kernel_driver_active;
extern libusb_detach_kernel_driver_t fn_libusb_detach_kernel_driver;
extern libusb_claim_interface_t      fn_libusb_claim_interface;
extern libusb_release_interface_t    fn_libusb_release_interface;
extern libusb_clear_halt_t           fn_libusb_clear_halt;
extern libusb_get_port_numbers_t     fn_libusb_get_port_numbers;

bool load_libusb();
void unload_libusb();
bool is_libusb_loaded();

#endif // LIBUSB_DYNAMIC_LOADER
