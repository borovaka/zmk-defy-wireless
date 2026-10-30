/*
 * Confirms ZMK's start to the UF2 loader (docs/uf2-loader.md). The loader
 * counts ZMK's starts in GPREGRET and shows its drive instead of a start
 * after DEFY_LOADER_START_LIMIT in a row without a confirmation. This clears
 * the count once ZMK has run DEFY_LOADER_CONFIRM_MS from its application
 * init, and defy_neuron_bootloader.c clears it before an entry into
 * n2_bootloader. Any other value stays, such as &defy_drive's request.
 *
 * SPDX-License-Identifier: MIT
 */

#include <zephyr/init.h>
#include <zephyr/kernel.h>
#include <hal/nrf_power.h>
#include <dygma/defy_loader.h>

/* Keeps the read and the write of GPREGRET together. */
static struct k_spinlock gpregret_lock;

void defy_loader_confirm(void) {
    k_spinlock_key_t key = k_spin_lock(&gpregret_lock);
    if (defy_loader_starts(nrf_power_gpregret_get(NRF_POWER, DEFY_LOADER_GPREGRET)) != 0) {
        nrf_power_gpregret_set(NRF_POWER, DEFY_LOADER_GPREGRET, 0);
    }
    k_spin_unlock(&gpregret_lock, key);
}

static void confirm(struct k_work *work) {
    ARG_UNUSED(work);
    defy_loader_confirm();
}

static K_WORK_DELAYABLE_DEFINE(confirm_work, confirm);

static int confirm_init(void) {
    k_work_schedule(&confirm_work, K_MSEC(DEFY_LOADER_CONFIRM_MS));
    return 0;
}

SYS_INIT(confirm_init, APPLICATION, CONFIG_APPLICATION_INIT_PRIORITY);
