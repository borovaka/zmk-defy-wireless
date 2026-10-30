/*
 * A half's dormant sleep, soc/rp2040/poweroff.c: preparing the key matrix to
 * wake it, entering DORMANT, and whether this start came from a key wake.
 * power/defy_half_sleep.c decides when to sleep.
 *
 * SPDX-License-Identifier: MIT
 */

#ifndef DYGMA_DEFY_HALF_POWER_H
#define DYGMA_DEFY_HALF_POWER_H

#include <stdbool.h>
#include <zephyr/toolchain.h>

/* Stops the matrix interrupt so that a key shows only as pending: -EBUSY
 * unless the matrix waits for a key with every line low, -EALREADY if
 * prepared. System work queue only. */
int defy_half_power_prepare(void);
/* Whether a key went down since prepare(), or prepare() has not run. */
bool defy_half_power_pending(void);
/* Gives the matrix its interrupt back: kscan_enable_callback()'s result, or
 * -EALREADY if not prepared. System work queue only. */
int defy_half_power_cancel(void);
/* DORMANT until a key, then a watchdog reset; resets at once if a key is
 * pending. */
FUNC_NORETURN void defy_half_power_enter(void);

#if defined(CONFIG_DEFY_HALF_SLEEP)
/* Whether this start came from a key wake out of DORMANT. */
bool defy_half_power_woke_from_key(void);
#else
static inline bool defy_half_power_woke_from_key(void) { return false; }
#endif

#endif
