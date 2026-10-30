# SPDX-License-Identifier: MIT

cmake_minimum_required(VERSION 3.20.0)
include(${CMAKE_CURRENT_SOURCE_DIR}/../common/defy_test.cmake)
find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(defy_split_pairing_test)

target_sources(app PRIVATE src/main.c ${ZEPHYR_BASE}/subsys/bluetooth/common/addr.c)
target_include_directories(app PRIVATE ${DEFY_ZMK_APP}/include)
target_compile_definitions(app PRIVATE
  CONFIG_ZMK_LOG_LEVEL=0
  CONFIG_ZMK_SPLIT_BLE=1
  CONFIG_ZMK_SPLIT_ROLE_CENTRAL=1
  CONFIG_ZMK_SPLIT_BLE_CENTRAL_PERIPHERALS=2
  CONFIG_BT_MAX_PAIRED=12)
