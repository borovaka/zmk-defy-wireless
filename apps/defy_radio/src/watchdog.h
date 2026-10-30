/*
 * The radio's watchdogs: watchdog.c.
 *
 * SPDX-License-Identifier: MIT
 */

#ifndef DEFY_RADIO_WATCHDOG_H
#define DEFY_RADIO_WATCHDOG_H

/* Reloads every enabled channel of the hardware watchdog, if it runs. Safe before Zephyr starts. */
void defy_radio_watchdog_feed(void);

/* The link thread's progress, for the software watchdog. */
void defy_radio_watchdog_heartbeat(void);

#endif
