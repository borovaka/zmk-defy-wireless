/*
 * Samples the battery gauge, or the comparator on a half without one, and
 * the charger, and publishes the samples for the host.
 *
 * SPDX-License-Identifier: MIT
 */

#include <errno.h>
#include <string.h>

#include <zephyr/device.h>
#include <zephyr/drivers/i2c.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/atomic.h>
#include <zephyr/sys/byteorder.h>

#include "battery.h"
#include "battery_comp.h"
#include "battery_filter.h"
#include "battery_trace.h"
#include "charger.h"

#define BATTERY_ADDRESS 0x36
#define BATTERY_START_DELAY_MS 1000
#define BATTERY_SAMPLE_INTERVAL_MS 30000
#define BATTERY_POWERED_INTERVAL_MS 5000
#define BATTERY_BURST_INTERVAL_MS 500
#define BATTERY_BURST_SAMPLES 5
/*
 * Republish an unchanged sample before the last published one gets older than
 * this: about once a minute when settled. Copies on the halves and the Neuron
 * then stay under DEFY_BATTERY_MAX_AGE_MS even after the Neuron's 15 s poll.
 */
#define BATTERY_REPUBLISH_MS 65000
#define BATTERY_STACK_SIZE 1024
/* Preemptible, below the radio's cooperative link thread. */
#define BATTERY_QUEUE_PRIORITY 5
/* The charger's words, from DEFY_BATTERY_ANALOG_CHARGER to the end of the sample. */
#define CHARGER_WORDS (DEFY_BATTERY_WORD_COUNT - DEFY_BATTERY_ANALOG_CHARGER)

static const struct device *const battery_bus = DEVICE_DT_GET(DT_NODELABEL(i2c1));
static const uint8_t battery_registers[DEFY_BATTERY_WORD_COUNT] = {
    [DEFY_BATTERY_DEVNAME] = 0x21,
    [DEFY_BATTERY_STATUS] = 0x00,
    [DEFY_BATTERY_FSTAT] = 0x3d,
    [DEFY_BATTERY_MODELCFG] = 0xdb,
    [DEFY_BATTERY_REPSOC] = 0x06,
    [DEFY_BATTERY_VCELL] = 0x09,
    [DEFY_BATTERY_CURRENT] = 0x0a,
    [DEFY_BATTERY_DESIGNCAP] = 0x18,
};
static const uint8_t charger_registers[DEFY_BATTERY_CHARGER_REGISTER_COUNT] = {
    [DEFY_BATTERY_CHARGER_ID] = DEFY_CHARGER_REG_ID,
    [DEFY_BATTERY_CHARGER_STATUS] = DEFY_CHARGER_REG_STATUS,
    [DEFY_BATTERY_CHARGER_CONTROL] = DEFY_CHARGER_REG_CONTROL,
    [DEFY_BATTERY_CHARGER_VOLTAGE] = DEFY_CHARGER_REG_VOLTAGE,
    [DEFY_BATTERY_CHARGER_CURRENT] = DEFY_CHARGER_REG_CURRENT,
    [DEFY_BATTERY_CHARGER_SPECIAL] = DEFY_CHARGER_REG_SPECIAL,
    [DEFY_BATTERY_CHARGER_SAFETY] = DEFY_CHARGER_REG_SAFETY,
};

static K_THREAD_STACK_DEFINE(battery_stack, BATTERY_STACK_SIZE);
static struct k_work_q battery_queue;
static atomic_t battery_started;
static atomic_t battery_stopping;
static atomic_t battery_burst;
static atomic_t battery_forced;
static uint32_t battery_sequence;
static bool battery_analog_selected;
static bool charger_status_valid;
static uint8_t charger_status;
static struct defy_battery_filter battery_filter;
static struct defy_battery_sample battery_published;
static bool battery_published_valid;
static int64_t battery_published_at;

static void battery_work_handler(struct k_work *work);
static K_WORK_DELAYABLE_DEFINE(battery_work, battery_work_handler);

static void charger_work_handler(struct k_work *work);
static K_WORK_DELAYABLE_DEFINE(charger_work, charger_work_handler);

int defy_radio_battery_refresh(void) {
    if (!atomic_get(&battery_started) || atomic_get(&battery_stopping)) {
        return -ESHUTDOWN;
    }
    atomic_set(&battery_burst, BATTERY_BURST_SAMPLES);
    atomic_set(&battery_forced, 1);
    k_work_reschedule_for_queue(&battery_queue, &battery_work, K_NO_WAIT);
    return 0;
}

