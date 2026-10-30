# SPDX-License-Identifier: MIT

cmake_minimum_required(VERSION 3.20.0)
include(${CMAKE_CURRENT_SOURCE_DIR}/../common/defy_test.cmake)
set(DTC_OVERLAY_FILE ${DEFY_ROOT}/tests/drivers/common/radio.overlay)
find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(defy_hci_spi_test)

set(common ${DEFY_ROOT}/tests/drivers/common)
target_sources(app PRIVATE src/main.c ${common}/fake_radio.c ${common}/test_bus.c
                           ${DEFY_ROOT}/apps/defy_radio/src/link.c)
target_include_directories(app PRIVATE ${common} ${DEFY_ROOT}/apps/defy_radio/src
                                       ${ZEPHYR_BASE}/drivers/spi)
zephyr_link_libraries(-Wl,--wrap=sys_reboot)
