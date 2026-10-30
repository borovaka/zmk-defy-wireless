/* SPDX-License-Identifier: MIT */

#include <errno.h>
#include <string.h>

#include <zephyr/device.h>
#include <zephyr/drivers/i2c.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/byteorder.h>
#include <zephyr/ztest.h>

#include "../../../../apps/defy_radio/src/battery_trace.c"
static bool hold_refresh;
static unsigned int refresh_schedules;
static int test_reschedule(struct k_work_q *queue, struct k_work_delayable *work, k_timeout_t delay) {
    refresh_schedules++;
    return hold_refresh ? 1 : k_work_reschedule_for_queue(queue, work, delay);
}

#define k_work_reschedule_for_queue test_reschedule
#include "../../../../apps/defy_radio/src/battery.c"
#undef k_work_reschedule_for_queue

NRF_TWIM_Type fake_twim1;
NRF_GPIO_Type fake_gpio0;
static nrfx_twim_evt_handler_t registered_handler;
static void *registered_context;
static const nrfx_twim_t *registered_instance;
static const nrfx_twim_config_t *registered_config;
static unsigned int forwarded_count;
static const nrfx_twim_evt_t *forwarded_event;
static void *forwarded_context;
static bool omit_completion;
static nrfx_twim_evt_type_t failure_event;
static size_t charger_transfers;
static int charger_error;
static uint8_t charger_value;
static int charger_failure_register;
static size_t charger_index;
static const uint8_t expected_charger_registers[] = {0x03, 0x00, 0x01, 0x02, 0x04, 0x05, 0x06};
static uint8_t charger_values[] = {0x94, 0x10, 0xc8, 0x8c, 0x79, 0x44, 0x70};
static int driver_context;
static const nrfx_twim_t twim_instance = {.p_twim = NRF_TWIM1};
static const nrfx_twim_config_t twim_configuration = {.frequency = 100000};

nrfx_err_t __real_nrfx_twim_init(const nrfx_twim_t *instance,
                                const nrfx_twim_config_t *config,
                                nrfx_twim_evt_handler_t handler, void *context) {
    registered_instance = instance;
    registered_config = config;
    registered_handler = handler;
    registered_context = context;
    return NRFX_SUCCESS;
}

static void driver_handler(const nrfx_twim_evt_t *event, void *context) {
    forwarded_count++;
    forwarded_event = event;
    forwarded_context = context;
}

static void emit_completion(nrfx_twim_evt_type_t event_type, nrfx_twim_xfer_type_t type) {
    nrfx_twim_evt_t event = {.type = event_type, .xfer_desc = {.type = type}};
    registered_handler(&event, registered_context);
    zassert_equal(forwarded_event, &event);
    zassert_equal(forwarded_context, &driver_context);
}

static void initialize_trace(void) {
    zassert_equal(__wrap_nrfx_twim_init(&twim_instance, &twim_configuration, driver_handler,
                                      &driver_context), NRFX_SUCCESS);
    defy_battery_trace_begin();
}

static const uint8_t expected_registers[] = {0x21, 0x00, 0x3d, 0xdb, 0x06, 0x09, 0x0a, 0x18};
static const uint16_t expected_words[] = {0x4031, 0x8082, 0x0000, 0x0000,
                                         0x3c80, 0xbeef, 0xff80, 0x1284};
static size_t transfer_count;
static int failing_transfer;
static int transfer_error;
static uint16_t gauge_identity = 0x4031;
static unsigned int analog_count;
static int analog_error;
static struct defy_battery_comp_reading analog_result = {
    .threshold = 54, .reference = DEFY_BATTERY_ANALOG_REFERENCE_VDD,
};

int defy_radio_battery_comp_read(struct defy_battery_comp_reading *out) {
    analog_count++;
    if (analog_error == 0) {
        *out = analog_result;
    }
    return analog_error;
}
static int publish_result;
static struct defy_battery_sample published;
static unsigned int publish_count;
static k_tid_t publisher_thread;
static K_SEM_DEFINE(published_sem, 0, 1);
static int64_t initial_elapsed;
static bool initial_on_worker;
static bool initial_charger_on_worker;
static unsigned int charger_service_count;
static int charger_service_result;
static k_tid_t charger_thread;
static bool stop_during_charger_service;

