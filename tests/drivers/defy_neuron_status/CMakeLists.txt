# SPDX-License-Identifier: MIT

cmake_minimum_required(VERSION 3.20.0)
include(${CMAKE_CURRENT_SOURCE_DIR}/../common/defy_test.cmake)
find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(defy_neuron_status_test)

target_sources(app PRIVATE src/main.c src/studio.c)
target_include_directories(app PRIVATE ${DEFY_ROOT}/tests/drivers/common)
target_compile_definitions(app PRIVATE CONFIG_DEFY_NEURON_STATUS_LED_BRIGHTNESS=10
                                       CONFIG_DEFY_HYBRID=1)
defy_test_zmk_events(endpoint_changed.c usb_conn_state_changed.c)
defy_test_zmk_hid(HKRO)
