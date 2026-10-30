/* SPDX-License-Identifier: MIT */

#include "battery_filter.h"
#include <assert.h>
#include <errno.h>
#include <string.h>

static struct defy_battery_sample raw(uint8_t threshold, uint8_t state) {
    struct defy_battery_sample sample = {
        .sequence = 1, .status = DEFY_BATTERY_SAMPLE_ANALOG_OK,
        .words = {DEFY_BATTERY_ANALOG_CHARGER_TAG, threshold,
                  DEFY_BATTERY_ANALOG_REFERENCE_VDD, 0, 0x94 | (uint16_t)state << 8,
                  0, 0, 0x0300},
    };
    assert(defy_battery_analog_mv(threshold, sample.words[2], &sample.words[3]) == 0);
    return sample;
}

static int observe(struct defy_battery_filter *filter, uint8_t threshold,
                    uint8_t state, int64_t now) {
    struct defy_battery_sample sample = raw(threshold, state);
    defy_battery_filter_apply(filter, &sample, now);
    assert(sample.words[0] == DEFY_BATTERY_ANALOG_FILTERED_TAG);
    assert((sample.words[1] & 0xff) == threshold);
    struct defy_battery_reading reading;
    int err = defy_battery_sample_reading(&sample, &reading);
    assert(err == 0 || err == -EAGAIN);
    return err ? -1 : reading.level;
}

static void seed(struct defy_battery_filter *filter, uint8_t threshold) {
    memset(filter, 0, sizeof(*filter));
    for (int i = 0; i < 60; i++) {
        observe(filter, threshold, 0, i * 1000);
    }
}

static void test_startup_needs_real_spaced_samples(void) {
    struct defy_battery_filter filter = {0};
    for (int t = 0; t < 4000; t += 100) {
        assert(observe(&filter, 52, 0, t) == -1);
    }
    assert(filter.count == 4);
    assert(observe(&filter, 52, 0, 4000) == 70);
    assert(filter.count == 5);
}

static void test_recent_charging_voltage_does_not_seed_history(void) {
    struct defy_battery_filter filter = {0};
    for (int i = 0; i < 5; i++) {
        assert(observe(&filter, 55, 0x10, i * 1000) == -1);
        assert(defy_battery_filter_delay(&filter) == DEFY_BATTERY_FILTER_CHARGER_INTERVAL_MS);
    }
    assert(filter.count == 0);
    for (int i = 5; i < 7; i++) {
        assert(observe(&filter, 53, 0, i * 1000) == -1);
        assert(defy_battery_filter_delay(&filter) == DEFY_BATTERY_FILTER_INTERVAL_MS);
    }
    for (int i = 7; i < 9; i++) {
        assert(observe(&filter, 52, 0, i * 1000) == -1);
    }
    assert(observe(&filter, 52, 0, 9000) == 70);
    assert(filter.count == 5);
    assert(defy_battery_filter_delay(&filter) == DEFY_BATTERY_FILTER_SETTLED_INTERVAL_MS);
}

static void test_noise_and_refreshes_do_not_weight_or_restart_history(void) {
    struct defy_battery_filter filter;
    seed(&filter, 52);
    for (int t = 60000; t < 61000; t += 10) {
        assert(observe(&filter, 53, 0, t) == 70);
    }
    assert(observe(&filter, 52, 0, 61000) == 70);
    assert(filter.count == 60);
    for (int i = 62; i < 100; i++) {
        assert(observe(&filter, i % 5 ? 52 : 53, 0, i * 1000) == 70);
    }
}

static void test_sustained_change_replaces_old_history_in_both_directions(void) {
    const int64_t step = DEFY_BATTERY_FILTER_SETTLED_INTERVAL_MS;
    struct defy_battery_filter filter;
    seed(&filter, 53);
    for (int k = 1; k < 30; k++) {
        assert(observe(&filter, 52, 0, 59000 + k * step) == 100);
    }
    assert(observe(&filter, 52, 0, 59000 + 30 * step) == 70);
    assert(30 * step == 5 * 60 * 1000);
    seed(&filter, 52);
    for (int k = 1; k < 41; k++) {
        assert(observe(&filter, 53, 0, 59000 + k * step) == 70);
    }
    assert(observe(&filter, 53, 0, 59000 + 41 * step) == 100);
}

static struct defy_battery_sample glitch(int kind) {
    struct defy_battery_sample sample = raw(52, 0);
    if (kind == 0) { sample.status = DEFY_BATTERY_SAMPLE_ANALOG_ERROR; }
    if (kind == 1) { sample.words[4] = 0x94; sample.words[7] = 0x0100; }
    if (kind == 2) { sample.words[3] = 55; }
    if (kind == 3) { sample.age_ms = DEFY_BATTERY_MAX_AGE_MS; }
    if (kind == 4) { sample.words[4] = 0x95 ^ 4; }
    if (kind == 5) { sample.words[4] = 0; sample.words[5] = 0; sample.words[6] = 0;
                     sample.words[7] = 0; }
    return sample;
}

static void test_charger_transitions_discard_old_estimate_at_once(void) {
    const uint8_t states[] = {0x10, 0x20, 0x31, 0x08, 0x01};
    for (size_t i = 0; i < sizeof(states); i++) {
        struct defy_battery_filter filter;
        seed(&filter, 53);
        assert(observe(&filter, 55, states[i], 60000) == -1);
        assert(filter.count == 0 && !filter.valid && filter.charger_active);
        assert(defy_battery_filter_delay(&filter) == DEFY_BATTERY_FILTER_CHARGER_INTERVAL_MS);
        for (int t = 61; t < 65; t++) {
            assert(observe(&filter, 52, 2, t * 1000) == -1);
            assert(!filter.charger_active);
        }
        assert(observe(&filter, 52, 0, 65000) == 70);
    }
}

