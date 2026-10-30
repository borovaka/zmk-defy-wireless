# SPDX-License-Identifier: MIT

cmake_minimum_required(VERSION 3.20.0)
include(${CMAKE_CURRENT_SOURCE_DIR}/../common/defy_test.cmake)
set(DTC_OVERLAY_FILE ${DEFY_ROOT}/tests/drivers/common/cable_peripheral.overlay)
find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(defy_wired_peripheral_test)

# The cable link inside the hybrid transport, with battery, lighting and the version report.
# The cable needs the hybrid transport in Kconfig, which needs ZMK's Bluetooth split, so
# this builds the driver itself and defines its settings below.
target_sources(app PRIVATE src/main.c ${DEFY_ROOT}/drivers/split/defy_wired_peripheral.c
                           ${DEFY_ROOT}/drivers/split/defy_wired_protocol.c
                           ${DEFY_ROOT}/battery/defy_battery_protocol.c
                           ${DEFY_ROOT}/battery/defy_half_version_protocol.c
                           ${DEFY_ROOT}/lighting/defy_feedback_protocol.c)
target_include_directories(app PRIVATE ${DEFY_ZMK_APP}/include ${DEFY_ROOT}/drivers/split)
zephyr_linker_sources(SECTIONS ${DEFY_ZMK_APP}/include/linker/zmk-split-transport-peripheral.ld)
target_compile_definitions(app PRIVATE
  CONFIG_DEFY_BATTERY=1
  CONFIG_DEFY_BATTERY_HCI=1
  CONFIG_DEFY_HALF_LEDS=1
  CONFIG_DEFY_HALF_VERSION=1
  CONFIG_DEFY_LIGHTING=1
  CONFIG_DEFY_HYBRID=1
  CONFIG_DEFY_HYBRID_IDLE_TIMEOUT_MS=100
  CONFIG_DEFY_HYBRID_IDLE_CABLE_POLL_MS=20
  CONFIG_DEFY_HYBRID_IDLE_CABLE_PROBE_MS=100
  CONFIG_DEFY_HYBRID_CABLE_PROBE_MS=20
  CONFIG_DEFY_WIRED=1
  CONFIG_DEFY_WIRED_PRIORITY=10
  CONFIG_DEFY_WIRED_POLL_INTERVAL_MS=2
  CONFIG_DEFY_WIRED_PERIPHERAL_TIMEOUT_MS=100
  CONFIG_DEFY_WIRED_PERIPHERAL_STACK_SIZE=4096
  CONFIG_DEFY_WIRED_PERIPHERAL_THREAD_PRIORITY=5)
