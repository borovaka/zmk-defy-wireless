# SPDX-License-Identifier: MIT

cmake_minimum_required(VERSION 3.20.0)
include(${CMAKE_CURRENT_SOURCE_DIR}/../common/defy_test.cmake)
find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(defy_usb_report_recovery_test)

target_sources(app PRIVATE src/main.c src/callers.c)
target_include_directories(app PRIVATE ${DEFY_ZMK_APP}/include ${DEFY_ROOT}/tests/drivers/common)
# ZMK's mouse report, as the template's defy_neuron.conf turns on CONFIG_ZMK_POINTING.
target_compile_definitions(app PRIVATE CONFIG_DEFY_USB_REPORT_RECOVERY=1 CONFIG_ZMK_LOG_LEVEL=3
                                       CONFIG_ZMK_POINTING=1)
defy_test_zmk_hid(NKRO)
zephyr_link_libraries(
  -Wl,--wrap=zmk_usb_hid_send_keyboard_report
  -Wl,--wrap=zmk_usb_hid_send_consumer_report
  -Wl,--wrap=zmk_usb_hid_send_mouse_report
  -Wl,--wrap=usb_wakeup_request
  -Wl,--wrap=usb_enable)
