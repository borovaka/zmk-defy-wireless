# SPDX-License-Identifier: MIT

cmake_minimum_required(VERSION 3.20.0)
include(${CMAKE_CURRENT_SOURCE_DIR}/../common/defy_test.cmake)
set(DTC_OVERLAY_FILE ${DEFY_ROOT}/tests/drivers/common/radio.overlay)

# The radio firmware the driver installs: a small stand-in.
set(DEFY_RADIO_IMAGE_SOURCE ${CMAKE_CURRENT_SOURCE_DIR}/src/image.c)

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(defy_radio_install_test)

set(common ${DEFY_ROOT}/tests/drivers/common)
target_sources(app PRIVATE src/main.c ${common}/fake_radio.c ${common}/test_bus.c
                           ${DEFY_ROOT}/apps/defy_radio/src/link.c)
target_include_directories(app PRIVATE ${common} ${DEFY_ROOT}/apps/defy_radio/src
                                       ${ZEPHYR_BASE}/drivers/spi)
# The driver reports its start-up stages to the half's LEDs, as with
# CONFIG_DEFY_HALF_LEDS; src/main.c records them in their place.
zephyr_compile_definitions(CONFIG_DEFY_HALF_LEDS=1)