int defy_radio_charger_service(const struct device *bus) {
    zassert_equal(bus, battery_bus);
    charger_service_count++;
    charger_thread = k_current_get();
    if (stop_during_charger_service) {
        atomic_set(&battery_stopping, 1);
    }
    return charger_service_result;
}

static int gauge_transfer(const struct device *dev, struct i2c_msg *msgs, uint8_t count,
                          uint16_t address) {
    ARG_UNUSED(dev);

    zassert_equal(count, 2);
    zassert_equal(msgs[0].len, 1);
    zassert_equal(msgs[0].flags, I2C_MSG_WRITE);
    zassert_equal(msgs[1].flags, I2C_MSG_READ | I2C_MSG_RESTART | I2C_MSG_STOP);
    zassert_equal(trace_count, 0);
    if (address == 0x6a) {
        zassert_false((uintptr_t)msgs[0].buf >= (uintptr_t)charger_registers &&
                      (uintptr_t)msgs[0].buf <
                          (uintptr_t)charger_registers + sizeof(charger_registers));
        uint8_t reg = msgs[0].buf[0];
        if (reg == 0 && charger_index != 1) {
            msgs[1].buf[0] = charger_values[1];
            return charger_error;
        }
        if (reg == 0x03) {
            charger_index = 0;
        }
        zassert_true(charger_index < ARRAY_SIZE(expected_charger_registers));
        zassert_equal(reg, expected_charger_registers[charger_index]);
        zassert_equal(msgs[1].len, 1);
        charger_transfers++;
        msgs[1].buf[0] = charger_index == 0 ? charger_value : charger_values[charger_index];
        charger_index++;
        int err = charger_failure_register == reg ? charger_error : 0;
        if (err) {
            emit_completion(NRFX_TWIM_EVT_ADDRESS_NACK, NRFX_TWIM_XFER_TX);
        } else {
            emit_completion(NRFX_TWIM_EVT_DONE, NRFX_TWIM_XFER_TX);
            emit_completion(NRFX_TWIM_EVT_DONE, NRFX_TWIM_XFER_RX);
        }
        return err;
    }
    zassert_equal(address, 0x36);
    zassert_equal(msgs[1].len, 2);
    size_t index = transfer_count++;
    zassert_true(index < ARRAY_SIZE(expected_registers));
    zassert_equal(msgs[0].buf[0], expected_registers[index]);

    if ((int)index == failing_transfer) {
        if (!omit_completion) {
            emit_completion(failure_event, NRFX_TWIM_XFER_TX);
        }
        return transfer_error;
    }
    emit_completion(NRFX_TWIM_EVT_DONE, NRFX_TWIM_XFER_TX);
    emit_completion(NRFX_TWIM_EVT_DONE, NRFX_TWIM_XFER_RX);
    sys_put_le16(index == 0 ? gauge_identity : expected_words[index], msgs[1].buf);
    return 0;
}

static DEVICE_API(i2c, gauge_api) = {.transfer = gauge_transfer};

I2C_DEVICE_DT_DEFINE(DT_NODELABEL(i2c1), NULL, NULL, NULL, NULL, POST_KERNEL,
                     CONFIG_I2C_INIT_PRIORITY, &gauge_api);

int defy_radio_battery_publish(const struct defy_battery_sample *sample) {
    published = *sample;
    publish_count++;
    publisher_thread = k_current_get();
    k_sem_give(&published_sem);
    return publish_result;
}

static void cancel_sampling(void) {
    struct k_work_sync sync;

    k_work_cancel_delayable_sync(&charger_work, &sync);
    k_work_cancel_delayable_sync(&battery_work, &sync);
}

static void *setup(void) {
    initialize_trace();
    failing_transfer = -1;
    publish_result = -ESHUTDOWN;
    int64_t started = k_uptime_get();

    defy_radio_battery_start();
    zassert_ok(k_sem_take(&published_sem, K_MSEC(1500)));
    initial_elapsed = k_uptime_get() - started;
    initial_on_worker = publisher_thread == &battery_queue.thread;
    initial_charger_on_worker = charger_thread == &battery_queue.thread;
    cancel_sampling();
    return NULL;
}

