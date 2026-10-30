/*
 * The radio's estimate of the battery level from comparator samples:
 * battery_filter.c.
 *
 * SPDX-License-Identifier: MIT
 */

#ifndef DEFY_RADIO_BATTERY_FILTER_H
#define DEFY_RADIO_BATTERY_FILTER_H

#include <dygma/defy_battery.h>

#define DEFY_BATTERY_FILTER_SAMPLES 60u
/* An estimate needs at least this many samples over at least this long. */
#define DEFY_BATTERY_FILTER_MIN_SAMPLES 5u
#define DEFY_BATTERY_FILTER_MIN_SPAN_MS 4000
#define DEFY_BATTERY_FILTER_INTERVAL_MS 1000
#define DEFY_BATTERY_FILTER_SETTLED_INTERVAL_MS 10000
#define DEFY_BATTERY_FILTER_CHARGER_INTERVAL_MS 5000
/* A longer gap between discharge samples restarts the history. */
#define DEFY_BATTERY_FILTER_GAP_MS (2 * DEFY_BATTERY_FILTER_SETTLED_INTERVAL_MS)
/* Consecutive unusable samples that clear the history. */
#define DEFY_BATTERY_FILTER_INVALID_LIMIT 3u

struct defy_battery_filter {
    uint16_t millivolts[DEFY_BATTERY_FILTER_SAMPLES];
    uint8_t levels[DEFY_BATTERY_FILTER_SAMPLES];
    uint8_t count;
    uint8_t next;
    uint8_t level;
    uint8_t invalid;
    uint16_t estimate_mv;
    bool valid;
    bool charger_active;
    int64_t started_at;
    int64_t sampled_at;
};

void defy_battery_filter_apply(struct defy_battery_filter *filter,
                               struct defy_battery_sample *sample, int64_t now);
uint32_t defy_battery_filter_delay(const struct defy_battery_filter *filter);

#endif
