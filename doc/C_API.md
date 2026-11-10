# QLC C API

QLC provides a C-compatible API for consumers that cannot directly use the
C++ classes in the QLC library. The API is intended for language bindings such
as Rust and for tools that load QLC dynamically.

The public declarations are in:

- `src/exports/qlc_diag_logger.h`
- `src/exports/qlc_export.h`

The shared library is named `DiagLoggerLib` and exports the `qlc_*` functions.

## ABI and ownership

Check the ABI before using the API:

```c
if (qlc_abi_version() != QLC_C_ABI_VERSION) {
    /* incompatible QLC library */
}
```

The ABI uses fixed-width integer types and opaque handles. C callers own all
memory passed to the API. QLC does not retain input strings after a create call
returns, and QLC writes output strings only into caller-provided buffers.

`QlcDiagLoggerHandle` must be released with
`qlc_diag_logger_destroy()`. Handles must not be used after destruction.

The API is not a replacement for the C++ API. C++ classes, STL containers,
exceptions, and callbacks are deliberately kept behind the ABI boundary.

## Status codes

Most functions return `QlcStatus`:

| Status | Meaning |
| --- | --- |
| `QLC_STATUS_OK` | Operation completed successfully. |
| `QLC_STATUS_INVALID_ARGUMENT` | A required pointer or argument was invalid. |
| `QLC_STATUS_INVALID_HANDLE` | An opaque handle is null or invalid. |
| `QLC_STATUS_BUFFER_TOO_SMALL` | The operation completed partially; provide larger buffers. |
| `QLC_STATUS_TIMEOUT` | A wait operation reached its timeout. |
| `QLC_STATUS_NOT_READY` | Logging could not start because the protocol is not ready. |
| `QLC_STATUS_INTERNAL_ERROR` | QLC caught an internal C++ exception. |

Use `qlc_last_error()` for discovery or global errors and
`qlc_diag_logger_last_error()` for errors associated with a logger handle.
Error strings are UTF-8 and use the same caller-buffer convention described
below.

## Caller-provided strings and records

String fields use a pointer plus capacity:

```c
char description[256];
QlcDeviceInfo info = {0};
info.description = description;
info.description_capacity = sizeof(description);
```

For record arrays, call the enumeration function first with `capacity == 0`
to obtain the required record count. Allocate an array, initialize each
record's string pointers and capacities, and call the function again.

For an individual string, pass a null buffer or zero capacity to query its
required size when using an error accessor. A required size includes the
terminating null character. Truncation returns `QLC_STATUS_BUFFER_TOO_SMALL`
and always leaves the destination null-terminated when the capacity is
nonzero.

## Logger configuration

Always initialize configuration structures before setting caller-specific
fields:

```c
QlcDiagLoggerConfig config;
qlc_diag_logger_config_init(&config);

config.cfg_path = "Default.cfg";
config.output_path = "logs";
config.enable_qdss = 1;
```

The initializer sets the same defaults as the C++ API:

- baud rate: `115200`
- command timeout: `2000` ms
- recovery: enabled
- recovery timeout: `120` seconds
- QDSS processor mask: `0x3F`
- ADPL version: `7`

Create and destroy a logger as follows:

```c
QlcDiagLoggerHandle logger = qlc_diag_logger_create(&config);
if (logger == NULL) {
    /* read qlc_last_error() */
}

/* use logger */
qlc_diag_logger_destroy(logger);
```

The normal capture sequence is:

1. Start device monitoring.
2. Enumerate devices and protocols.
3. Create a logger for the selected device/protocol configuration.
4. Request the DIAG protocol to open.
5. Wait for readiness.
6. Start logging.
7. Read statistics or stop logging.
8. Destroy the logger and stop monitoring when finished.

`qlc_diag_logger_start()` returns `QLC_STATUS_NOT_READY` if the DIAG protocol
has not completed its readiness handshake. `request_open()` and
`request_close()` are asynchronous; use `wait_ready()` or `wait_closed()` when
the caller needs a completed state transition.

`qlc_diag_logger_validate_ready()` performs the stronger readiness check used
by the C++ API and returns `QLC_STATUS_INTERNAL_ERROR` with a handle error
message if the protocol is not ready before the timeout.

## Device discovery

Discovery is polling-based in ABI version 1:

```c
QlcStatus status = qlc_device_discovery_start_monitoring(5000);
if (status == QLC_STATUS_OK) {
    size_t required = 0;
    qlc_device_discovery_get_devices(NULL, 0, NULL, &required);
    /* allocate required QlcDeviceInfo records and their string buffers */
}
```

`qlc_device_discovery_get_devices()` returns composite devices. Protocol
records are available separately through
`qlc_device_discovery_get_protocols()`. The protocol records include the
device handle needed to associate each protocol with its parent device and
preserve the complete public `ProtocolInfo` data model.

Live callbacks are intentionally not part of ABI version 1. Consumers should
poll until a callback contract is required and can be added in a later ABI
version.

## Building

The shared target produces:

- Windows: `DiagLoggerLib.dll` and `DiagLoggerLib.lib`
- Linux: `libDiagLoggerLib.so` and its link library as applicable

The static target is also available as `DiagLoggerLib`, built alongside the
shared target from the same sources (see `CMakeLists.txt`). External consumers
should link to the shared import library and make sure the runtime DLL is
available on the platform library search path.

## Threading

The logger owns worker threads internally. Calls to the C API should be
serialized per logger handle unless the operation is explicitly documented as
asynchronous. `request_open()` and `request_close()` only request a state
transition; they do not block until the transition completes.

Do not destroy a logger while another thread is using the same handle.
