# SPDX-License-Identifier: MIT

cmake_minimum_required(VERSION 3.20.0)
include(${CMAKE_CURRENT_SOURCE_DIR}/../common/defy_test.cmake)
find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(defy_battery_central_test)

target_sources(app PRIVATE src/main.c ${DEFY_ROOT}/battery/defy_battery_protocol.c
                           ${DEFY_ROOT}/lighting/defy_lighting_protocol.c
                           ${DEFY_ROOT}/lighting/defy_feedback_protocol.c
                           ${DEFY_ROOT}/drivers/split/defy_wired_protocol.c
                           ${ZEPHYR_BASE}/subsys/bluetooth/host/uuid.c)
target_include_directories(app PRIVATE ${DEFY_ROOT}/tests/drivers/common)
target_compile_definitions(app PRIVATE CONFIG_BT_MAX_CONN=6 CONFIG_DEFY_BATTERY=1
                                      CONFIG_DEFY_CONTROL=1 CONFIG_DEFY_LIGHTING=1
                                      CONFIG_DEFY_HYBRID=1 CONFIG_DEFY_BATTERY_LOG_LEVEL=3)
