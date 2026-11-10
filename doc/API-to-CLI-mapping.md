# QLC API-to-CLI mapping

This document maps the `qlc` command-line behavior to the public APIs exported
by `DiagLoggerLib`.

The public DLL surface is provided by `src/exports/diag_logger.h`,
`src/exports/device_discovery.h`, and `src/exports/Definitions.h`.

| CLI command/function | API function | Description matching the CLI behavior |
|---|---|---|
| `qlc --devices` / `qlc --collect-log` | `QC::DeviceDiscovery::start_monitor()` | Starts QcDevice monitoring so connected DIAG, QDSS, and ADPL interfaces can be discovered. |
| `qlc --devices` | `QC::DeviceDiscovery::enumerate_composite_devices()` | Enumerates connected devices grouped by composite parent and returns their available protocols. |
| `qlc --collect-log` | `QC::DeviceDiscovery::enumerate_composite_devices()` | Returns the device list that the CLI searches by description, MSM serial number, or ADB serial number to resolve the corresponding DIAG device and optional QDSS/ADPL interfaces. |
| `qlc --devices` / `qlc --collect-log` cleanup | `QC::DeviceDiscovery::stop_monitor()` | Stops the QcDevice monitor when the command finishes or fails. |
| `qlc --collect-log` | `QC::DiagLogger::DiagLogger(DiagLoggerConfig)` | Creates a logging session using the CFG path, device ports, baud rate, output directory, duration, recovery, QDSS, and ADPL settings. |
| `qlc --collect-log` startup | `QC::DiagLogger::request_open(PROT_DIAG)` | Asynchronously requests that the DIAG protocol open and begin its connection/handshake lifecycle. |
| `qlc --collect-log` startup | `QC::DiagLogger::wait_ready(timeout, PROT_DIAG)` | Waits for the DIAG port to open. `QC::DiagLogger::start()` then validates readiness/handshake before beginning capture. |
| `qlc --collect-log` startup | `QC::DiagLogger::start()` | Validates the DIAG protocol, parses the CFG mask file, configures device logging, and starts diagnostic capture. QDSS and ADPL setup is performed when enabled in the configuration. Only called on the initial startup — on a DIAG recovery reconnect, `start_diaglogger()` finds `is_running()` already true and skips `start()`, since the existing service's recovery worker reapplies the setup instead. |
| `qlc --collect-log` | `QC::DiagLogger::is_running()` | Reports whether the logging session remains active while the CLI waits for the duration limit or a stop signal. |
| `qlc --collect-log` shutdown | `QC::DiagLogger::stop()` | Gracefully stops logging, disables configured masks, tears down capture, and joins logging workers. |
| `qlc --collect-log` cleanup | `QC::DiagLogger::request_close(PROT_DIAG)` | Asynchronously requests that the DIAG protocol close after logging stops. |
| `qlc --collect-log` cleanup | `QC::DiagLogger::wait_closed(timeout, PROT_DIAG)` | Waits for the DIAG protocol close operation to complete. |
| `qlc --collect-log --cfg=<file>` | `DiagLoggerConfig::cfg_path` | Supplies the CFG mask file consumed by `QC::DiagLogger::start()`. |
| `qlc --collect-log --device=<device>` | `DiagLoggerConfig::port_name` | Supplies the resolved DIAG device port. |
| `qlc --collect-log --duration=<seconds>` | `DiagLoggerConfig::duration_seconds` | Controls the CLI wait loop; the logger itself reports activity through `is_running()`. |
| `qlc --collect-log --out=<directory>` | `DiagLoggerConfig::output_path` | Selects the output directory for diagnostic, metadata, QDSS, ADPL, and trace files. |
| `qlc --collect-log --baud=<rate>` | `DiagLoggerConfig::baud_rate` | Sets the transport baud rate. |
| `qlc --collect-log --verbose` | `DiagLoggerConfig::verbose` and `KL::Logger::get_instance().setLevel()` / `setOptions()` | Enables verbose diagnostic output and debug logging. |
| `qlc --collect-log --hdlc` | `DiagLoggerConfig::hdlc_only` | Forces HDLC mode and skips the Non-HDLC framing switch. |
| `qlc --collect-log --no-recovery` | `DiagLoggerConfig::enable_recovery` | Disables automatic recovery after a device disconnect or reboot. |
| `qlc --collect-log --recovery-timeout=<seconds>` | `DiagLoggerConfig::recovery_timeout_seconds` | Sets the maximum recovery wait time. |
| `qlc --collect-log --enable-qdss` | `DiagLoggerConfig::enable_qdss` and `DiagLoggerConfig::qdss_port_name` | Enables QDSS capture and supplies the resolved QDSS port. |
| `qlc --collect-log --enable-adpl` | `DiagLoggerConfig::enable_adpl` and `DiagLoggerConfig::adpl_port_name` | Enables ADPL capture and supplies the resolved ADPL port. |
| `Ctrl+C` / `SIGTERM` | `QC::DiagLogger::stop()` | Converts the signal into a graceful shutdown handled by the CLI control flow. |
| `qlc --devices` / `qlc --collect-log` | `KL::Logger::get_instance().init()` | Initializes the CLI application's logging and output/ptrace directories. `--help`, `--version`, and parse-error paths return before this initialization. |
| `qlc --collect-log --verbose` | `KL::Logger::get_instance().setLevel()` and `setOptions()` | Enables debug-level logging and debug-to-file output. |
| `qlc --devices` / `qlc --collect-log` cleanup | `KL::Logger::get_instance().flush_and_shutdown()` | Flushes pending CLI log messages and shuts down the CLI logging worker. |

## Public status and callback APIs

The CLI uses these `DeviceDiscovery` callbacks for device arrival, removal, and
protocol add/remove recovery:

```cpp
QC::DeviceDiscovery::set_protocol_added_listener()
QC::DeviceDiscovery::set_protocol_removed_listener()
QC::DeviceDiscovery::set_device_connected_listener()
QC::DeviceDiscovery::set_device_disconnected_listener()
QC::DeviceDiscovery::unsubscribe()
```

The following APIs are public but are not called directly by the CLI branch:

```cpp
QC::DiagLogger::stats()
QC::DiagLogger::validate_diag_ready()
QC::DiagLogger::protocol_state()
QC::DeviceDiscovery::set_protocol_state_listener()
QC::DeviceDiscovery::is_monitor_running()
```

`QC::DiagLogger::is_running()` is used by the CLI duration loop and is mapped
in the table above.

`open_transport()` and `close_transport()` are not part of the exported DLL
API on this branch. Their public replacements are `request_open()` /
`wait_ready()` and `request_close()` / `wait_closed()`.
