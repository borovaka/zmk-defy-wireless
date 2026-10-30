/* SPDX-License-Identifier: MIT */

#include <zephyr/toolchain.h>
#include <hardware/address_mapped.h>
#include <hardware/structs/watchdog.h>

#ifdef CONFIG_DEFY_FATAL_RECOVERY
#include <dygma/defy_fatal_recovery.h>
#endif

/* Dygma's bootloader leaves its watchdog running. */
FUNC_NO_STACK_PROTECTOR void soc_reset_hook(void) {
    hw_clear_bits(&watchdog_hw->ctrl, WATCHDOG_CTRL_ENABLE_BITS);
#ifdef CONFIG_DEFY_FATAL_RECOVERY
    defy_fatal_recovery_boot();
#endif
}
