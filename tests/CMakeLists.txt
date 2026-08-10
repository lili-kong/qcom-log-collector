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
