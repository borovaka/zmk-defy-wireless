/*
 * The battery voltage from the comparator: battery_comp.c.
 *
 * SPDX-License-Identifier: MIT
 */

#ifndef DEFY_RADIO_BATTERY_COMP_H
#define DEFY_RADIO_BATTERY_COMP_H

#include <stdint.h>

#include <dygma/defy_battery.h>

struct defy_battery_comp_reading {
    uint8_t threshold;
    /* DEFY_BATTERY_ANALOG_REFERENCE_2V4 or DEFY_BATTERY_ANALOG_REFERENCE_VDD. */
    uint8_t reference;
};

int defy_radio_battery_comp_read(struct defy_battery_comp_reading *out);

#endif
