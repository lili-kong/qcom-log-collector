# qualcomm-log-collector - Open-source Qualcomm diag protocol logging tool

cmake_minimum_required(VERSION 3.14)
project(qualcomm-log-collector VERSION 1.0.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Set output directories (matches QIL convention for CI/CD pipelines)
if(WIN32)
    set(PLATFORM_NAME ${CMAKE_VS_PLATFORM_NAME})
else()
    set(PLATFORM_NAME ${CMAKE_SYSTEM_PROCESSOR})
endif()

set(OUTPUT_ROOT ${CMAKE_SOURCE_DIR}/build/${CMAKE_SYSTEM_NAME}/${PLATFORM_NAME})
# Visual Studio needs configuration-specific output directories. Linux keeps
# the same category layout without an additional configuration directory.
if(WIN32)
    set(OUTPUT_CONFIGURATION_SUFFIX /$<CONFIG>)
else()
    set(OUTPUT_CONFIGURATION_SUFFIX)
endif()
set(STATIC_LIBRARY_OUTPUT_DIRECTORY ${OUTPUT_ROOT}/lib${OUTPUT_CONFIGURATION_SUFFIX})
set(DYNAMIC_LIBRARY_OUTPUT_DIRECTORY ${OUTPUT_ROOT}/dll${OUTPUT_CONFIGURATION_SUFFIX})
set(EXECUTABLE_OUTPUT_DIRECTORY ${OUTPUT_ROOT}/bin${OUTPUT_CONFIGURATION_SUFFIX})

set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY ${STATIC_LIBRARY_OUTPUT_DIRECTORY})
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY ${DYNAMIC_LIBRARY_OUTPUT_DIRECTORY})
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${EXECUTABLE_OUTPUT_DIRECTORY})

# Shared compiler warning flags
function(set_compiler_warnings target)
    if(MSVC)
        target_compile_options(${target} PRIVATE
            /W4
            /WX-         # Don't treat warnings as errors (for now)
            /wd4100      # Unreferenced formal parameter
            /wd4189      # Local variable initialized but not referenced
        )
    else()
        target_compile_options(${target} PRIVATE
            -Wall
            -Wextra
            -Wpedantic
            -Wno-unused-parameter
            -Wno-unused-variable
        )
    endif()
endfunction()

# --- Platform detection ---
if(WIN32)
    add_compile_definitions(TOOLS_TARGET_WINDOWS=1)
elseif(UNIX AND NOT APPLE)
    add_compile_definitions(TOOLS_TARGET_LINUX=1)
elseif(APPLE)
    add_compile_definitions(TOOLS_TARGET_OSX=1)
endif()

# --- kLogger library integration ---
set(KLOGGER_DIR "${CMAKE_CURRENT_SOURCE_DIR}/src/external/kLogger")

# kLogger – header-only INTERFACE target
add_library(kLogger INTERFACE)
target_include_directories(kLogger INTERFACE
    "${KLOGGER_DIR}/include"
)

# --- QDS (QcDevice) library integration ---
# QDS sources are vendored into the src/qds/ directory.
set(QDS_DIR "${CMAKE_CURRENT_SOURCE_DIR}/src/qds")

# QDS source files (compiled into qualcomm-log-collector-lib to avoid DLL dependency)
if(WIN32)
    set(QDS_SOURCES
        ${QDS_DIR}/qds_win/scandev.cpp
        ${QDS_DIR}/qds_win/QdIoQmi.cpp
        ${QDS_DIR}/libusb/source/qcom-libusb.cpp
        ${QDS_DIR}/libusb/source/libusb-dynamic-loader.cpp
        ${QDS_DIR}/common/utils.cpp
        ${QDS_DIR}/common/qdver.cpp
    )
    set(QDS_INCLUDE_DIRS
        ${QDS_DIR}/qds_win
        ${QDS_DIR}/common
        ${QDS_DIR}/libusb/source
        ${QDS_DIR}/libusb/lib-1.0.27/include
    )
else()
    set(QDS_SOURCES
        ${QDS_DIR}/qds_lnx/libLNX/qcdev.cpp
        ${QDS_DIR}/qds_lnx/libLNX/QdIoQmi.cpp
        ${QDS_DIR}/qds_lnx/libLNX/qdaio.cpp
        ${QDS_DIR}/qds_lnx/libLNX/liblpc.cpp
        ${QDS_DIR}/libusb/source/qcom-libusb.cpp
        ${QDS_DIR}/libusb/source/libusb-dynamic-loader.cpp
        ${QDS_DIR}/common/utils.cpp
        ${QDS_DIR}/common/qdver.cpp
    )
    set(QDS_INCLUDE_DIRS
        ${QDS_DIR}/qds_lnx/inc
        ${QDS_DIR}/common
        ${QDS_DIR}/libusb/source
        ${QDS_DIR}/libusb/lib-1.0.27/include
    )