static void before(void *fixture) {
    ARG_UNUSED(fixture);
    cancel_sampling();
    atomic_clear(&battery_stopping);
    atomic_clear(&battery_burst);
    atomic_clear(&battery_forced);
    memset(&battery_filter, 0, sizeof(battery_filter));
    memset(&battery_published, 0, sizeof(battery_published));
    battery_published_valid = false;
    battery_published_at = 0;
    charger_status_valid = false;
    charger_status = 0;
    charger_values[1] = 0x10;
    hold_refresh = true;
    refresh_schedules = 0;
    charger_service_count = 0;
    charger_service_result = 0;
    stop_during_charger_service = false;
    transfer_count = 0;
    failing_transfer = -1;
    transfer_error = -EIO;
    battery_analog_selected = false;
    gauge_identity = 0x4031;
    analog_count = 0;
    analog_error = 0;
    analog_result = (struct defy_battery_comp_reading){
        .threshold = 54, .reference = DEFY_BATTERY_ANALOG_REFERENCE_VDD,
    };
    publish_result = -ESHUTDOWN;
    publish_count = 0;
    memset(&published, 0, sizeof(published));
    k_sem_reset(&published_sem);
    forwarded_count = 0;
    omit_completion = false;
    failure_event = NRFX_TWIM_EVT_DATA_NACK;
    charger_transfers = 0;
    charger_error = 0;
    charger_value = 0x94;
    charger_failure_register = -1;
    charger_index = 0;
    initialize_trace();
}

static void after(void *fixture) {
    ARG_UNUSED(fixture);
    cancel_sampling();
}

ZTEST(defy_radio_battery, test_refresh_burst_publishes_once_then_fills_at_one_hertz) {
    battery_analog_selected = true;
    charger_values[1] = 0;
    publish_result = 0;
    hold_refresh = false;
    zassert_ok(defy_radio_battery_refresh());
    zassert_ok(k_sem_take(&published_sem, K_MSEC(100)));
    zassert_equal(published.age_ms, 0);
    zassert_equal(published.words[1] >> 8, DEFY_BATTERY_LEVEL_UNAVAILABLE);
    k_sleep(K_MSEC(BATTERY_BURST_INTERVAL_MS * (BATTERY_BURST_SAMPLES - 1) + 10));
    zassert_equal(analog_count, BATTERY_BURST_SAMPLES);
    zassert_equal(publish_count, 1);
    int64_t delay = k_ticks_to_ms_ceil64(k_work_delayable_remaining_get(&battery_work));
    zassert_between_inclusive(delay, 900, 1000);
    zassert_between_inclusive(battery_filter.count, 2, 3);
    defy_radio_battery_stop();
    zassert_equal(defy_radio_battery_refresh(), -ESHUTDOWN);
    zassert_equal(k_work_delayable_remaining_get(&battery_work), 0);
}

/* Discharge samples at 1 Hz until the filter settles on 70%, published when first sampled
 * and when the estimate appears. */
static void settle_on_battery(void) {
    battery_analog_selected = true;
    charger_values[1] = 0;
    charger_status_valid = true;
    charger_status = 0;
    analog_result.threshold = 52;
    publish_result = 0;
    hold_refresh = false;
    k_work_reschedule_for_queue(&battery_queue, &battery_work, K_NO_WAIT);
    k_sleep(K_MSEC(4500));
    zassert_equal(analog_count, 5);
    zassert_equal(publish_count, 2);
    zassert_true(battery_filter.valid);
    zassert_equal(published.words[1] >> 8, 70);
    int64_t delay = k_ticks_to_ms_ceil64(k_work_delayable_remaining_get(&battery_work));
    zassert_between_inclusive(delay, DEFY_BATTERY_FILTER_SETTLED_INTERVAL_MS - 1000,
                              DEFY_BATTERY_FILTER_SETTLED_INTERVAL_MS);
    k_sem_reset(&published_sem);
}

