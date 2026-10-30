# SPDX-License-Identifier: MIT

zephyr_include_directories(include)
include(${CMAKE_CURRENT_LIST_DIR}/cmake/defy_upstream.cmake)

# What the image was built from, for <dygma/defy_build_info.h>: scripts/build
# passes the values, and docs/versions.md describes them.
if(NOT DEFINED DEFY_BUILD_MODULE_VERSION)
  set(DEFY_BUILD_MODULE_VERSION unknown)
endif()
if(NOT DEFINED DEFY_BUILD_SOURCE)
  set(DEFY_BUILD_SOURCE local)
endif()
if(NOT DEFINED DEFY_BUILD_COMMIT)
  set(DEFY_BUILD_COMMIT "")
endif()
configure_file(include/dygma/defy_build_info.h.in
               ${CMAKE_CURRENT_BINARY_DIR}/include/dygma/defy_build_info.h @ONLY)
zephyr_include_directories(${CMAKE_CURRENT_BINARY_DIR}/include)

add_subdirectory(drivers)
add_subdirectory(diagnostics)
add_subdirectory(power)
add_subdirectory(battery)
add_subdirectory(lighting)
add_subdirectory(bootloader)
add_subdirectory(status)
add_subdirectory(updater)
add_subdirectory(studio)
add_subdirectory_ifdef(CONFIG_SOC_RP2040 soc/rp2040)
