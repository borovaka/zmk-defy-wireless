# SPDX-License-Identifier: MIT

cmake_minimum_required(VERSION 3.20.0)
include(${CMAKE_CURRENT_SOURCE_DIR}/../common/defy_test.cmake)
# app.overlay's behavior nodes and keymap take their bindings from ZMK.
list(APPEND DTS_ROOT ${DEFY_ZMK_APP})
find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(defy_lighting_test)

# Listeners run in link order: consumer.c after the lighting, which main.c includes.
target_sources(app PRIVATE src/main.c src/consumer.c
                           ${DEFY_ROOT}/lighting/defy_lighting_protocol.c)
target_include_directories(app PRIVATE ${DEFY_ROOT}/tests/drivers/common)
# The looks and layer colours come from app.overlay.
target_compile_definitions(app PRIVATE CONFIG_DEFY_HALF_LEDS=1)
defy_test_zmk_events(position_state_changed.c layer_state_changed.c)