ZTEST(defy_radio_battery, test_settled_estimate_backs_off_and_republishes_every_minute) {
    charger_values[1] = 0;
    charger_status_valid = true;
    charger_status = 0;
    k_work_reschedule_for_queue(&battery_queue, &charger_work, K_NO_WAIT);
    settle_on_battery();
    analog_result.threshold = 53;
    k_sleep(K_MSEC(DEFY_BATTERY_FILTER_SETTLED_INTERVAL_MS));
    zassert_equal(analog_count, 6);
    zassert_equal(publish_count, 2);
    analog_result.threshold = 52;
    k_sleep(K_MSEC(45000));
    zassert_equal(analog_count, 10);
    zassert_equal(publish_count, 2);
    k_sleep(K_MSEC(5000));
    zassert_equal(analog_count, 11);
    zassert_equal(publish_count, 3);
    zassert_equal(published.age_ms, 0);
    zassert_equal(published.words[1] >> 8, 70);
    zassert_equal(charger_service_count, 13);
    zassert_equal(refresh_schedules, 0);
}

ZTEST(defy_radio_battery, test_refresh_publishes_unchanged_estimate_once) {
    settle_on_battery();
    uint32_t sequence = published.sequence;
    zassert_ok(defy_radio_battery_refresh());
    zassert_ok(k_sem_take(&published_sem, K_MSEC(100)));
    zassert_true(published.sequence > sequence);
    zassert_equal(published.age_ms, 0);
    zassert_equal(published.words[1] >> 8, 70);
    k_sleep(K_MSEC(BATTERY_BURST_INTERVAL_MS * BATTERY_BURST_SAMPLES));
    zassert_equal(analog_count, 5 + BATTERY_BURST_SAMPLES);
    zassert_equal(publish_count, 3);
    int64_t delay = k_ticks_to_ms_ceil64(k_work_delayable_remaining_get(&battery_work));
    zassert_between_inclusive(delay, DEFY_BATTERY_FILTER_SETTLED_INTERVAL_MS - 1000,
                              DEFY_BATTERY_FILTER_SETTLED_INTERVAL_MS);
}

ZTEST(defy_radio_battery, test_comparator_glitch_retries_in_one_second_and_keeps_estimate) {
    settle_on_battery();
    analog_error = -ETIMEDOUT;
    k_work_reschedule_for_queue(&battery_queue, &battery_work, K_NO_WAIT);
    zassert_ok(k_sem_take(&published_sem, K_MSEC(100)));
    zassert_equal(published.status, DEFY_BATTERY_SAMPLE_ANALOG_ERROR);
    k_sleep(K_MSEC(10));
    int64_t delay = k_ticks_to_ms_ceil64(k_work_delayable_remaining_get(&battery_work));
    zassert_between_inclusive(delay, 900, DEFY_BATTERY_FILTER_INTERVAL_MS);
    analog_error = 0;
    zassert_ok(k_sem_take(&published_sem, K_MSEC(1100)));
    zassert_equal(published.status, DEFY_BATTERY_SAMPLE_ANALOG_OK);
    zassert_equal(published.words[1] >> 8, 70);
    zassert_equal(publish_count, 4);
    k_sleep(K_MSEC(10));
    delay = k_ticks_to_ms_ceil64(k_work_delayable_remaining_get(&battery_work));
    zassert_between_inclusive(delay, DEFY_BATTERY_FILTER_SETTLED_INTERVAL_MS - 1000,
                              DEFY_BATTERY_FILTER_SETTLED_INTERVAL_MS);
}

ZTEST(defy_radio_battery, test_charger_transition_requests_refresh_but_steady_state_does_not) {
    charger_work_handler(NULL);
    zassert_equal(refresh_schedules, 1);
    charger_work_handler(NULL);
    zassert_equal(refresh_schedules, 1);
    charger_values[1] = 0xc2;
    charger_work_handler(NULL);
    zassert_equal(refresh_schedules, 2);
    zassert_equal(charger_status, 2);
    charger_service_result = -EIO;
    charger_work_handler(NULL);
    zassert_equal(refresh_schedules, 3);
    zassert_false(charger_status_valid);
}

ZTEST(defy_radio_battery, test_cabled_sampling_checks_switch_state_every_five_seconds) {
    charger_status_valid = true;
    charger_status = 0x10;
    publish_result = 0;
    battery_work_handler(NULL);
    int64_t delay = k_ticks_to_ms_ceil64(k_work_delayable_remaining_get(&battery_work));
    zassert_between_inclusive(delay, 4900, 5001);
}

