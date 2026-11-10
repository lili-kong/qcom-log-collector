# qualcomm-log-collector unit tests

# HDLC round-trip tests
add_executable(test_hdlc test_hdlc.cpp)
target_link_libraries(test_hdlc PRIVATE test_objects ${PLATFORM_LIBS})
add_test(NAME hdlc_tests COMMAND test_hdlc)

# Command builder tests
add_executable(test_commands test_commands.cpp)
target_link_libraries(test_commands PRIVATE test_objects ${PLATFORM_LIBS})
add_test(NAME command_tests COMMAND test_commands)

# CFG parser tests
add_executable(test_cfg_parser test_cfg_parser.cpp)
target_link_libraries(test_cfg_parser PRIVATE test_objects ${PLATFORM_LIBS})
add_test(NAME cfg_parser_tests COMMAND test_cfg_parser)

# Subsystem command tests
add_executable(test_subsystem_commands test_subsystem_commands.cpp)
target_link_libraries(test_subsystem_commands PRIVATE test_objects ${PLATFORM_LIBS})
add_test(NAME subsystem_command_tests COMMAND test_subsystem_commands)

# QDSS command tests
add_executable(test_qdss_commands test_qdss_commands.cpp)
target_link_libraries(test_qdss_commands PRIVATE test_objects ${PLATFORM_LIBS})
add_test(NAME qdss_command_tests COMMAND test_qdss_commands)

# ADPL command tests
add_executable(test_adpl_commands test_adpl_commands.cpp)
target_link_libraries(test_adpl_commands PRIVATE test_objects ${PLATFORM_LIBS})
add_test(NAME adpl_command_tests COMMAND test_adpl_commands)

# Handshake command tests
add_executable(test_handshake_commands test_handshake_commands.cpp)
target_link_libraries(test_handshake_commands PRIVATE test_objects ${PLATFORM_LIBS})
add_test(NAME handshake_command_tests COMMAND test_handshake_commands)

# Non-HDLC encoder/decoder tests
add_executable(test_non_hdlc test_non_hdlc.cpp)
target_link_libraries(test_non_hdlc PRIVATE test_objects ${PLATFORM_LIBS})
add_test(NAME non_hdlc_tests COMMAND test_non_hdlc)

# Response pairing tests
add_executable(test_response_pairing test_response_pairing.cpp)
target_link_libraries(test_response_pairing PRIVATE test_objects ${PLATFORM_LIBS})
add_test(NAME response_pairing_tests COMMAND test_response_pairing)

# Device info command/response tests
add_executable(test_device_info test_device_info.cpp)
target_link_libraries(test_device_info PRIVATE test_objects ${PLATFORM_LIBS})
add_test(NAME device_info_tests COMMAND test_device_info)

# Binary writer tests
add_executable(test_binary_writer test_binary_writer.cpp)
target_link_libraries(test_binary_writer PRIVATE test_objects ${PLATFORM_LIBS})
add_test(NAME binary_writer_tests COMMAND test_binary_writer)

# DIAG ptrace writer tests
add_executable(test_ptrace_writer test_ptrace_writer.cpp)
target_link_libraries(test_ptrace_writer PRIVATE test_objects ${PLATFORM_LIBS})
add_test(NAME ptrace_writer_tests COMMAND test_ptrace_writer)

# QLC metadata writer tests
add_executable(test_qlc_writer test_qlc_writer.cpp)
target_link_libraries(test_qlc_writer PRIVATE test_objects ${PLATFORM_LIBS})
add_test(NAME qlc_writer_tests COMMAND test_qlc_writer)

# Inflater tests
add_executable(test_inflate test_inflate.cpp)
target_link_libraries(test_inflate PRIVATE test_objects ${PLATFORM_LIBS})
add_test(NAME inflate_tests COMMAND test_inflate)

