/*
 * Build-time checks that a half's image fits Dygma's factory flash layout:
 * ks_bootloader at 0, its seal at 0x5000, ZMK from 0x6100, and ZMK's
 * settings from 0x1C0000 to the end of the 2 MiB flash. ks_bootloader starts
 * the image, so no second-stage boot loader is built.
 *
 * SPDX-License-Identifier: MIT
 */

#include <zephyr/devicetree.h>
#include <zephyr/sys/util.h>

#define CODE DT_NODELABEL(code_partition)
#define SETTINGS DT_NODELABEL(storage_partition)

/* The handoff is on for every image linked at 0x6100, and only for those. */
BUILD_ASSERT(IS_ENABLED(CONFIG_DEFY_RP2040_FACTORY_HANDOFF),
             "Defy images link at 0x6100, with the factory watchdog handoff");
BUILD_ASSERT(!IS_ENABLED(CONFIG_ZMK_DBL_TAP_BOOTLOADER), "Defy must start without a double-tap delay");
BUILD_ASSERT(!IS_ENABLED(CONFIG_BUILD_OUTPUT_UF2), "Package Defy UF2 images with package_factory.py");
BUILD_ASSERT(CONFIG_ROM_START_OFFSET == 0, "Defy factory handoff requires the vectors first");
BUILD_ASSERT(DT_REG_ADDR(CODE) == 0x6100 && DT_REG_ADDR(SETTINGS) == 0x1c0000 &&
                 DT_REG_ADDR(CODE) + DT_REG_SIZE(CODE) == DT_REG_ADDR(SETTINGS) &&
                 DT_REG_ADDR(SETTINGS) + DT_REG_SIZE(SETTINGS) <= 0x200000,
             "Defy's code runs from 0x6100 to its settings at 0x1C0000, inside the 2 MiB flash");
