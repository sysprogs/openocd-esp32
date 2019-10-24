cmake_minimum_required (VERSION 3.0)
project (OpenOCD)

INCLUDE (CheckIncludeFiles)
SET(OPENOCD_VERSION 0.10.0)

include(config.cmake)

find_path(HIDAPI_INCLUDE_DIR hidapi.h
	HINTS ${CMAKE_SYSTEM_INCLUDE_PATH}
	PATH_SUFFIXES hidapi)

add_subdirectory(libusb1-cmake)
	
add_definitions(-DHAVE_CONFIG_H -DPKGDATADIR="." -DBINDIR="../../bin")
include_directories(${CMAKE_CURRENT_BINARY_DIR} ${HIDAPI_INCLUDE_DIR})
set(CMAKE_C_FLAGS "-std=gnu99")

add_subdirectory(jimtcl-cmake)
add_subdirectory(src)

INSTALL(DIRECTORY tcl/ DESTINATION share/openocd/scripts)
INSTALL(DIRECTORY contrib DESTINATION share/openocd)
