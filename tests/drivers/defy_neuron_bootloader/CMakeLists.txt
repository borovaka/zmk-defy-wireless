# SPDX-License-Identifier: MIT

cmake_minimum_required(VERSION 3.20.0)
include(${CMAKE_CURRENT_SOURCE_DIR}/../common/defy_test.cmake)
find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(defy_neuron_bootloader_test)

target_sources(app PRIVATE src/main.c ${DEFY_ROOT}/bootloader/defy_update_guard.c
                           ${DEFY_ROOT}/updater/defy_updater.c)
target_compile_definitions(app PRIVATE CONFIG_SOC_NRF52833=1 CONFIG_ZMK_LOG_LEVEL=3)
