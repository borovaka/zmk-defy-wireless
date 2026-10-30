# SPDX-License-Identifier: MIT

cmake_minimum_required(VERSION 3.20.0)
include(${CMAKE_CURRENT_SOURCE_DIR}/../common/defy_test.cmake)
find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(defy_hybrid_central_test)

target_sources(app PRIVATE src/main.c ${DEFY_ROOT}/drivers/split/defy_wired_protocol.c
                           ${ZEPHYR_BASE}/subsys/bluetooth/host/uuid.c)
target_include_directories(app PRIVATE ${DEFY_ZMK_APP}/include ${DEFY_ROOT}/tests/drivers/common)
target_compile_definitions(app PRIVATE
  CONFIG_BT_SMP=1
  CONFIG_DEFY_BATTERY=1
  CONFIG_DEFY_CONTROL=1
  CONFIG_DEFY_KEY_TRACE=1
  CONFIG_DEFY_USB_REPORT_RECOVERY=1
  CONFIG_ZMK_LOG_LEVEL=3
  CONFIG_DEFY_HYBRID_QUEUE_SIZE=128
  CONFIG_DEFY_HYBRID_BLE_TIMEOUT_MS=1000
  CONFIG_DEFY_HYBRID_HANDOFF_GRACE_MS=200
  CONFIG_ZMK_SPLIT_BLE_CENTRAL_PERIPHERALS=2)
