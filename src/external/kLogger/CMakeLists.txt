cmake_minimum_required(VERSION 3.15)
project(kLogger)

add_library(kLogger INTERFACE)

target_include_directories(kLogger INTERFACE
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:include>
)