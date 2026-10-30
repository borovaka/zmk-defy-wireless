/*
 * I2C diagnostics for the battery samples: battery_trace.c.
 *
 * SPDX-License-Identifier: MIT
 */

#ifndef DEFY_RADIO_BATTERY_TRACE_H
#define DEFY_RADIO_BATTERY_TRACE_H

#include <dygma/defy_battery.h>

void defy_battery_trace_begin(void);
void defy_battery_trace_error(struct defy_battery_sample *sample, uint8_t reg, int error,
                              uint32_t elapsed_us);
bool defy_battery_trace_tx_address_nack(const struct defy_battery_sample *sample);
void defy_battery_trace_charger(struct defy_battery_sample *sample, uint8_t reg, int error,
                               uint8_t value, uint32_t elapsed_us);

#endif