endif()

# EUD support is shared by the Windows and Linux QDS implementations.
list(APPEND QDS_SOURCES
    ${QDS_DIR}/EUD/QdsEud.cpp
    ${QDS_DIR}/EUD/QdsEud.h
)

# --- Source files ---
set(HDLC_SOURCES
    src/hdlc/hdlc.cpp
    src/hdlc/non_hdlc.cpp
)

set(DIAG_SOURCES
    src/diag/commands.cpp
    src/diag/cfg_parser.cpp
    src/diag/subsystem_commands.cpp
    src/diag/qdss_commands.cpp
    src/diag/adpl_commands.cpp
    src/diag/handshake_commands.cpp
    src/diag/response_pairing.cpp
)

set(TRANSPORT_SOURCES
    src/transport/transport.cpp
    src/transport/transport_manager.cpp
    src/transport/qcdevice_transport.cpp
    src/transport/transport_factory.cpp
)

set(PROTOCOL_SOURCES
    src/protocol/protocol.cpp
    src/protocol/diag.cpp
    src/protocol/qdss/qdss.cpp
    src/protocol/adpl/adpl.cpp
)

set(FILE_WRITER_SOURCES
    src/file_writer/ptrace_writer.cpp
    src/file_writer/binary_writer.cpp
    src/file_writer/qlc_writer.cpp
    src/file_writer/qsr4_writer.cpp
)


set(CLI_SOURCES
    src/cli/cli_argument_definitions.cpp
    src/cli/cli_parser.cpp
    src/cli/cli_help.cpp
)

set(FUNCTION_SOURCES
    src/function/diag_logger_service.cpp
    src/function/diag_logger_qdss_manager.cpp
)

set(EXTERNAL_SOURCES
    src/external/inflate/inflate.cpp
)

# --- Public API implementation (src/exports/ headers) ---
# Thin wrappers over the internal engine; the only sources whose declarations
# are marked QLC_API and exported from the shared library.
set(EXPORTS_SOURCES
    src/device_discovery.cpp
    src/diag_logger.cpp
    src/DiagLoggerC.cpp
)

# --- All engine sources (the library body; excludes src/cli/) ---
# CLI_SOURCES is deliberately NOT here: argument parsing and help text belong to
# the qlc executable, not to the SDK library.
set(ALL_SOURCES
    ${HDLC_SOURCES}
    ${DIAG_SOURCES}
    ${TRANSPORT_SOURCES}
    ${PROTOCOL_SOURCES}
    ${FILE_WRITER_SOURCES}
    ${FUNCTION_SOURCES}
    ${EXTERNAL_SOURCES}
    ${QDS_SOURCES}
)

# --- Public API headers (src/exports/) ---
set(EXPORTS_HEADERS
    src/exports/Definitions.h
    src/exports/device_discovery.h
    src/exports/diag_logger.h
    src/exports/qlc_export.h
    src/exports/qlc_diag_logger.h
)

# --- Common include directories ---
set(PROJECT_INCLUDE_DIRS
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${CMAKE_CURRENT_SOURCE_DIR}/src/external/kLogger/include
    ${CMAKE_CURRENT_SOURCE_DIR}/src/external/inflate
    ${KLOGGER_DIR}/include
)

# QDS headers are included via PROJECT_INCLUDE_DIRS for targets that need them.
if(WIN32)
    set_source_files_properties(${QDS_SOURCES} PROPERTIES
        COMPILE_DEFINITIONS "UNICODE;_UNICODE"
    )
endif()

# Disable compiler warnings for third-party QDS sources
if(MSVC)
    set_source_files_properties(${QDS_SOURCES} PROPERTIES
        COMPILE_FLAGS "/W0"
    )
else()
    set_source_files_properties(${QDS_SOURCES} PROPERTIES
        COMPILE_FLAGS "-w -fpermissive"
    )
endif()

# Platform-specific link libraries
set(PLATFORM_LIBS "")
if(WIN32)
    list(APPEND PLATFORM_LIBS kLogger setupapi cfgmgr32 shlwapi)
    # libusb static library for QDS
    if(CMAKE_VS_PLATFORM_NAME MATCHES "ARM64" OR CMAKE_SYSTEM_PROCESSOR MATCHES "ARM64|aarch64")
        set(LIBUSB_ARCH "arm64")
    elseif(CMAKE_SIZEOF_VOID_P EQUAL 8)
        set(LIBUSB_ARCH "x64")
    else()
        set(LIBUSB_ARCH "x86")
    endif()
    set(LIBUSB_LIB_DIR "${QDS_DIR}/libusb/lib-1.0.27/lib/${LIBUSB_ARCH}/release")
    if(EXISTS "${LIBUSB_LIB_DIR}/libusb-1.0.lib")
        list(APPEND PLATFORM_LIBS "${LIBUSB_LIB_DIR}/libusb-1.0.lib")
    endif()