ZTEST(defy_radio_battery, test_ready_with_or_without_no_input_code_uses_idle_sampling) {
    const uint8_t states[] = {0x00, 0x02};
    for (size_t i = 0; i < ARRAY_SIZE(states); i++) {
        cancel_sampling();
        transfer_count = 0;
        charger_status_valid = true;
        charger_status = states[i];
        publish_result = 0;
        battery_work_handler(NULL);
        int64_t delay = k_ticks_to_ms_ceil64(k_work_delayable_remaining_get(&battery_work));
        zassert_between_inclusive(delay, 29000, 30001);
    }
}

ZTEST(defy_radio_battery, test_reads_only_expected_registers_little_endian) {
    struct defy_battery_sample sample = {0};

    battery_read(battery_bus, &sample);
    zassert_false(battery_analog_selected);
    zassert_equal(analog_count, 0);
    zassert_equal(sample.status, DEFY_BATTERY_SAMPLE_OK);
    zassert_equal(transfer_count, ARRAY_SIZE(expected_registers));
    zassert_equal(charger_transfers, 0);
    zassert_mem_equal(sample.words, expected_words, sizeof(expected_words));
}

ZTEST(defy_radio_battery, test_first_error_stops_and_identifies_failed_register) {
    for (size_t i = 0; i < ARRAY_SIZE(expected_registers); i++) {
        struct defy_battery_sample sample = {0};

        transfer_count = 0;
        failing_transfer = i;
        battery_gauge_read(battery_bus, &sample);
        zassert_equal(sample.status, DEFY_BATTERY_SAMPLE_I2C_DIAGNOSTIC);
        zassert_equal(transfer_count, i + 1);
        zassert_equal(sample.words[DEFY_BATTERY_I2C_TAG], DEFY_BATTERY_I2C_DIAGNOSTIC_TAG);
        zassert_equal(sample.words[DEFY_BATTERY_I2C_REGISTER_EVENTS],
                      expected_registers[i] | 0x100);
        zassert_equal((int16_t)sample.words[DEFY_BATTERY_I2C_ERRNO], -EIO);
        zassert_equal(sample.words[DEFY_BATTERY_I2C_EVENT_TYPE], NRFX_TWIM_EVT_DATA_NACK);
        zassert_equal(charger_transfers, 0);
    }
}

ZTEST(defy_radio_battery, test_start_delay_and_dedicated_queue) {
    zassert_between_inclusive(initial_elapsed, 1000, 1500);
    zassert_true(initial_on_worker);
    zassert_true(initial_charger_on_worker);
    defy_radio_battery_start();
    zassert_equal(k_work_delayable_remaining_get(&battery_work), 0);
    zassert_equal(publish_count, 0);
}

ZTEST(defy_radio_battery, test_charger_uses_five_second_retry_without_sampling_battery) {
    const int results[] = {0, -EIO, -ENODEV, -EBADMSG, -ETIMEDOUT};

    for (size_t i = 0; i < ARRAY_SIZE(results); i++) {
        charger_service_result = results[i];
        charger_work_handler(NULL);
        int64_t delay = k_ticks_to_ms_ceil64(k_work_delayable_remaining_get(&charger_work));
        zassert_between_inclusive(delay, 4900, 5001);
        zassert_equal(charger_service_count, i + 1);
        zassert_equal(transfer_count, 0);
        zassert_equal(analog_count, 0);
        zassert_equal(publish_count, 0);
        cancel_sampling();
    }
}

ZTEST(defy_radio_battery, test_stop_cancels_both_workers_and_prevents_rearming) {
    charger_work_handler(NULL);
    publish_result = 0;
    battery_work_handler(NULL);
    zassert_true(k_work_delayable_remaining_get(&charger_work) > 0);
    zassert_true(k_work_delayable_remaining_get(&battery_work) > 0);
    defy_radio_battery_stop();
    zassert_equal(k_work_delayable_remaining_get(&charger_work), 0);
    zassert_equal(k_work_delayable_remaining_get(&battery_work), 0);
    charger_work_handler(NULL);
    battery_work_handler(NULL);
    zassert_equal(charger_service_count, 1);
    zassert_equal(publish_count, 1);
}

ZTEST(defy_radio_battery, test_shutdown_during_charger_service_does_not_rearm) {
    stop_during_charger_service = true;
    charger_work_handler(NULL);
    zassert_equal(charger_service_count, 1);
    zassert_equal(k_work_delayable_remaining_get(&charger_work), 0);
}