static void test_unusable_samples_clear_only_after_three_in_a_row(void) {
    for (int kind = 0; kind < 6; kind++) {
        struct defy_battery_filter filter;
        seed(&filter, 53);
        for (int i = 1; i < 3; i++) {
            struct defy_battery_sample sample = glitch(kind);
            defy_battery_filter_apply(&filter, &sample, 59000 + i * 1000);
            assert(filter.valid && filter.count == 60 && filter.invalid == i);
            assert(defy_battery_filter_delay(&filter) == DEFY_BATTERY_FILTER_INTERVAL_MS);
        }
        struct defy_battery_sample sample = glitch(kind);
        defy_battery_filter_apply(&filter, &sample, 62000);
        assert(!filter.valid && filter.count == 0);
        assert(defy_battery_filter_delay(&filter) == DEFY_BATTERY_FILTER_SETTLED_INTERVAL_MS);
        sample = glitch(kind);
        defy_battery_filter_apply(&filter, &sample, 63000);
        assert(defy_battery_filter_delay(&filter) == DEFY_BATTERY_FILTER_SETTLED_INTERVAL_MS);
        assert(observe(&filter, 52, 0, 93000) == -1);
        assert(filter.count == 1 && filter.invalid == 0);
        assert(defy_battery_filter_delay(&filter) == DEFY_BATTERY_FILTER_INTERVAL_MS);
    }
}

static void test_single_glitch_keeps_estimate(void) {
    struct defy_battery_filter filter;
    seed(&filter, 52);
    struct defy_battery_sample sample = glitch(0);
    defy_battery_filter_apply(&filter, &sample, 60000);
    struct defy_battery_reading reading;
    assert(defy_battery_sample_reading(&sample, &reading) == -ENODATA);
    assert(observe(&filter, 52, 0, 61000) == 70);
    sample = glitch(5);
    defy_battery_filter_apply(&filter, &sample, 62000);
    sample = glitch(1);
    defy_battery_filter_apply(&filter, &sample, 63000);
    assert(observe(&filter, 52, 0, 64000) == 70);
    assert(filter.invalid == 0);
    assert(defy_battery_filter_delay(&filter) == DEFY_BATTERY_FILTER_SETTLED_INTERVAL_MS);
}

static void test_dither_between_adjacent_thresholds_settles_low(void) {
    for (int first = 0; first < 2; first++) {
        struct defy_battery_filter filter = {0};
        int64_t now = 0;
        for (int i = 0; i < 200; i++) {
            uint8_t threshold = (i + first) % 2 ? 52 : 53;
            int level = observe(&filter, threshold, 0, now);
            if (i < 4) {
                assert(level == -1);
                assert(defy_battery_filter_delay(&filter) == DEFY_BATTERY_FILTER_INTERVAL_MS);
            } else if (i == 4) {
                assert(level == (first ? 70 : 100));
            } else {
                assert(level == 70);
                assert(defy_battery_filter_delay(&filter) ==
                       DEFY_BATTERY_FILTER_SETTLED_INTERVAL_MS);
            }
            now += i < 5 ? DEFY_BATTERY_FILTER_INTERVAL_MS
                         : DEFY_BATTERY_FILTER_SETTLED_INTERVAL_MS;
        }
        assert(filter.count == DEFY_BATTERY_FILTER_SAMPLES);
    }
}

static void test_gap_and_clock_reset_require_new_history(void) {
    struct defy_battery_filter filter;
    seed(&filter, 53);
    assert(observe(&filter, 53, 0, 59000 + DEFY_BATTERY_FILTER_SETTLED_INTERVAL_MS) == 100);
    assert(observe(&filter, 53, 0, 59000 + 2 * DEFY_BATTERY_FILTER_SETTLED_INTERVAL_MS) == 100);
    assert(filter.count == 60);
    assert(observe(&filter, 52, 0, 59000 + 4 * DEFY_BATTERY_FILTER_SETTLED_INTERVAL_MS + 1) ==
           -1);
    assert(filter.count == 1);
    seed(&filter, 53);
    assert(observe(&filter, 52, 0, 1000) == -1);
    assert(filter.count == 1);
}

static void test_gauge_and_error_payloads_are_preserved(void) {
    struct defy_battery_filter filter = {0};
    struct defy_battery_sample sample = {.sequence = 1, .words = {0x4031, 2}};
    struct defy_battery_sample saved = sample;
    defy_battery_filter_apply(&filter, &sample, 0);
    assert(memcmp(&saved, &sample, sizeof(sample)) == 0);
    sample = raw(52, 0x10);
    sample.status = DEFY_BATTERY_SAMPLE_ANALOG_ERROR;
    sample.words[1] = (uint16_t)-ETIMEDOUT;
    defy_battery_filter_apply(&filter, &sample, 1000);
    assert(sample.words[1] == (uint16_t)-ETIMEDOUT);
    struct defy_battery_charger charger;
    assert(defy_battery_sample_charger(&sample, &charger) == 0);
    assert(charger.state == DEFY_BATTERY_CHARGE_CHARGING);
}

int main(void) {
    test_startup_needs_real_spaced_samples();
    test_recent_charging_voltage_does_not_seed_history();
    test_noise_and_refreshes_do_not_weight_or_restart_history();
    test_sustained_change_replaces_old_history_in_both_directions();
    test_charger_transitions_discard_old_estimate_at_once();
    test_unusable_samples_clear_only_after_three_in_a_row();
    test_single_glitch_keeps_estimate();
    test_dither_between_adjacent_thresholds_settles_low();
    test_gap_and_clock_reset_require_new_history();
    test_gauge_and_error_payloads_are_preserved();
    return 0;
}