# QSR4 writer tests
add_executable(test_qsr4_writer test_qsr4_writer.cpp)
target_link_libraries(test_qsr4_writer PRIVATE test_objects ${PLATFORM_LIBS})
add_test(NAME qsr4_writer_tests COMMAND test_qsr4_writer)

# C ABI smoke tests
add_executable(test_qlc_c_api test_qlc_c_api.cpp)
target_link_libraries(test_qlc_c_api PRIVATE test_objects ${PLATFORM_LIBS})
add_test(NAME qlc_c_api_tests COMMAND test_qlc_c_api)

# Compile and link a pure C consumer against the static wrapper.
add_executable(test_qlc_c_header test_qlc_c_header.c)
target_link_libraries(test_qlc_c_header PRIVATE test_objects ${PLATFORM_LIBS})
add_test(NAME qlc_c_header_tests COMMAND test_qlc_c_header)

# Verify the exported C ABI through the shared library target itself.
add_executable(test_qlc_c_api_shared test_qlc_c_api_shared.c)
target_link_libraries(test_qlc_c_api_shared PRIVATE DiagLoggerLibShared)
set_target_properties(test_qlc_c_api_shared PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${DYNAMIC_LIBRARY_OUTPUT_DIRECTORY}
)
if(UNIX)
    set_target_properties(test_qlc_c_api_shared PROPERTIES
        BUILD_RPATH ${DYNAMIC_LIBRARY_OUTPUT_DIRECTORY}
    )
endif()
add_test(NAME qlc_c_api_shared_tests COMMAND test_qlc_c_api_shared)

# Optional live hardware diagnostic for the exported discovery ABI. This is
# intentionally not part of the normal ctest suite because it requires a
# connected Qualcomm device.
add_executable(test_qlc_discovery_live test_qlc_discovery_live.cpp)
target_link_libraries(test_qlc_discovery_live PRIVATE DiagLoggerLibShared)
set_target_properties(test_qlc_discovery_live PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${DYNAMIC_LIBRARY_OUTPUT_DIRECTORY}
)

# Protocol lifecycle cancellation without attached hardware.
add_executable(test_protocol_lifecycle test_protocol_lifecycle.cpp)
target_link_libraries(test_protocol_lifecycle PRIVATE test_objects ${PLATFORM_LIBS})
add_test(NAME protocol_lifecycle_tests COMMAND test_protocol_lifecycle)
set_tests_properties(protocol_lifecycle_tests PROPERTIES TIMEOUT 30)

# Exercise Windows QDS boundaries without attached hardware.
if(WIN32)
    add_executable(test_qcdevice_transport
        test_qcdevice_transport.cpp
        ${PROJECT_SOURCE_DIR}/src/transport/qcdevice_transport.cpp
        ${PROJECT_SOURCE_DIR}/src/transport/transport.cpp
    )
    target_include_directories(test_qcdevice_transport PRIVATE
        ${PROJECT_INCLUDE_DIRS}
        ${QDS_INCLUDE_DIRS}
    )
    target_compile_definitions(test_qcdevice_transport PRIVATE QLC_STATIC QCDEVLIB_STATIC_LINK)
    target_link_libraries(test_qcdevice_transport PRIVATE kLogger)
    add_test(NAME qcdevice_transport_tests COMMAND test_qcdevice_transport)
    set_tests_properties(qcdevice_transport_tests PROPERTIES TIMEOUT 15)

    add_executable(test_transport_manager
        test_transport_manager.cpp
        ${PROJECT_SOURCE_DIR}/src/transport/transport_manager.cpp
    )
    target_include_directories(test_transport_manager PRIVATE
        ${PROJECT_INCLUDE_DIRS}
        ${QDS_INCLUDE_DIRS}
    )
    target_compile_definitions(test_transport_manager PRIVATE QLC_STATIC QCDEVLIB_STATIC_LINK)
    target_link_libraries(test_transport_manager PRIVATE kLogger)
    add_test(NAME transport_manager_tests COMMAND test_transport_manager)
endif()
