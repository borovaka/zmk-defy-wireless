/*
 * The UF2 loader's register, GPREGRET, which the loader (apps/defy_loader/)
 * and ZMK on the Neuron share: ZMK's request for the drive, and the count of
 * ZMK's starts that it has not confirmed. GPREGRET keeps its value through
 * every reset but power-on and brownout. GPREGRET2 belongs to the half
 * updater. See docs/uf2-loader.md.
 *
 * The loader's start hook includes this header before Zephyr has set up RAM,
 * so it holds only constants and a function without state.
 *
 * SPDX-License-Identifier: MIT
 */

#ifndef DYGMA_DEFY_LOADER_H
#define DYGMA_DEFY_LOADER_H

#include <stdint.h>

/* GPREGRET's index for nrf_power_gpregret_get() and nrf_power_gpregret_set(). */
#define DEFY_LOADER_GPREGRET 0

/*
 * &defy_drive's request for the drive, ZMK's boot-mode magic for Adafruit's
 * nRF52 bootloader. bootloader/behavior_defy_drive.c checks it against ZMK's
 * CONFIG_ZMK_BOOTMODE_BOOTLOADER_MAGIC_VALUE.
 */
#define DEFY_LOADER_DRIVE_MAGIC 0x57u

/*
 * GPREGRET after n starts of ZMK in a row without a confirmation, n from 1
 * to DEFY_LOADER_START_LIMIT. The loader writes it before each jump to ZMK.
 * ZMK clears it once it has run DEFY_LOADER_CONFIRM_MS, and before an entry
 * into n2_bootloader.
 */
#define DEFY_LOADER_STARTS_BASE 0xc0u
#define DEFY_LOADER_STARTS(n) (DEFY_LOADER_STARTS_BASE + (n))

/* After this many unconfirmed starts the loader shows its drive instead of
 * starting ZMK again. */
#define DEFY_LOADER_START_LIMIT 3u

/* How long ZMK runs before it confirms its start. */
#define DEFY_LOADER_CONFIRM_MS 10000

/* The unconfirmed starts that a GPREGRET value records: 0 for any other value. */
static inline uint32_t defy_loader_starts(uint32_t gpregret) {
    return gpregret > DEFY_LOADER_STARTS_BASE &&
                   gpregret <= DEFY_LOADER_STARTS(DEFY_LOADER_START_LIMIT)
               ? gpregret - DEFY_LOADER_STARTS_BASE
               : 0;
}

/*
 * ZMK's side (bootloader/defy_loader_confirm.c, CONFIG_DEFY_LOADER_CONFIRM):
 * clears the count of unconfirmed starts, leaving any other value alone.
 */
void defy_loader_confirm(void);

#endif /* DYGMA_DEFY_LOADER_H */
