/*
 * Power off to System OFF: power_off.c.
 *
 * SPDX-License-Identifier: MIT
 */

#ifndef DEFY_RADIO_POWER_OFF_H
#define DEFY_RADIO_POWER_OFF_H

/*
 * Powers off to System OFF and does not return. Returns -EBUSY while chip
 * select is active or the SPIS holds its semaphore, for the caller to try
 * again, or a GPIO error.
 */
int defy_radio_power_off(void);

#endif
