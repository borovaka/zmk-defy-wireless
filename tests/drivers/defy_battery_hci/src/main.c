/* SPDX-License-Identifier: MIT */

#include <zephyr/bluetooth/hci.h>
#include <zephyr/ztest.h>
#include <dygma/defy_battery.h>
#include <dygma/defy_half_leds.h>
#include <dygma/defy_hci_spi.h>

#include "fake_uptime.h"

static bt_hci_vnd_evt_cb_t *registered_callback;
static bool registered_before_application;
static unsigned int notification_count;
static int notification_error;
static uint32_t notification_generation;
static uint32_t notification_sequence;

/* defy_battery_hci_refresh() passes on to the driver, which this suite does not build. */
int defy_hci_spi_battery_refresh(void) {
    return 0;
}

static void test_battery_updated(void) {
    struct defy_battery_sample sample;
    notification_count++;
    notification_error = defy_battery_hci_get_generation(&sample, &notification_generation);
    if (!notification_error) {
        notification_sequence = sample.sequence;
    }
}

int bt_hci_register_vnd_evt_cb(bt_hci_vnd_evt_cb_t cb) {
    registered_callback = cb;
    return 0;
}

#define defy_half_leds_battery_updated test_battery_updated
#include "../../../../drivers/bluetooth/defy_battery_hci.c"
#undef defy_half_leds_battery_updated
#undef k_uptime_get

static int application_init(void) {
    registered_before_application = registered_callback != NULL;
    return 0;
}
SYS_INIT(application_init, APPLICATION, 0);

static uint8_t payload[DEFY_BATTERY_HCI_PAYLOAD_SIZE + 1];

static void before(void *fixture) {
    ARG_UNUSED(fixture);
    now = 1000;
    notification_count = 0;
    notification_error = 0;
    notification_generation = notification_sequence = 0;
    have_sample = false;
    radio_generation = 1;
    memset(&cached_sample, 0, sizeof(cached_sample));
    memset(payload, 0, sizeof(payload));
    sys_put_le16(DEFY_HCI_SPI_EVT_BATTERY, payload);
    payload[2] = DEFY_BATTERY_VERSION;
    sys_put_le32(10, &payload[4]);
    sys_put_le32(25, &payload[8]);
    for (size_t i = 0; i < DEFY_BATTERY_WORD_COUNT; i++) {
        sys_put_le16(0x1200 + i, &payload[12 + 2 * i]);
    }
}

static bool receive(size_t len) {
    struct net_buf_simple buf;

    net_buf_simple_init_with_data(&buf, payload, len);
    return registered_callback(&buf);
}

ZTEST(battery_hci, test_registered_before_bluetooth_application_init) {
    zassert_true(registered_before_application);
    zassert_equal(registered_callback, battery_event);
}

ZTEST(battery_hci, test_old_radio_is_unavailable) {
    struct defy_battery_sample sample;

    zassert_equal(defy_battery_hci_get(&sample), -ENODATA);
}

ZTEST(battery_hci, test_valid_words_and_elapsed_age) {
    struct defy_battery_sample sample;
    uint32_t generation;

    zassert_true(receive(DEFY_BATTERY_HCI_PAYLOAD_SIZE));
    now += 77;
    zassert_ok(defy_battery_hci_get_generation(&sample, &generation));
    zassert_equal(generation, 1);
    zassert_equal(sample.sequence, 10);
    zassert_equal(sample.age_ms, 102);
    zassert_equal(sample.status, DEFY_BATTERY_SAMPLE_OK);
    for (size_t i = 0; i < DEFY_BATTERY_WORD_COUNT; i++) {
        zassert_equal(sample.words[i], 0x1200 + i);
    }
}

