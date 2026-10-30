# SPDX-License-Identifier: MIT

cmake_minimum_required(VERSION 3.20.0)

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(defy_radio)

get_filename_component(module ${CMAKE_CURRENT_LIST_DIR}/../.. ABSOLUTE)
include(${module}/cmake/defy_upstream.cmake)
# src/startup.c replaces the soc_reset_hook binding, as the UF2 loader's start.c does.
defy_upstream_pin(${ZEPHYR_BASE}/soc/nordic/common/platform_init.ld
                  2e75270bf5d0bfbcefe237fb653c8c4a975bf787b9f60406268f4a7f0cc7279c
                  "the radio's src/startup.c")
defy_upstream_pin(${ZEPHYR_BASE}/arch/arm/core/cortex_m/reset.S
                  3245028db3c46e9ed273d4e981f1776d7d1741a02d1fbda7d18617a81e82e74c
                  "the radio's src/startup.c")

target_include_directories(app PRIVATE src)
target_sources(app PRIVATE src/main.c src/link.c src/power_off.c src/watchdog.c src/fatal.c src/boot.c src/startup.c)
target_sources(app PRIVATE src/battery.c src/battery_hci.c src/battery_trace.c
  src/battery_comp.c src/battery_filter.c src/charger.c ../../battery/defy_battery_protocol.c)
zephyr_link_libraries(-Wl,--wrap=nrfx_twim_init)
defy_upstream_uses(${ZEPHYR_BASE}/drivers/i2c/i2c_nrfx_twim_common.c
                   "the I2C trace's wrap in src/battery_trace.c" "nrfx_twim_init(")
