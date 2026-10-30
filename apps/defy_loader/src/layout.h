/*
 * The Neuron's flash and RAM as the loader uses them. The flash ranges come
 * from boards/dygma/defy_neuron/defy_neuron-partitions.dtsi, and main.c
 * checks at build time that the two agree. The files that hold no Zephyr code
 * (boot_mode.c, uf2.c, fat.c) take them from here, so that the host
 * suites in tests/host/ can build them. tests/scripts/test_loader_layout.py
 * compares the partitions with scripts/neuron_image.py.
 *
 * SPDX-License-Identifier: MIT
 */

#pragma once

/* ZMK's code_partition, the only range the loader writes. */
#define DEFY_LOADER_ZMK_START 0xd000u
#define DEFY_LOADER_ZMK_END 0x6c000u

/* The nRF52833's flash page, the unit of an erase. */
#define DEFY_LOADER_PAGE_SIZE 0x1000u

/* The nRF52833's RAM. */
#define DEFY_LOADER_RAM_START 0x20000000u
#define DEFY_LOADER_RAM_END 0x20020000u
