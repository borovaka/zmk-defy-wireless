# SPDX-License-Identifier: MIT

cmake_minimum_required(VERSION 3.20.0)
include(${CMAKE_CURRENT_SOURCE_DIR}/../common/defy_test.cmake)
find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(defy_key_trace_test)

target_sources(app PRIVATE src/main.c)
target_include_directories(app PRIVATE ${DEFY_ZMK_APP}/include)
target_compile_definitions(app PRIVATE CONFIG_DEFY_KEY_TRACE=1)