ZTEST(defy_radio_battery, test_completed_sample_has_fresh_age_and_nonzero_sequence) {
    battery_sequence = UINT32_MAX;
    battery_work_handler(NULL);
    zassert_equal(published.sequence, 1);
    zassert_equal(published.age_ms, 0);
    zassert_equal(publish_count, 1);
    zassert_mem_equal(published.words, expected_words, sizeof(expected_words));

    transfer_count = 0;
    failing_transfer = 2;
    battery_work_handler(NULL);
    zassert_equal(published.sequence, 2);
    zassert_equal(published.status, DEFY_BATTERY_SAMPLE_I2C_DIAGNOSTIC);
    zassert_equal(published.age_ms, 0);
    zassert_equal(published.words[DEFY_BATTERY_I2C_TAG], DEFY_BATTERY_I2C_DIAGNOSTIC_TAG);
}

ZTEST(defy_radio_battery, test_shutdown_stops_rescheduling_even_after_read_error) {
    failing_transfer = 0;
    battery_work_handler(NULL);
    zassert_equal(publish_count, 1);
    zassert_equal(published.status, DEFY_BATTERY_SAMPLE_I2C_DIAGNOSTIC);
    zassert_equal(k_work_delayable_remaining_get(&battery_work), 0);
}

ZTEST(defy_radio_battery, test_failed_sample_recovers_on_next_queued_run) {
    failing_transfer = 4;
    failure_event = NRFX_TWIM_EVT_ADDRESS_NACK;
    publish_result = -EAGAIN;
    battery_work_handler(NULL);
    uint32_t failed_sequence = published.sequence;

    zassert_equal(published.status, DEFY_BATTERY_SAMPLE_I2C_DIAGNOSTIC);
    zassert_equal(published.words[DEFY_BATTERY_I2C_CHARGER_FORMAT],
                  DEFY_BATTERY_I2C_CHARGER_TAG);
    zassert_true(k_work_delayable_remaining_get(&battery_work) > 0);
    k_sem_reset(&published_sem);
    transfer_count = 0;
    failing_transfer = -1;
    publish_result = -ESHUTDOWN;
    k_work_reschedule_for_queue(&battery_queue, &battery_work, K_NO_WAIT);
    zassert_ok(k_sem_take(&published_sem, K_MSEC(100)));
    cancel_sampling();
    zassert_equal(published.sequence, failed_sequence + 1);
    zassert_equal(published.status, DEFY_BATTERY_SAMPLE_OK);
    zassert_mem_equal(published.words, expected_words, sizeof(expected_words));
    zassert_equal(publisher_thread, &battery_queue.thread);
}

ZTEST(defy_radio_battery, test_wrapper_preserves_instance_config_context_and_event) {
    zassert_equal(registered_instance, &twim_instance);
    zassert_equal(registered_config, &twim_configuration);
    zassert_equal(registered_context, &driver_context);
    zassert_not_equal(registered_handler, driver_handler);
    emit_completion(NRFX_TWIM_EVT_DATA_NACK, NRFX_TWIM_XFER_RX);
    zassert_equal(forwarded_count, 1);
    struct defy_battery_sample sample = {0};
    defy_battery_trace_error(&sample, 0x21, -EIO, 123456);
    zassert_equal(sample.words[DEFY_BATTERY_I2C_REGISTER_EVENTS], 0x121);
    zassert_equal(sample.words[DEFY_BATTERY_I2C_ELAPSED_LOW], 123456 & 0xffff);
    zassert_equal(sample.words[DEFY_BATTERY_I2C_ELAPSED_HIGH], 123456 >> 16);
    zassert_equal(sample.words[DEFY_BATTERY_I2C_EVENT_TYPE],
                  NRFX_TWIM_EVT_DATA_NACK | NRFX_TWIM_XFER_RX << 8);
}

