/*
 * The battery level estimate from comparator samples, which battery.c applies
 * to every sample before publishing it. docs/battery.md describes it under
 * "Persistent analogue estimate".
 *
 * SPDX-License-Identifier: MIT
 */

#include <string.h>

#include "battery_filter.h"

static uint8_t filter_reading(struct defy_battery_filter *filter,
                              const struct defy_battery_reading *reading, int64_t now) {
    if (filter->count &&
        (now < filter->sampled_at || now - filter->sampled_at > DEFY_BATTERY_FILTER_GAP_MS)) {
        memset(filter, 0, sizeof(*filter));
    }
    if (filter->count && now - filter->sampled_at < DEFY_BATTERY_FILTER_INTERVAL_MS) {
        return filter->valid ? filter->level : DEFY_BATTERY_LEVEL_UNAVAILABLE;
    }
    if (!filter->count) {
        filter->started_at = now;
    }
    filter->millivolts[filter->next] = reading->millivolts;
    filter->levels[filter->next] = reading->level;
    filter->next = (filter->next + 1u) % DEFY_BATTERY_FILTER_SAMPLES;
    if (filter->count < DEFY_BATTERY_FILTER_SAMPLES) {
        filter->count++;
    }
    filter->sampled_at = now;

    uint8_t best = 0;
    uint8_t votes = 0;
    uint8_t held = 0;
    for (uint8_t i = 0; i < filter->count; i++) {
        uint8_t count = 0;
        for (uint8_t j = 0; j < filter->count; j++) {
            count += filter->millivolts[j] == filter->millivolts[i];
        }
        if (filter->valid && filter->millivolts[i] == filter->estimate_mv) {
            held = count;
        }
        if (count > votes ||
            (count == votes && filter->millivolts[i] < filter->millivolts[best])) {
            best = i;
            votes = count;
        }
    }
    /*
     * The most frequent voltage wins, ties going to the lower one. A higher
     * voltage must also outnumber the held estimate two to one, so a reading
     * that dithers between adjacent thresholds settles on the lower one
     * instead of alternating.
     */
    if (filter->count >= DEFY_BATTERY_FILTER_MIN_SAMPLES &&
        now - filter->started_at >= DEFY_BATTERY_FILTER_MIN_SPAN_MS &&
        (!filter->valid || filter->millivolts[best] < filter->estimate_mv ||
         votes > 2u * held)) {
        filter->level = filter->levels[best];
        filter->estimate_mv = filter->millivolts[best];
        filter->valid = true;
    }
    return filter->valid ? filter->level : DEFY_BATTERY_LEVEL_UNAVAILABLE;
}

void defy_battery_filter_apply(struct defy_battery_filter *filter,
                               struct defy_battery_sample *sample, int64_t now) {
    struct defy_battery_reading reading;
    struct defy_battery_charger charger;
    uint8_t level = DEFY_BATTERY_LEVEL_UNAVAILABLE;
    bool charger_known =
        sample->words[DEFY_BATTERY_ANALOG_FORMAT] == DEFY_BATTERY_ANALOG_CHARGER_TAG &&
        !defy_battery_sample_charger(sample, &charger) && charger.status_valid;
    bool charger_idle = charger_known && !charger.boost &&
        charger.state == DEFY_BATTERY_CHARGE_READY &&
        (charger.fault == 0 || charger.fault == DEFY_BATTERY_CHARGER_FAULT_SLEEP);
    if (charger_idle && !defy_battery_sample_reading(sample, &reading)) {
        filter->invalid = 0;
        filter->charger_active = false;
        level = filter_reading(filter, &reading, now - sample->age_ms);
    } else if (charger_known && !charger_idle) {
        /* Charging, charged, boost or a fault: a real change, not a bad sample. */
        memset(filter, 0, sizeof(*filter));
        filter->charger_active = true;
    } else if (filter->invalid < DEFY_BATTERY_FILTER_INVALID_LIMIT &&
               ++filter->invalid == DEFY_BATTERY_FILTER_INVALID_LIMIT) {
        memset(filter, 0, sizeof(*filter));
        filter->invalid = DEFY_BATTERY_FILTER_INVALID_LIMIT;
    }
    /*
     * battery.c lays out every analog sample as DEFY_BATTERY_ANALOG_CHARGER_TAG:
     * the raw comparator reading and the charger registers, which the decoders
     * in battery/defy_battery_protocol.c read. Here it becomes
     * DEFY_BATTERY_ANALOG_FILTERED_TAG, with the estimate in the threshold
     * word's high byte. Only the filtered form leaves the radio.
     */
    if ((sample->status == DEFY_BATTERY_SAMPLE_ANALOG_OK ||
         sample->status == DEFY_BATTERY_SAMPLE_ANALOG_ERROR) &&
        sample->words[DEFY_BATTERY_ANALOG_FORMAT] == DEFY_BATTERY_ANALOG_CHARGER_TAG) {
        sample->words[DEFY_BATTERY_ANALOG_FORMAT] = DEFY_BATTERY_ANALOG_FILTERED_TAG;
        if (sample->status == DEFY_BATTERY_SAMPLE_ANALOG_OK) {
            sample->words[DEFY_BATTERY_ANALOG_THRESHOLD] =
                (sample->words[DEFY_BATTERY_ANALOG_THRESHOLD] & 0xffu) | (uint16_t)level << 8;
        }
    }
}

/*
 * While the charger is idle: 1 Hz while the history fills or a bad sample
 * awaits its retry, otherwise the settled interval.
 */
uint32_t defy_battery_filter_delay(const struct defy_battery_filter *filter) {
    if (filter->charger_active) {
        return DEFY_BATTERY_FILTER_CHARGER_INTERVAL_MS;
    }
    if ((filter->invalid > 0 && filter->invalid < DEFY_BATTERY_FILTER_INVALID_LIMIT) ||
        (filter->count > 0 && !filter->valid)) {
        return DEFY_BATTERY_FILTER_INTERVAL_MS;
    }
    return DEFY_BATTERY_FILTER_SETTLED_INTERVAL_MS;
}
