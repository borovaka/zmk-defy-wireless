/*
 * The RP2040's copy of the radio's latest battery sample, from its vendor
 * events, and refresh requests to the radio.
 *
 * SPDX-License-Identifier: MIT
 */

#include <errno.h>

#include <zephyr/bluetooth/hci.h>
#include <zephyr/init.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/byteorder.h>

#include <dygma/defy_battery.h>
#include <dygma/defy_battery_hci.h>
#include <dygma/defy_half_leds.h>
#include <dygma/defy_hci_spi.h>

static struct k_spinlock battery_lock;
static struct defy_battery_sample cached_sample;
static int64_t received_at;
static bool have_sample;
static uint32_t radio_generation = 1;

int defy_battery_hci_refresh(void) { return defy_hci_spi_battery_refresh(); }

void defy_battery_hci_reset(void) {
    k_spinlock_key_t key = k_spin_lock(&battery_lock);
    have_sample = false;
    if (++radio_generation == 0) {
        radio_generation = 1;
    }
    k_spin_unlock(&battery_lock, key);
    defy_half_leds_battery_updated();
}

/* Takes the battery event, and leaves every other vendor event to the host. */
static bool battery_event(struct net_buf_simple *buf) {
    struct defy_battery_sample sample;

    if (buf->len < 2 || sys_get_le16(buf->data) != DEFY_HCI_SPI_EVT_BATTERY) {
        return false;
    }
    if (!defy_battery_hci_decode(buf->data, buf->len, &sample)) {
        return true;
    }

    k_spinlock_key_t key = k_spin_lock(&battery_lock);
    bool updated = !have_sample || (int32_t)(sample.sequence - cached_sample.sequence) > 0;
    if (updated) {
        cached_sample = sample;
        received_at = k_uptime_get();
        have_sample = true;
    }
    k_spin_unlock(&battery_lock, key);
    if (updated) {
        defy_half_leds_battery_updated();
    }
    return true;
}

int defy_battery_hci_get_generation(struct defy_battery_sample *sample, uint32_t *generation) {
    if (sample == NULL || generation == NULL) {
        return -EINVAL;
    }

    k_spinlock_key_t key = k_spin_lock(&battery_lock);
    if (!have_sample) {
        k_spin_unlock(&battery_lock, key);
        return -ENODATA;
    }
    *sample = cached_sample;
    *generation = radio_generation;
    uint64_t elapsed = MAX(k_uptime_get() - received_at, 0);
    sample->age_ms = MIN((uint64_t)sample->age_ms + elapsed, UINT32_MAX);
    k_spin_unlock(&battery_lock, key);
    return 0;
}

int defy_battery_hci_get(struct defy_battery_sample *sample) {
    uint32_t generation;

    return defy_battery_hci_get_generation(sample, &generation);
}

static int battery_hci_init(void) {
    return bt_hci_register_vnd_evt_cb(battery_event);
}

/* Before ZMK's bt_enable(), at APPLICATION. */
SYS_INIT(battery_hci_init, POST_KERNEL, CONFIG_KERNEL_INIT_PRIORITY_DEFAULT);