static void charger_work_handler(struct k_work *work) {
    ARG_UNUSED(work);

    if (atomic_get(&battery_stopping)) {
        return;
    }
    int err = defy_radio_charger_service(battery_bus);
    uint8_t status = 0;
    if (err == 0) {
        uint8_t reg = DEFY_CHARGER_REG_STATUS;
        defy_battery_trace_begin();
        err = i2c_write_read(battery_bus, DEFY_CHARGER_ADDRESS, &reg, 1, &status, 1);
    }
    status &= 0x3f;
    bool changed = charger_status_valid != (err == 0) ||
                   (err == 0 && status != charger_status);
    charger_status_valid = err == 0;
    charger_status = status;
    if (changed) {
        defy_radio_battery_refresh();
    }
    if (!atomic_get(&battery_stopping)) {
        k_work_schedule_for_queue(&battery_queue, &charger_work,
                                  K_MSEC(DEFY_CHARGER_SERVICE_INTERVAL_MS));
    }
}

static void charger_read_diagnostic(const struct device *bus,
                                    struct defy_battery_sample *sample) {
    uint8_t reg = DEFY_CHARGER_REG_ID;
    uint8_t value = 0;
    defy_battery_trace_begin();
    uint32_t started = k_cycle_get_32();
    int err = i2c_write_read(bus, DEFY_CHARGER_ADDRESS, &reg, sizeof(reg), &value, sizeof(value));
    uint32_t elapsed = k_cyc_to_us_floor64((uint32_t)(k_cycle_get_32() - started));
    defy_battery_trace_charger(sample, reg, err, value, elapsed);
}

/*
 * Reads the gauge into sample. True if the half has no gauge, from an address
 * NACK or another device's DevName, so the comparator takes over.
 */
static bool battery_gauge_read(const struct device *bus, struct defy_battery_sample *sample) {
    if (!device_is_ready(bus)) {
        sample->status = DEFY_BATTERY_SAMPLE_NO_DEVICE;
        return false;
    }

    for (size_t i = 0; i < ARRAY_SIZE(battery_registers); i++) {
        uint8_t reg = battery_registers[i];
        uint8_t bytes[2];
        defy_battery_trace_begin();
        uint32_t started = k_cycle_get_32();
        int err = i2c_write_read(bus, BATTERY_ADDRESS, &reg, sizeof(reg), bytes, sizeof(bytes));

        if (err < 0) {
            uint32_t elapsed = k_cyc_to_us_floor64((uint32_t)(k_cycle_get_32() - started));
            defy_battery_trace_error(sample, reg, err, elapsed);
            bool address_nack = defy_battery_trace_tx_address_nack(sample);
            if (address_nack) {
                charger_read_diagnostic(bus, sample);
            }
            return i == DEFY_BATTERY_DEVNAME && address_nack;
        }
        sample->words[i] = sys_get_le16(bytes);
        if (i == DEFY_BATTERY_DEVNAME && sample->words[i] != DEFY_BATTERY_GAUGE_DEVNAME) {
            sample->status = DEFY_BATTERY_SAMPLE_NO_DEVICE;
            return true;
        }
    }
    return false;
}

static void charger_read(const struct device *bus, struct defy_battery_sample *sample) {
    memset(&sample->words[DEFY_BATTERY_ANALOG_CHARGER], 0,
           CHARGER_WORDS * sizeof(sample->words[0]));
    if (!device_is_ready(bus)) {
        return;
    }
    for (size_t i = 0; i < ARRAY_SIZE(charger_registers); i++) {
        uint8_t reg = charger_registers[i];
        uint8_t value = 0;
        defy_battery_trace_begin();
        int err = i2c_write_read(bus, DEFY_CHARGER_ADDRESS, &reg, 1, &value, 1);
        if (err != 0) {
            return;
        }
        sample->words[DEFY_BATTERY_ANALOG_CHARGER + i / 2] |= (uint16_t)value << (8 * (i % 2));
        sample->words[DEFY_BATTERY_ANALOG_CHARGER_VALID] |= (uint16_t)(1u << i) << 8;
        if (i == DEFY_BATTERY_CHARGER_ID &&
            (value & DEFY_BATTERY_CHARGER_ID_MASK) != DEFY_BATTERY_CHARGER_ID_VALUE) {
            return;
        }
    }
}