elseif(UNIX)
    find_package(Threads REQUIRED)
    list(APPEND PLATFORM_LIBS kLogger Threads::Threads udev)
    # libusb static library for Linux
    set(LIBUSB_LIB_DIR "${QDS_DIR}/libusb/lib-1.0.27/lib/linux")
    if(EXISTS "${LIBUSB_LIB_DIR}/libusb-1.0.a")
        list(APPEND PLATFORM_LIBS "${LIBUSB_LIB_DIR}/libusb-1.0.a")
    endif()
endif()

# --- Public SDK libraries: static for qlc, shared for external consumers ---
# Both flavours use the same sources and export name, matching QIL's target
# convention. The static archive goes in lib/; the shared library and its
# Windows import library go in dll/ so their .lib files cannot collide.
function(qlc_configure_sdk_target target)
    target_include_directories(${target}
        PUBLIC ${PROJECT_INCLUDE_DIRS}
        PRIVATE ${QDS_INCLUDE_DIRS}
    )
    target_link_libraries(${target} PRIVATE ${PLATFORM_LIBS})
    set_compiler_warnings(${target})
    set_target_properties(${target} PROPERTIES
        POSITION_INDEPENDENT_CODE ON
        OUTPUT_NAME "DiagLoggerLib"
    )
endfunction()

# Static flavour: linked into qlc and available to static SDK consumers.
add_library(DiagLoggerLib STATIC
    ${EXPORTS_SOURCES}
    ${EXPORTS_HEADERS}
    ${ALL_SOURCES}
)
qlc_configure_sdk_target(DiagLoggerLib)
target_compile_definitions(DiagLoggerLib PUBLIC QLC_STATIC)
set_target_properties(DiagLoggerLib PROPERTIES
    ARCHIVE_OUTPUT_DIRECTORY ${STATIC_LIBRARY_OUTPUT_DIRECTORY}
)

# Shared flavour: exported SDK DLL/shared object for external consumers.
add_library(DiagLoggerLibShared SHARED
    ${EXPORTS_SOURCES}
    ${EXPORTS_HEADERS}
    ${ALL_SOURCES}
)
qlc_configure_sdk_target(DiagLoggerLibShared)
target_compile_definitions(DiagLoggerLibShared PRIVATE QLC_DLL_EXPORTS)
set_target_properties(DiagLoggerLibShared PROPERTIES
    CXX_VISIBILITY_PRESET hidden
    VISIBILITY_INLINES_HIDDEN ON
    RUNTIME_OUTPUT_DIRECTORY ${DYNAMIC_LIBRARY_OUTPUT_DIRECTORY}
    LIBRARY_OUTPUT_DIRECTORY ${DYNAMIC_LIBRARY_OUTPUT_DIRECTORY}
    ARCHIVE_OUTPUT_DIRECTORY ${DYNAMIC_LIBRARY_OUTPUT_DIRECTORY}
)

# Keep the former in-tree target names usable for add_subdirectory consumers.
add_library(DiagLogger ALIAS DiagLoggerLibShared)
add_library(DiagLoggerStatic ALIAS DiagLoggerLib)

# --- CLI executable ---
# Only the CLI translation units; every diag/transport/protocol source is
# provided by the statically linked DiagLoggerLib library.
add_executable(qlc
    src/cli/main.cpp
    ${CLI_SOURCES}
)
target_include_directories(qlc PRIVATE ${PROJECT_INCLUDE_DIRS})
# QLC_STATIC is propagated by the static library target; qlc does not depend on
# the shared DLL at runtime.
target_link_libraries(qlc PRIVATE DiagLoggerLib kLogger)
set_compiler_warnings(qlc)

# --- Tests ---
option(BUILD_TESTS "Build unit tests" ON)

if(BUILD_TESTS)
    enable_testing()

    add_library(test_objects OBJECT ${ALL_SOURCES} ${EXPORTS_SOURCES})
    target_include_directories(test_objects PUBLIC
        ${PROJECT_INCLUDE_DIRS}
        ${QDS_INCLUDE_DIRS}
    )
    # Same rationale as qlc: tests compile the sources directly, so the public
    # API must not be declared dllimport. PUBLIC so test executables agree.
    target_compile_definitions(test_objects PUBLIC QLC_STATIC)
    set_compiler_warnings(test_objects)

    add_subdirectory(tests)
endif()