ZTEST(defy_radio_battery, test_probe_condition_requires_actual_tx_address_nack) {
    const uint16_t events[] = {DEFY_BATTERY_I2C_NO_EVENT, NRFX_TWIM_EVT_DONE,
                               NRFX_TWIM_EVT_DATA_NACK, NRFX_TWIM_EVT_OVERRUN,
                               NRFX_TWIM_EVT_BUS_ERROR,
                               NRFX_TWIM_EVT_ADDRESS_NACK | NRFX_TWIM_XFER_RX << 8};
    struct defy_battery_sample sample = {.status = DEFY_BATTERY_SAMPLE_I2C_DIAGNOSTIC};
    sample.words[DEFY_BATTERY_I2C_TAG] = DEFY_BATTERY_I2C_DIAGNOSTIC_TAG;
    sample.words[DEFY_BATTERY_I2C_REGISTER_EVENTS] = 0x121;
    for (size_t i = 0; i < ARRAY_SIZE(events); i++) {
        sample.words[DEFY_BATTERY_I2C_EVENT_TYPE] = events[i];
        zassert_false(defy_battery_trace_tx_address_nack(&sample));
    }
    sample.words[DEFY_BATTERY_I2C_EVENT_TYPE] = NRFX_TWIM_EVT_ADDRESS_NACK;
    sample.words[DEFY_BATTERY_I2C_REGISTER_EVENTS] = 0x21;
    zassert_false(defy_battery_trace_tx_address_nack(&sample));
    sample.words[DEFY_BATTERY_I2C_REGISTER_EVENTS] = 0x121;
    zassert_true(defy_battery_trace_tx_address_nack(&sample));
    sample.status = DEFY_BATTERY_SAMPLE_OK;
    zassert_false(defy_battery_trace_tx_address_nack(&sample));
}

ZTEST(defy_radio_battery, test_initial_gauge_nack_selects_analog_once_per_boot) {
    failing_transfer = 0;
    failure_event = NRFX_TWIM_EVT_ADDRESS_NACK;
    struct defy_battery_sample sample = {.sequence = 77};
    battery_read(battery_bus, &sample);
    zassert_true(battery_analog_selected);
    zassert_equal(transfer_count, 1);
    zassert_equal(charger_transfers, 8);
    zassert_equal(analog_count, 1);
    zassert_equal(sample.sequence, 77);
    zassert_equal(sample.status, DEFY_BATTERY_SAMPLE_ANALOG_OK);
    const uint16_t expected[] = {DEFY_BATTERY_ANALOG_CHARGER_TAG, 54,
                                 DEFY_BATTERY_ANALOG_REFERENCE_VDD,
                                 4082, 0x1094, 0x8cc8, 0x4479, 0x7f70};
    zassert_mem_equal(sample.words, expected, sizeof(expected));
    struct defy_battery_reading reading;
    zassert_ok(defy_battery_sample_reading(&sample, &reading));
    zassert_equal(reading.source, DEFY_BATTERY_SOURCE_ANALOG);
    battery_read(battery_bus, &sample);
    zassert_equal(transfer_count, 1);
    zassert_equal(charger_transfers, 15);
    zassert_equal(analog_count, 2);
}

ZTEST(defy_radio_battery, test_wrong_gauge_identity_selects_analog_with_charger_reads) {
    gauge_identity = 0x1234;
    struct defy_battery_sample sample = {.sequence = 1};
    battery_read(battery_bus, &sample);
    zassert_true(battery_analog_selected);
    zassert_equal(transfer_count, 1);
    zassert_equal(charger_transfers, 7);
    zassert_equal(analog_count, 1);
    zassert_equal(sample.status, DEFY_BATTERY_SAMPLE_ANALOG_OK);
}

ZTEST(defy_radio_battery, test_data_nack_or_timeout_does_not_select_analog) {
    failing_transfer = 0;
    struct defy_battery_sample sample = {.sequence = 1};
    battery_read(battery_bus, &sample);
    zassert_false(battery_analog_selected);
    zassert_equal(analog_count, 0);
    zassert_equal(sample.status, DEFY_BATTERY_SAMPLE_I2C_DIAGNOSTIC);
    transfer_count = 0;
    omit_completion = true;
    battery_read(battery_bus, &sample);
    zassert_false(battery_analog_selected);
    zassert_equal(analog_count, 0);
}

