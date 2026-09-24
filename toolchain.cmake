if("${TOOLCHAIN_ROOT}" STREQUAL "" AND NOT "$ENV{TOOLCHAIN_ROOT}" STREQUAL "")
    set(TOOLCHAIN_ROOT "$ENV{TOOLCHAIN_ROOT}")
endif()

if("${TOOLCHAIN_ROOT}" STREQUAL "")
    message(WARNING "Missing TOOLCHAIN_ROOT variable. Please specify it via the environment")
endif()

SET(CMAKE_SYSTEM_NAME Generic)
SET(VISUALGDB_TOOLCHAIN_TYPE Unknown)
SET(VISUALGDB_TOOLCHAIN_SUBTYPE GCC)
SET(CMAKE_C_COMPILER "${TOOLCHAIN_ROOT}/bin/gcc.exe")
SET(CMAKE_CXX_COMPILER "${TOOLCHAIN_ROOT}/bin/g++.exe")
SET(CMAKE_ASM_COMPILER "${TOOLCHAIN_ROOT}/bin/g++.exe")

if(EXISTS "${TOOLCHAIN_ROOT}/Qt/v5-CMake/Qt5Cross.cmake")
	include("${TOOLCHAIN_ROOT}/Qt/v5-CMake/Qt5Cross.cmake")
endif()

if(EXISTS "${TOOLCHAIN_ROOT}/Qt/v6-CMake/Qt6Cross.cmake")
	include("${TOOLCHAIN_ROOT}/Qt/v6-CMake/Qt6Cross.cmake")
endif()