static void battery_read(const struct device *bus, struct defy_battery_sample *sample) {
    if (!battery_analog_selected) {
        battery_analog_selected = battery_gauge_read(bus, sample);
        if (!battery_analog_selected) {
            return;
        }
    }

    struct defy_battery_comp_reading analog;
    int err = defy_radio_battery_comp_read(&analog);
    uint16_t millivolts;
    if (err == 0) {
        err = defy_battery_analog_mv(analog.threshold, analog.reference, &millivolts);
    }
    memset(sample->words, 0, sizeof(sample->words));
    sample->words[DEFY_BATTERY_ANALOG_FORMAT] = DEFY_BATTERY_ANALOG_CHARGER_TAG;
    if (err != 0) {
        sample->status = DEFY_BATTERY_SAMPLE_ANALOG_ERROR;
        sample->words[DEFY_BATTERY_ANALOG_ERRNO] = (uint16_t)err;
    } else {
        sample->status = DEFY_BATTERY_SAMPLE_ANALOG_OK;
        sample->words[DEFY_BATTERY_ANALOG_THRESHOLD] = analog.threshold;
        sample->words[DEFY_BATTERY_ANALOG_REFERENCE] = analog.reference;
        sample->words[DEFY_BATTERY_ANALOG_MILLIVOLTS] = millivolts;
    }
    charger_read(bus, sample);
}

/*
 * A filtered analog sample compares the estimate, not the raw threshold, which
 * can dither between adjacent steps.
 */
static bool battery_changed(const struct defy_battery_sample *sample) {
    const struct defy_battery_sample *last = &battery_published;

    if (!battery_published_valid || sample->status != last->status ||
        sample->words[DEFY_BATTERY_ANALOG_FORMAT] != last->words[DEFY_BATTERY_ANALOG_FORMAT]) {
        return true;
    }
    if (sample->status == DEFY_BATTERY_SAMPLE_ANALOG_OK &&
        sample->words[DEFY_BATTERY_ANALOG_FORMAT] == DEFY_BATTERY_ANALOG_FILTERED_TAG) {
        return (sample->words[DEFY_BATTERY_ANALOG_THRESHOLD] >> 8) !=
                   (last->words[DEFY_BATTERY_ANALOG_THRESHOLD] >> 8) ||
               memcmp(&sample->words[DEFY_BATTERY_ANALOG_CHARGER],
                      &last->words[DEFY_BATTERY_ANALOG_CHARGER],
                      CHARGER_WORDS * sizeof(sample->words[0])) != 0;
    }
    return memcmp(sample->words, last->words, sizeof(sample->words)) != 0;
}

static uint32_t next_delay(void) {
    if (atomic_get(&battery_burst) > 0) {
        return BATTERY_BURST_INTERVAL_MS;
    }
    if (battery_analog_selected) {
        return defy_battery_filter_delay(&battery_filter);
    }
    return charger_status_valid && charger_status != 0 &&
                   charger_status != DEFY_BATTERY_CHARGER_FAULT_SLEEP
               ? BATTERY_POWERED_INTERVAL_MS
               : BATTERY_SAMPLE_INTERVAL_MS;
}

static void battery_work_handler(struct k_work *work) {
    ARG_UNUSED(work);

    if (atomic_get(&battery_stopping)) {
        return;
    }
    if (atomic_get(&battery_burst) > 0) {
        atomic_dec(&battery_burst);
    }
    bool forced = atomic_clear(&battery_forced);
    if (++battery_sequence == 0) {
        battery_sequence = 1;
    }
    struct defy_battery_sample sample = {.sequence = battery_sequence};
    int64_t now = k_uptime_get();

    battery_read(battery_bus, &sample);
    defy_battery_filter_apply(&battery_filter, &sample, now);
    uint32_t delay = next_delay();
    /* Publish only news, a refresh, or a sample the next one would leave too old. */
    if (forced || battery_changed(&sample) ||
        now + delay - battery_published_at > BATTERY_REPUBLISH_MS) {
        int err = defy_radio_battery_publish(&sample);

        if (err == -ESHUTDOWN) {
            return;
        }
        battery_published_valid = err == 0;
        if (err == 0) {
            battery_published = sample;
            battery_published_at = now;
        }
    }
    if (!atomic_get(&battery_stopping)) {
        k_work_schedule_for_queue(&battery_queue, &battery_work, K_MSEC(delay));
    }
}

void defy_radio_battery_start(void) {
    if (!atomic_cas(&battery_started, 0, 1)) {
        return;
    }
    k_work_queue_start(&battery_queue, battery_stack, K_THREAD_STACK_SIZEOF(battery_stack),
                       BATTERY_QUEUE_PRIORITY, NULL);
    k_work_schedule_for_queue(&battery_queue, &charger_work, K_MSEC(BATTERY_START_DELAY_MS));
    k_work_schedule_for_queue(&battery_queue, &battery_work, K_MSEC(BATTERY_START_DELAY_MS));
}

void defy_radio_battery_stop(void) {
    struct k_work_sync sync;

    atomic_set(&battery_stopping, 1);
    k_work_cancel_delayable_sync(&charger_work, &sync);
    k_work_cancel_delayable_sync(&battery_work, &sync);
}
