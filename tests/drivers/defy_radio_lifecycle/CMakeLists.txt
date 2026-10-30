# SPDX-License-Identifier: MIT

cmake_minimum_required(VERSION 3.20.0)
include(${CMAKE_CURRENT_SOURCE_DIR}/../common/defy_test.cmake)
find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(defy_radio_lifecycle_test)

target_sources(app PRIVATE src/lifecycle.c ${DEFY_ROOT}/apps/defy_radio/src/link.c)
target_include_directories(app PRIVATE include ${DEFY_ROOT}/apps/defy_radio/src)
