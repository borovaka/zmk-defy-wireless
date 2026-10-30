/*
 * Battery support on the radio. battery.c samples the battery gauge, or the
 * comparator on a half without one, and the charger. battery_hci.c hands each
 * sample to the host as a vendor event (dygma/defy_battery_hci.h).
 *
 * SPDX-License-Identifier: MIT
 */

#ifndef DEFY_RADIO_BATTERY_H
#define DEFY_RADIO_BATTERY_H

#include <stdint.h>

#include <zephyr/net_buf.h>

#include <dygma/defy_battery.h>

/* battery.c: sampling, from the radio's start until it powers off. */
void defy_radio_battery_start(void);
void defy_radio_battery_stop(void);

/* battery.c: a burst of samples, the first now. -ESHUTDOWN before start or once stopping. */
int defy_radio_battery_refresh(void);

/* battery_hci.c: queues sample for the host. -ESHUTDOWN once the radio is powering off. */
int defy_radio_battery_publish(const struct defy_battery_sample *sample);

/* battery_hci.c: the host has read a message, which may free the buffer for a waiting sample. */
void defy_radio_battery_hci_sent(void);

/* battery_hci.c: the radio is powering off. A waiting sample is dropped, and no more taken. */
void defy_radio_battery_hci_power_off(void);

/*
 * battery_hci.c: if buf is the battery event, updates the age in message, the
 * copy of buf armed for the host.
 */
void defy_radio_battery_hci_arm(const struct net_buf *buf, uint8_t *message);

/* battery_hci.c: the refresh command. Asks battery.c for a sample now; returns the reply status. */
uint8_t defy_radio_battery_hci_refresh(void);

#endif
