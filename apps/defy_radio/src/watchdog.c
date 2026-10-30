/*
 * The radio's watchdogs.
 *
 * Nothing in this firmware or in rf_bootloader starts the nRF52820's hardware
 * watchdog. Dygma's radio application does, and only a power-on, pin or
 * watchdog reset stops it again: a soft reset and the bootloader's jump into
 * this firmware leave it running, with the timeout that application chose.
 * So whenever it runs, every enabled reload channel is fed: from the reset
 * hook in startup.c, at EARLY init, and from a timer every 125 ms.
 *
 * That timer is also a software watchdog, with or without the hardware one.
 * The link thread must report progress within 5 s of the start and then at
 * least once a second. Once it misses that, feeding stops and the radio
 * reboots.
 *
 * SPDX-License-Identifier: MIT
 */

#include <hal/nrf_wdt.h>
#include <zephyr/init.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/reboot.h>

#include "watchdog.h"

#define STARTUP_GRACE_MS 5000U
#define LINK_TIMEOUT_MS 1000U
#define FEED_INTERVAL_MS 125

static struct k_spinlock watchdog_lock;
static uint32_t watchdog_last_progress;
static uint32_t watchdog_timeout;
static bool watchdog_expired;

void defy_radio_watchdog_feed(void) {
    if (!nrf_wdt_started_check(NRF_WDT)) {
        return;
    }

    for (unsigned int channel = 0; channel < NRF_WDT_CHANNEL_NUMBER; channel++) {
        nrf_wdt_rr_register_t rr = (nrf_wdt_rr_register_t)channel;

        if (nrf_wdt_reload_request_enable_check(NRF_WDT, rr)) {
            nrf_wdt_reload_request_set(NRF_WDT, rr);
        }
    }
}

static bool watchdog_fresh_locked(uint32_t now) {
    if ((uint32_t)(now - watchdog_last_progress) >= watchdog_timeout) {
        watchdog_expired = true;
    }
    return !watchdog_expired;
}

void defy_radio_watchdog_heartbeat(void) {
    k_spinlock_key_t key = k_spin_lock(&watchdog_lock);

    if (watchdog_timeout != 0) {
        uint32_t now = k_uptime_get_32();

        if (watchdog_fresh_locked(now)) {
            watchdog_last_progress = now;
            watchdog_timeout = LINK_TIMEOUT_MS;
        }
    }
    k_spin_unlock(&watchdog_lock, key);
}

static int watchdog_early_feed(void) {
    defy_radio_watchdog_feed();
    return 0;
}

static void watchdog_timer_expired(struct k_timer *timer) {
    ARG_UNUSED(timer);

    k_spinlock_key_t key = k_spin_lock(&watchdog_lock);
    bool fresh = watchdog_fresh_locked(k_uptime_get_32());

    k_spin_unlock(&watchdog_lock, key);
    if (!fresh) {
        sys_reboot(SYS_REBOOT_COLD);
    }
    defy_radio_watchdog_feed();
}

K_TIMER_DEFINE(watchdog_timer, watchdog_timer_expired, NULL);

static int watchdog_timer_start(void) {
    k_spinlock_key_t key = k_spin_lock(&watchdog_lock);

    watchdog_last_progress = k_uptime_get_32();
    watchdog_timeout = STARTUP_GRACE_MS;
    k_spin_unlock(&watchdog_lock, key);
    defy_radio_watchdog_feed();
    k_timer_start(&watchdog_timer, K_MSEC(FEED_INTERVAL_MS), K_MSEC(FEED_INTERVAL_MS));
    return 0;
}

SYS_INIT(watchdog_early_feed, EARLY, 0);
SYS_INIT(watchdog_timer_start, POST_KERNEL, 0);
