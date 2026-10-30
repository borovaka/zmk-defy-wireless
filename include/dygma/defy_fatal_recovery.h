/*
 * Restarting a half after a fatal error, and holding it after repeated quick
 * faults: diagnostics/defy_fatal_recovery.c.
 *
 * SPDX-License-Identifier: MIT
 */

#ifndef DYGMA_DEFY_FATAL_RECOVERY_H
#define DYGMA_DEFY_FATAL_RECOVERY_H

#include <zephyr/toolchain.h>

/*
 * From the reset hook, before Zephyr starts: counts a fault that ended in a
 * watchdog reset, and after the third quick one waits in DORMANT for a key.
 */
void defy_fatal_recovery_boot(void);

/* From the fatal error handler: shows the fault's pattern, then the watchdog restarts the half. */
FUNC_NORETURN void defy_fatal_recover(unsigned int reason);

#endif
