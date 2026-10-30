# SPDX-License-Identifier: MIT

cmake_minimum_required(VERSION 3.20.0)
include(${CMAKE_CURRENT_SOURCE_DIR}/../common/defy_test.cmake)
find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(defy_wired_central_test)

# The cable central with battery, control, lighting and the version report.
target_sources(app PRIVATE src/main.c src/fake_spi.c
                           ${DEFY_ROOT}/drivers/split/defy_wired_protocol.c
                           ${DEFY_ROOT}/battery/defy_battery_protocol.c
                           ${DEFY_ROOT}/battery/defy_half_version_protocol.c
                           ${DEFY_ROOT}/lighting/defy_feedback_protocol.c
                           ${DEFY_ROOT}/lighting/defy_lighting_protocol.c)
target_include_directories(app PRIVATE ${DEFY_ZMK_APP}/include)
target_compile_definitions(app PRIVATE
  CONFIG_DEFY_BATTERY=1
  CONFIG_DEFY_CONTROL=1
  CONFIG_DEFY_LIGHTING=1
  CONFIG_DEFY_HALF_VERSION=1
  CONFIG_DEFY_KEY_TRACE=1
  CONFIG_ZMK_LOG_LEVEL=3
  CONFIG_DEFY_WIRED_CENTRAL_STACK_SIZE=4096
  CONFIG_DEFY_WIRED_CENTRAL_THREAD_PRIORITY=5
  CONFIG_DEFY_WIRED_CENTRAL_TIMEOUT_MS=250)
