# SPDX-License-Identifier: MIT

cmake_minimum_required(VERSION 3.20.0)
include(${CMAKE_CURRENT_SOURCE_DIR}/../common/defy_test.cmake)
option(DEFY_TEST_GRACEFUL_SLEEP "Stopping Bluetooth before the half sleeps, in src/graceful_main.c" OFF)
set(DTC_OVERLAY_FILE ${DEFY_ROOT}/tests/drivers/common/cable_peripheral.overlay)
find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(defy_hybrid_peripheral_test)

target_include_directories(app PRIVATE ${DEFY_ZMK_APP}/include ${DEFY_ROOT}/drivers/split)
if(DEFY_TEST_GRACEFUL_SLEEP)
  target_sources(app PRIVATE src/graceful_main.c src/fake_links.c)
  target_compile_definitions(app PRIVATE CONFIG_DEFY_HALF_SLEEP=1 CONFIG_BT_MAX_CONN=1)
else()
  target_sources(app PRIVATE src/main.c src/fake_links.c)
  # The half's LEDs, for the connection state that the hybrid passes to
  # defy_startup_split_connected(), which src/main.c defines and counts.
  # The graceful build leaves them out, as a hybrid half may.
  target_compile_definitions(app PRIVATE CONFIG_DEFY_HALF_LEDS=1)
endif()
zephyr_linker_sources(SECTIONS ${DEFY_ZMK_APP}/include/linker/zmk-split-transport-peripheral.ld)
