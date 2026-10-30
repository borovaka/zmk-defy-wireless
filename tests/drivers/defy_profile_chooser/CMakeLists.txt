# SPDX-License-Identifier: MIT

cmake_minimum_required(VERSION 3.20.0)
include(${CMAKE_CURRENT_SOURCE_DIR}/../common/defy_test.cmake)
# The chooser's behavior node in app.overlay takes its binding's base from ZMK.
list(APPEND DTS_ROOT ${DEFY_ZMK_APP})
find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(defy_profile_chooser_test)

# Listeners run in link order: keymap.c after the lighting, which main.c includes.
target_sources(app PRIVATE src/main.c src/keymap.c ${DEFY_ROOT}/lighting/defy_feedback_protocol.c
                           ${DEFY_ROOT}/drivers/split/defy_wired_protocol.c)
target_include_directories(app PRIVATE ${DEFY_ROOT}/tests/drivers/common)
target_compile_definitions(app PRIVATE CONFIG_DEFY_NEURON_DOCK=1 CONFIG_BT_MAX_PAIRED=10)
defy_test_zmk_hid(NKRO)
defy_test_zmk_events(position_state_changed.c layer_state_changed.c)
