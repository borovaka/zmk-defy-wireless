# SPDX-License-Identifier: MIT

cmake_minimum_required(VERSION 3.20.0)
include(${CMAKE_CURRENT_SOURCE_DIR}/../common/defy_test.cmake)
# ZMK's bindings, for the keymap in app.overlay.
list(APPEND DTS_ROOT ${DEFY_ZMK_APP})
find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(defy_updater_usb_test)

target_sources(app PRIVATE src/main.c ${DEFY_ROOT}/updater/defy_updater.c
                           ${DEFY_ROOT}/updater/defy_keymap_changes.c
                           ${DEFY_ROOT}/bootloader/defy_update_guard.c)
target_include_directories(app PRIVATE include ${DEFY_ZMK_APP}/include)
target_compile_definitions(app PRIVATE CONFIG_DEFY_NEURON_DOCK=1 CONFIG_DEFY_KEY_TRACE=1
                                       CONFIG_DEFY_HALF_VERSION=1
                                       CONFIG_ZMK_KEYMAP_SETTINGS_STORAGE=1 CONFIG_ZMK_STUDIO=1)
defy_test_zmk_hid(HKRO)
