/*
 * Quiescing the SPI slave rf_bootloader leaves enabled: boot.c, called from
 * the reset hook in startup.c.
 *
 * SPDX-License-Identifier: MIT
 */

#ifndef DEFY_RADIO_BOOT_H
#define DEFY_RADIO_BOOT_H

#include <stdbool.h>

/* How long chip select has to go idle. */
#define DEFY_RADIO_BOOT_QUIESCE_TIMEOUT_US 20000u

/*
 * Waits for chip select to be idle, takes the SPIS semaphore and disables the
 * SPIS, calling keep_alive, which may be NULL, once a millisecond meanwhile.
 * True if the SPIS is disabled, false if it could not be disabled safely.
 */
bool defy_radio_boot_quiesce_spis(void (*keep_alive)(void));

#endif
