# SPDX-License-Identifier: MIT
#
# What the driver suites share. Every suite includes this file first, before
# find_package(Zephyr), which then finds the test bindings in dts/bindings here,
# and calls the functions below after project(). The suites reach the module's
# files through DEFY_ROOT. The fakes they share are here, which a suite
# reaches by adding ${DEFY_ROOT}/tests/drivers/common to its include
# directories, and the fake radio's sources to its target.
#
# The suites #include the module sources they test and define the CONFIG_
# values those sources read with target_compile_definitions(), because the
# Kconfig symbols behind them depend on ZMK and on the boards. The values that
# several suites share come from here.

# The module, and ZMK's app in the west workspace next to it.
get_filename_component(DEFY_ROOT ${CMAKE_CURRENT_LIST_DIR}/../../.. ABSOLUTE)
get_filename_component(DEFY_ZMK_APP ${DEFY_ROOT}/../zmk-workspace/zmk/app ABSOLUTE)

list(APPEND DTS_ROOT ${CMAKE_CURRENT_LIST_DIR})

# ZMK's HID report settings: HKRO with six keys or NKRO, and the full consumer
# usages, six at a time.
function(defy_test_zmk_hid keyboard)
  if(keyboard STREQUAL "HKRO")
    target_compile_definitions(app PRIVATE CONFIG_ZMK_HID_REPORT_TYPE_HKRO=1
                                           CONFIG_ZMK_HID_KEYBOARD_REPORT_SIZE=6)
  elseif(keyboard STREQUAL "NKRO")
    target_compile_definitions(app PRIVATE CONFIG_ZMK_HID_REPORT_TYPE_NKRO=1)
  else()
    message(FATAL_ERROR "defy_test_zmk_hid: HKRO or NKRO, not ${keyboard}")
  endif()
  target_compile_definitions(app PRIVATE CONFIG_ZMK_HID_CONSUMER_REPORT_USAGES_FULL=1
                                         CONFIG_ZMK_HID_CONSUMER_REPORT_SIZE=6)
endfunction()

# ZMK's event manager, without its log, and the events named, from its src/events/.
function(defy_test_zmk_events)
  list(TRANSFORM ARGN PREPEND ${DEFY_ZMK_APP}/src/events/ OUTPUT_VARIABLE events)
  target_sources(app PRIVATE ${DEFY_ZMK_APP}/src/event_manager.c ${events})
  target_include_directories(app PRIVATE ${DEFY_ZMK_APP}/include)
  target_compile_definitions(app PRIVATE CONFIG_ZMK_LOG_LEVEL=0)
  zephyr_linker_sources(RODATA ${DEFY_ZMK_APP}/include/linker/zmk-events.ld)
endfunction()
