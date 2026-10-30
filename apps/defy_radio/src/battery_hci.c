/*
 * The samples battery.c publishes, as vendor events for the host
 * (include/dygma/defy_battery_hci.h). One buffer holds the event: a sample
 * published while it waits to be read replaces any other waiting sample, and
 * goes out once the host has read the event before it. The age in a queued
 * event is brought up to date in the copy armed for each read, never in the
 * queued buffer.
 *
 * SPDX-License-Identifier: MIT
 */

#include <errno.h>

#include <zephyr/bluetooth/buf.h>
#include <zephyr/bluetooth/hci_types.h>
#include <zephyr/kernel.h>
#include <zephyr/net_buf.h>
#include <zephyr/sys/byteorder.h>

#include <dygma/defy_battery_hci.h>
#include <dygma/defy_hci_spi.h>

#include "battery.h"
#include "bridge.h"

NET_BUF_POOL_FIXED_DEFINE(battery_pool, 1,
                          BT_BUF_RESERVE + sizeof(struct bt_hci_evt_hdr) +
                              DEFY_BATTERY_HCI_PAYLOAD_SIZE,
                          sizeof(struct bt_buf_data), NULL);

static struct k_spinlock battery_lock;
static struct defy_battery_sample battery_latest;
static int64_t battery_latest_at;
static bool battery_pending;
static bool battery_stopped;
static uint32_t battery_queued_age;
static int64_t battery_queued_at;

static uint32_t battery_age(uint32_t age, int64_t since) {
    uint64_t elapsed = MAX(k_uptime_get() - since, 0);

    return MIN((uint64_t)age + elapsed, UINT32_MAX);
}

/* The event for the waiting sample, if there is one and the buffer is free. Called locked. */
static struct net_buf *take_locked(void) {
    if (!battery_pending || battery_stopped) {
        return NULL;
    }

    struct net_buf *buf = net_buf_alloc(&battery_pool, K_NO_WAIT);

    if (buf == NULL) {
        return NULL;
    }
    net_buf_reserve(buf, BT_BUF_RESERVE);
    bt_buf_set_type(buf, BT_BUF_EVT);
    net_buf_add_u8(buf, BT_HCI_EVT_VENDOR);
    net_buf_add_u8(buf, DEFY_BATTERY_HCI_PAYLOAD_SIZE);
    uint8_t *payload = net_buf_add(buf, DEFY_BATTERY_HCI_PAYLOAD_SIZE);
    struct defy_battery_sample sample = battery_latest;

    battery_queued_age = battery_latest.age_ms;
    battery_queued_at = battery_latest_at;
    sample.age_ms = battery_age(battery_queued_age, battery_queued_at);
    sys_put_le16(DEFY_HCI_SPI_EVT_BATTERY, payload);
    defy_battery_hci_encode(payload, &sample);
    battery_pending = false;
    return buf;
}

static void queue_waiting(void) {
    k_spinlock_key_t key = k_spin_lock(&battery_lock);
    struct net_buf *buf = take_locked();

    k_spin_unlock(&battery_lock, key);
    if (buf != NULL) {
        defy_radio_queue_for_host(buf, false);
    }
}

int defy_radio_battery_publish(const struct defy_battery_sample *sample) {
    if (!defy_battery_hci_sample_valid(sample)) {
        return -EINVAL;
    }

    k_spinlock_key_t key = k_spin_lock(&battery_lock);

    if (battery_stopped) {
        k_spin_unlock(&battery_lock, key);
        return -ESHUTDOWN;
    }
    battery_latest = *sample;
    battery_latest_at = k_uptime_get();
    battery_pending = true;
    k_spin_unlock(&battery_lock, key);
    queue_waiting();
    return 0;
}

void defy_radio_battery_hci_sent(void) { queue_waiting(); }

void defy_radio_battery_hci_power_off(void) {
    k_spinlock_key_t key = k_spin_lock(&battery_lock);

    battery_stopped = true;
    battery_pending = false;
    k_spin_unlock(&battery_lock, key);
}

void defy_radio_battery_hci_arm(const struct net_buf *buf, uint8_t *message) {
    if (net_buf_pool_get(buf->pool_id) != &battery_pool) {
        return;
    }

    k_spinlock_key_t key = k_spin_lock(&battery_lock);
    uint32_t age = battery_age(battery_queued_age, battery_queued_at);

    k_spin_unlock(&battery_lock, key);
    defy_battery_hci_set_age(&message[1 + sizeof(struct bt_hci_evt_hdr)], age);
}

uint8_t defy_radio_battery_hci_refresh(void) {
    k_spinlock_key_t key = k_spin_lock(&battery_lock);
    bool stopped = battery_stopped;

    k_spin_unlock(&battery_lock, key);
    return !stopped && defy_radio_battery_refresh() == 0 ? BT_HCI_ERR_SUCCESS
                                                         : BT_HCI_ERR_CMD_DISALLOWED;
}
