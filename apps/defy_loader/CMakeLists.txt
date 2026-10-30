# SPDX-License-Identifier: MIT
#
# The Neuron's UF2 loader, linked at 0x1000 in front of ZMK. Build it for
# defy_neuron/nrf52833; this module is its board root.

cmake_minimum_required(VERSION 3.20.0)

get_filename_component(module ${CMAKE_CURRENT_LIST_DIR}/../.. ABSOLUTE)
list(APPEND BOARD_ROOT ${module})
list(APPEND DTS_ROOT ${module})

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(defy_loader)

# src/start.c replaces the soc_reset_hook binding in Nordic's platform_init.ld,
# and Zephyr's Cortex-M reset.S calls the hook before SystemInit(). Another
# version of either file stops the build until start.c is reviewed against it.
include(${module}/cmake/defy_upstream.cmake)
defy_upstream_pin(${ZEPHYR_BASE}/soc/nordic/common/platform_init.ld
                  2e75270bf5d0bfbcefe237fb653c8c4a975bf787b9f60406268f4a7f0cc7279c
                  "the loader's src/start.c")
defy_upstream_pin(${ZEPHYR_BASE}/arch/arm/core/cortex_m/reset.S
                  3245028db3c46e9ed273d4e981f1776d7d1741a02d1fbda7d18617a81e82e74c
                  "the loader's src/start.c")

target_include_directories(app PRIVATE src ${module}/include)
target_sources(app PRIVATE src/start.c src/boot_mode.c src/main.c src/disk.c src/uf2.c
                           src/fat.c src/led.c)