ZTEST(battery_hci, test_malformed_samples_do_not_refresh_cache) {
    struct defy_battery_sample sample;

    zassert_true(receive(DEFY_BATTERY_HCI_PAYLOAD_SIZE));
    now += 500;
    sys_put_le32(11, &payload[4]);
    for (size_t len = 2; len < DEFY_BATTERY_HCI_PAYLOAD_SIZE; len++) {
        zassert_true(receive(len));
    }
    zassert_true(receive(sizeof(payload)));
    payload[2]++;
    zassert_true(receive(DEFY_BATTERY_HCI_PAYLOAD_SIZE));
    payload[2]--;
    payload[3] = DEFY_BATTERY_SAMPLE_ANALOG_ERROR + 1;
    zassert_true(receive(DEFY_BATTERY_HCI_PAYLOAD_SIZE));
    payload[3] = DEFY_BATTERY_SAMPLE_OK;
    sys_put_le32(0, &payload[4]);
    zassert_true(receive(DEFY_BATTERY_HCI_PAYLOAD_SIZE));
    zassert_ok(defy_battery_hci_get(&sample));
    zassert_equal(sample.sequence, 10);
    zassert_equal(sample.age_ms, 525);
    zassert_equal(notification_count, 1);
}

ZTEST(battery_hci, test_unrelated_vendor_events_are_not_claimed) {
    struct defy_battery_sample sample;

    zassert_false(receive(0));
    zassert_false(receive(1));
    sys_put_le16(0x0201, payload);
    zassert_false(receive(DEFY_BATTERY_HCI_PAYLOAD_SIZE));
    zassert_equal(defy_battery_hci_get(&sample), -ENODATA);
}

ZTEST(battery_hci, test_duplicate_and_older_sample_do_not_refresh) {
    struct defy_battery_sample sample;

    zassert_true(receive(DEFY_BATTERY_HCI_PAYLOAD_SIZE));
    now += 50;
    sys_put_le32(0, &payload[8]);
    zassert_true(receive(DEFY_BATTERY_HCI_PAYLOAD_SIZE));
    sys_put_le32(9, &payload[4]);
    zassert_true(receive(DEFY_BATTERY_HCI_PAYLOAD_SIZE));
    zassert_ok(defy_battery_hci_get(&sample));
    zassert_equal(sample.sequence, 10);
    zassert_equal(sample.age_ms, 75);
    zassert_equal(notification_count, 1);
}

ZTEST(battery_hci, test_reset_invalidates_and_changes_generation) {
    struct defy_battery_sample sample;
    uint32_t generation;

    zassert_true(receive(DEFY_BATTERY_HCI_PAYLOAD_SIZE));
    defy_battery_hci_reset();
    zassert_equal(defy_battery_hci_get(&sample), -ENODATA);
    sys_put_le32(1, &payload[4]);
    zassert_true(receive(DEFY_BATTERY_HCI_PAYLOAD_SIZE));
    zassert_ok(defy_battery_hci_get_generation(&sample, &generation));
    zassert_equal(sample.sequence, 1);
    zassert_equal(generation, 2);
}

ZTEST(battery_hci, test_notification_runs_after_unlock_and_sees_new_cache) {
    zassert_true(receive(DEFY_BATTERY_HCI_PAYLOAD_SIZE));
    zassert_equal(notification_count, 1);
    zassert_ok(notification_error);
    zassert_equal(notification_generation, 1);
    zassert_equal(notification_sequence, 10);
    sys_put_le32(11, &payload[4]);
    zassert_true(receive(DEFY_BATTERY_HCI_PAYLOAD_SIZE));
    zassert_equal(notification_count, 2);
    zassert_equal(notification_sequence, 11);
}

ZTEST(battery_hci, test_reset_notification_sees_invalid_cache_before_next_generation) {
    zassert_true(receive(DEFY_BATTERY_HCI_PAYLOAD_SIZE));
    defy_battery_hci_reset();
    zassert_equal(notification_count, 2);
    zassert_equal(notification_error, -ENODATA);
    sys_put_le32(1, &payload[4]);
    zassert_true(receive(DEFY_BATTERY_HCI_PAYLOAD_SIZE));
    zassert_equal(notification_count, 3);
    zassert_equal(notification_generation, 2);
    zassert_equal(notification_sequence, 1);
}

ZTEST_SUITE(battery_hci, NULL, NULL, before, NULL, NULL);
