cmake_minimum_required(VERSION 3.5.0)
project(QcDevice VERSION 0.1.0 LANGUAGES CXX)

if(UNIX)
	file(GLOB_RECURSE SOURCES
		"${CMAKE_CURRENT_SOURCE_DIR}/qds_lnx/libLNX/*.cpp"
		"${CMAKE_CURRENT_SOURCE_DIR}/libusb/source/*.cpp"
		"${CMAKE_CURRENT_SOURCE_DIR}/common/*.cpp"
	)
	add_library(QcDevice STATIC ${SOURCES})
	target_include_directories(QcDevice PRIVATE
		"${CMAKE_CURRENT_SOURCE_DIR}/qds_lnx/inc"
		"${CMAKE_CURRENT_SOURCE_DIR}/common"
		"${CMAKE_CURRENT_SOURCE_DIR}/libusb/source"
	)
	find_package(PkgConfig REQUIRED)
	pkg_check_modules(LIBUSB REQUIRED libusb-1.0)
	target_include_directories(QcDevice PRIVATE ${LIBUSB_INCLUDE_DIRS})
	target_link_libraries(QcDevice PRIVATE ${LIBUSB_LIBRARIES})
	target_compile_options(QcDevice PRIVATE -fPIC -fpermissive -Wno-narrowing -Wno-write-strings)
	target_compile_definitions(QcDevice PRIVATE TOOLS_TARGET_LINUX)
elseif(WIN32)
	file(GLOB_RECURSE SOURCES
		"${CMAKE_CURRENT_SOURCE_DIR}/qds_win/*.*"
		"${CMAKE_CURRENT_SOURCE_DIR}/libusb/source/*.*"
		"${CMAKE_CURRENT_SOURCE_DIR}/common/*.cpp"
		"${CMAKE_CURRENT_SOURCE_DIR}/common/*.h"
	)
	add_library(QcDevice STATIC
		${SOURCES}
	)
	target_include_directories(QcDevice PRIVATE
		"${CMAKE_CURRENT_SOURCE_DIR}/qds_win"
		"${CMAKE_CURRENT_SOURCE_DIR}"
		"${CMAKE_CURRENT_SOURCE_DIR}/libusb/lib-1.0.27/include"
	)
	target_compile_definitions(QcDevice PRIVATE
		TOOLS_TARGET_WINDOWS
		_UNICODE
		UNICODE
		WIN32
		NOMINMAX
		$<$<CONFIG:Debug>:_DEBUG>
		$<$<CONFIG:Release>:NDEBUG>
	)
endif()