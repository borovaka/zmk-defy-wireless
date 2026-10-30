# SPDX-License-Identifier: MIT

cmake_minimum_required(VERSION 3.20.0)
include(${CMAKE_CURRENT_SOURCE_DIR}/../common/defy_test.cmake)
find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(defy_radio_battery_test)

target_sources(app PRIVATE src/main.c ${DEFY_ROOT}/battery/defy_battery_protocol.c
                           ${DEFY_ROOT}/apps/defy_radio/src/battery_filter.c)
target_include_directories(app PRIVATE include)