ZTEST(defy_radio_battery, test_analog_failure_replaces_previous_values_and_retries_next_sample) {
    battery_analog_selected = true;
    struct defy_battery_sample sample = {.sequence = 1};
    battery_read(battery_bus, &sample);
    zassert_equal(sample.status, DEFY_BATTERY_SAMPLE_ANALOG_OK);
    analog_error = -ETIMEDOUT;
    battery_read(battery_bus, &sample);
    zassert_equal(sample.status, DEFY_BATTERY_SAMPLE_ANALOG_ERROR);
    const uint16_t expected[] = {DEFY_BATTERY_ANALOG_CHARGER_TAG, (uint16_t)-ETIMEDOUT, 0, 0,
                                 0x1094, 0x8cc8, 0x4479, 0x7f70};
    zassert_mem_equal(sample.words, expected, sizeof(expected));
    struct defy_battery_reading reading;
    zassert_equal(defy_battery_sample_reading(&sample, &reading), -ENODATA);
    struct defy_battery_charger charger;
    zassert_ok(defy_battery_sample_charger(&sample, &charger));
    zassert_equal(charger.state, DEFY_BATTERY_CHARGE_CHARGING);
    analog_error = 0;
    battery_read(battery_bus, &sample);
    zassert_equal(sample.status, DEFY_BATTERY_SAMPLE_ANALOG_OK);
    zassert_equal(analog_count, 3);
    zassert_equal(transfer_count, 0);
}

ZTEST(defy_radio_battery, test_analog_queued_publish_has_fresh_sequence_and_interval) {
    battery_analog_selected = true;
    publish_result = -ENOBUFS;
    battery_sequence = UINT32_MAX;
    battery_work_handler(NULL);
    zassert_equal(published.sequence, 1);
    zassert_equal(published.age_ms, 0);
    zassert_equal(published.status, DEFY_BATTERY_SAMPLE_ANALOG_OK);
    zassert_equal(published.words[0], DEFY_BATTERY_ANALOG_FILTERED_TAG);
    zassert_equal(published.words[1] >> 8, DEFY_BATTERY_LEVEL_UNAVAILABLE);
    zassert_equal(analog_count, 1);
    zassert_equal(transfer_count, 0);
    int64_t delay = k_ticks_to_ms_ceil64(k_work_delayable_remaining_get(&battery_work));
    zassert_between_inclusive(delay, 4900, 5001);
}

ZTEST(defy_radio_battery, test_charger_first_error_stops_and_clears_unread_values) {
    battery_analog_selected = true;
    for (size_t i = 0; i < ARRAY_SIZE(expected_charger_registers); i++) {
        struct defy_battery_sample sample = {.sequence = 1};
        charger_transfers = 0;
        charger_failure_register = expected_charger_registers[i];
        charger_error = -EIO;
        memset(sample.words, 0xff, sizeof(sample.words));
        battery_read(battery_bus, &sample);
        zassert_equal(sample.status, DEFY_BATTERY_SAMPLE_ANALOG_OK);
        zassert_equal(charger_transfers, i + 1);
        zassert_equal(sample.words[7] >> 8, (1u << i) - 1u);
        for (size_t j = 0; j < ARRAY_SIZE(expected_charger_registers); j++) {
            uint8_t value = sample.words[4 + j / 2] >> (8 * (j % 2));
            zassert_equal(value, j < i ? charger_values[j] : 0);
        }
        struct defy_battery_reading reading;
        zassert_ok(defy_battery_sample_reading(&sample, &reading));
        zassert_equal(reading.level, 100);
    }
}

ZTEST(defy_radio_battery, test_charger_identity_guard_accepts_only_verified_family) {
    battery_analog_selected = true;
    const uint8_t identities[] = {0, 0x50, 0x93, 0x94, 0x95, 0x96, 0x97, 0x98, 0xff};
    for (size_t i = 0; i < ARRAY_SIZE(identities); i++) {
        charger_value = identities[i];
        charger_transfers = 0;
        struct defy_battery_sample sample = {.sequence = 1};
        battery_read(battery_bus, &sample);
        bool accepted = charger_value >= 0x94 && charger_value <= 0x97;
        zassert_equal(charger_transfers, accepted ? 7 : 1);
        zassert_equal(sample.words[7] >> 8, accepted ? 0x7f : 1);
        struct defy_battery_charger charger;
        zassert_equal(defy_battery_sample_charger(&sample, &charger),
                      accepted ? 0 : -ENODEV);
    }
}

ZTEST_SUITE(defy_radio_battery, NULL, setup, before, after, NULL);
