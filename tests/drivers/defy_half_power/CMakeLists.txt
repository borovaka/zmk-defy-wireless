# SPDX-License-Identifier: MIT

cmake_minimum_required(VERSION 3.20.0)
include(${CMAKE_CURRENT_SOURCE_DIR}/../common/defy_test.cmake)
find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(defy_half_power_test)

target_sources(app PRIVATE src/main.c)
target_include_directories(app BEFORE PRIVATE ${DEFY_ROOT}/tests/fakes/rp2040)
target_compile_definitions(app PRIVATE CONFIG_DEFY_HALF_SLEEP=1)
zephyr_link_libraries(-Wl,--wrap=sys_reboot)
