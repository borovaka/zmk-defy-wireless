# SPDX-License-Identifier: MIT

cmake_minimum_required(VERSION 3.20.0)
include(${CMAKE_CURRENT_SOURCE_DIR}/../common/defy_test.cmake)
find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(defy_neuron_dock_test)

target_sources(app PRIVATE src/main.c ${DEFY_ROOT}/bootloader/defy_update_guard.c)
target_include_directories(app PRIVATE ${DEFY_ZMK_APP}/include)
target_compile_definitions(app PRIVATE CONFIG_DEFY_HALF_UPDATER=1)
defy_test_zmk_hid(HKRO)
