/*
 * A fatal error reboots the radio, where Zephyr's default handler would halt
 * it. It starts again through rf_bootloader, and the RP2040's driver takes
 * the new ready event for a restart of the controller.
 *
 * SPDX-License-Identifier: MIT
 */

#include <zephyr/fatal.h>
#include <zephyr/sys/reboot.h>

void k_sys_fatal_error_handler(unsigned int reason, const struct arch_esf *esf) {
    ARG_UNUSED(reason);
    ARG_UNUSED(esf);
    sys_reboot(SYS_REBOOT_COLD);
}
