/*
 * A fatal error restarts the Neuron, where Zephyr's default handler would
 * halt it. The UF2 loader counts the restart as an unconfirmed start
 * (defy_loader_confirm.c), so a ZMK that fails within DEFY_LOADER_CONFIRM_MS
 * of three starts in a row ends in the loader's drive (docs/uf2-loader.md).
 *
 * SPDX-License-Identifier: MIT
 */

#include <zephyr/fatal.h>
#include <zephyr/logging/log_ctrl.h>
#include <zephyr/sys/reboot.h>

void k_sys_fatal_error_handler(unsigned int reason, const struct arch_esf *esf) {
    ARG_UNUSED(reason);
    ARG_UNUSED(esf);
    /* Flushes the deferred log, which holds the error that z_fatal_error() logged. */
    LOG_PANIC();
    sys_reboot(SYS_REBOOT_COLD);
}
