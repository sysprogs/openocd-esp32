cmake_minimum_required (VERSION 3.0)
project (OpenOCD)

INCLUDE (CheckIncludeFiles)
SET(OPENOCD_VERSION 0.12.0)

set(SH_WRAPPER ${CMAKE_CURRENT_SOURCE_DIR}/external/run-sh.bat)
include(config.cmake)

add_subdirectory(external/hidapi/scripts)
add_subdirectory(external/libusb1/scripts)
add_subdirectory(external/zlib)
add_subdirectory(external/prebuilt)
	
add_definitions(-DHAVE_CONFIG_H -DPKGDATADIR="." -DBINDIR="../../bin")
include_directories(${CMAKE_CURRENT_BINARY_DIR})
set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -std=gnu99 -Werror=incompatible-pointer-types")

add_subdirectory(jimtcl-cmake)
add_subdirectory(src)

INSTALL(DIRECTORY tcl/ DESTINATION share/openocd/scripts)
INSTALL(DIRECTORY contrib DESTINATION share/openocd)
