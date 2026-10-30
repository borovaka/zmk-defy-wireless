/*
 * The halves' wrap of bt_le_adv_start(), drivers/bluetooth/defy_bt_identity.c.
 * It saves the Bluetooth identity before advertising, and holds off new
 * advertising while a half shuts Bluetooth down to sleep.
 *
 * SPDX-License-Identifier: MIT
 */

#ifndef DYGMA_DEFY_BT_IDENTITY_H
#define DYGMA_DEFY_BT_IDENTITY_H

#include <stdbool.h>

#ifdef CONFIG_DEFY_HALF_SLEEP
/* Allows advertising, or refuses it while the half stops Bluetooth to sleep. */
void defy_bt_identity_set_advertising(bool allowed);
#endif

#endif
