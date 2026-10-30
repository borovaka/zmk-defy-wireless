# SPDX-License-Identifier: MIT

cmake_minimum_required(VERSION 3.20.0)
include(${CMAKE_CURRENT_SOURCE_DIR}/../common/defy_test.cmake)
find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(defy_ble_scan_test)

set(central ${DEFY_ZMK_APP}/src/split/bluetooth/central.c)
set(patched ${PROJECT_BINARY_DIR}/split_central.c)
set(extract ${CMAKE_CURRENT_SOURCE_DIR}/extract_source.py)
include(${DEFY_ROOT}/drivers/split/zmk_split_central.cmake)
defy_split_central_source(${central} ${patched})
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS ${extract})
execute_process(
  COMMAND ${PYTHON_EXECUTABLE} ${extract} ${patched} ${PROJECT_BINARY_DIR}/scan_fixture.inc
  RESULT_VARIABLE extract_result ERROR_VARIABLE extract_error)
if(NOT extract_result EQUAL 0)
  message(FATAL_ERROR "Cannot prepare scan recovery fixture: ${extract_error}")
endif()
target_sources(app PRIVATE src/main.c src/host.c)
target_include_directories(app PRIVATE ${PROJECT_BINARY_DIR} ${DEFY_ZMK_APP}/include)
target_compile_definitions(app PRIVATE
  CONFIG_ZMK_SPLIT_BLE_CENTRAL_PERIPHERALS=2
  CONFIG_ZMK_SPLIT_BLE_PREF_INT=6
  CONFIG_ZMK_SPLIT_BLE_PREF_LATENCY=0
  CONFIG_ZMK_SPLIT_BLE_PREF_TIMEOUT=100
  CONFIG_ZMK_BLE_INIT_PRIORITY=50
  CONFIG_ZMK_BLE_THREAD_PRIORITY=0)
