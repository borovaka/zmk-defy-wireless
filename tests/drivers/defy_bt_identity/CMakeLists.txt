# SPDX-License-Identifier: MIT

cmake_minimum_required(VERSION 3.20.0)
include(${CMAKE_CURRENT_SOURCE_DIR}/../common/defy_test.cmake)
find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(defy_bt_identity_test)

target_sources(app PRIVATE src/main.c ${ZEPHYR_BASE}/subsys/bluetooth/common/addr.c)
target_compile_definitions(app PRIVATE CONFIG_BT_ID_MAX=2 CONFIG_DEFY_HALF_SLEEP=1)
