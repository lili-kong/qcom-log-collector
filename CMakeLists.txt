# qualcomm-log-collector - Open-source Qualcomm diag protocol logging tool

cmake_minimum_required(VERSION 3.14)
project(qualcomm-log-collector VERSION 1.0.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Set output directories (matches QIL convention for CI/CD pipelines)
if(WIN32)
    set(PLATFORM_NAME ${CMAKE_VS_PLATFORM_NAME})
else()
    set(PLATFORM_NAME ${CMAKE_SYSTEM_PROCESSOR})
endif()

set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY ${CMAKE_SOURCE_DIR}/Build/${CMAKE_SYSTEM_NAME}/${PLATFORM_NAME}/lib)
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY ${CMAKE_SOURCE_DIR}/Build/${CMAKE_SYSTEM_NAME}/${PLATFORM_NAME}/lib)
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_SOURCE_DIR}/Build/${CMAKE_SYSTEM_NAME}/${PLATFORM_NAME}/bin)

# Include compiler warning configuration
include(cmake/CompilerWarnings.cmake)

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
    src/transport/transport_manager.cpp
    src/transport/qcdevice_transport.cpp
    src/transport/transport_factory.cpp
)

set(FILE_WRITER_SOURCES
    src/file_writer/ptrace_writer.cpp
    src/file_writer/binary_writer.cpp
    src/file_writer/qlc_writer.cpp
    src/file_writer/qsr4_writer.cpp
)


set(QLC_SOURCES
    src/qlc/cli_argument_definitions.cpp
    src/qlc/cli_parser.cpp
    src/qlc/cli_help.cpp
)

set(LOGGER_SOURCES
    src/logger/diag_logger.cpp
    src/logger/qdss/qdss.cpp
    src/logger/qdss/qdss_diag_manager.cpp
    src/logger/adpl/adpl.cpp
)

set(EXTERNAL_SOURCES
    src/external/inflate/inflate.cpp
)

# --- All project sources (excluding main.cpp) ---
set(ALL_SOURCES
    ${HDLC_SOURCES}
    ${DIAG_SOURCES}
    ${TRANSPORT_SOURCES}
    ${FILE_WRITER_SOURCES}
    ${QLC_SOURCES}
    ${LOGGER_SOURCES}
    ${EXTERNAL_SOURCES}
    ${QDS_SOURCES}
)

# --- Common include directories ---
set(PROJECT_INCLUDE_DIRS
    ${CMAKE_CURRENT_SOURCE_DIR}/include
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

# --- Main executable ---
add_executable(qlc
    src/main.cpp
    ${ALL_SOURCES}
)
target_include_directories(qlc PRIVATE
    ${PROJECT_INCLUDE_DIRS}
    ${QDS_INCLUDE_DIRS}
)
target_link_libraries(qlc PRIVATE ${PLATFORM_LIBS})
set_compiler_warnings(qlc)

# --- Tests ---
option(BUILD_TESTS "Build unit tests" ON)

if(BUILD_TESTS)
    enable_testing()

    add_library(test_objects OBJECT ${ALL_SOURCES})
    target_include_directories(test_objects PUBLIC
        ${PROJECT_INCLUDE_DIRS}
        ${QDS_INCLUDE_DIRS}
    )
    set_compiler_warnings(test_objects)

    add_subdirectory(tests)
endif()
