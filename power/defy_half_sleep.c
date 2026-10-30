/* SPDX-License-Identifier: MIT */

#include <stdbool.h>
#include <errno.h>

#include <zephyr/device.h>
#include <zephyr/init.h>
#include <zephyr/kernel.h>
#include <zephyr/pm/device.h>
#include <zephyr/pm/device_runtime.h>
#include <zephyr/sys/reboot.h>

#include <dygma/defy_battery.h>
#include <dygma/defy_battery_hci.h>
#include <dygma/defy_half_leds.h>
#include <dygma/defy_half_power.h>
#include <dygma/defy_hybrid.h>
#include <zmk/pm.h>

#define POLL_MS 1000
/* The poll while Bluetooth stops for sleep. */
#define STOPPING_POLL_MS 20

static const struct device *const matrix = DEVICE_DT_GET(DT_NODELABEL(kscan0));
static const struct device *const outputs[] = {
    DEVICE_DT_GET(DT_CHOSEN(zephyr_bt_hci)),
#if defined(CONFIG_DEFY_LEDS) && DT_NODE_HAS_STATUS_OKAY(DT_NODELABEL(defy_leds))
    DEVICE_DT_GET(DT_NODELABEL(defy_leds)),
#endif
#if defined(CONFIG_IS31FL3743B) && DT_NODE_HAS_STATUS_OKAY(DT_NODELABEL(key_leds))
    DEVICE_DT_GET(DT_NODELABEL(key_leds)),
#endif
};

static bool has_state(const struct device *device, enum pm_device_state expected) {
    enum pm_device_state actual;
    return device_is_ready(device) && pm_device_state_get(device, &actual) == 0 &&
           actual == expected;
}

static bool matrix_ready(void) {
    return has_state(matrix, PM_DEVICE_STATE_ACTIVE) &&
           pm_device_wakeup_is_enabled(matrix) && !pm_device_runtime_is_enabled(matrix);
}

static bool outputs_ready(void) {
    for (size_t i = 0; i < ARRAY_SIZE(outputs); i++) {
        if (!has_state(outputs[i], PM_DEVICE_STATE_ACTIVE) || pm_device_is_busy(outputs[i]) ||
            pm_device_wakeup_is_enabled(outputs[i]) || pm_device_runtime_is_enabled(outputs[i])) {
            return false;
        }
    }
    return true;
}

static bool outputs_suspended(void) {
    for (size_t i = 0; i < ARRAY_SIZE(outputs); i++) {
        if (!has_state(outputs[i], PM_DEVICE_STATE_SUSPENDED)) {
            return false;
        }
    }
    return true;
}

/* The radio services the charger only while the half is awake. Asleep, the
 * charger's own watchdog would end the charge within about 30 s. A stale
 * sample, such as from a radio that stopped, does not hold the half awake. */
static bool charger_active(void) {
    struct defy_battery_sample sample;
    struct defy_battery_charger charger;

    return IS_ENABLED(CONFIG_DEFY_BATTERY_HCI) && defy_battery_hci_get(&sample) == 0 &&
           defy_battery_sample_charger(&sample, &charger) == 0 && charger.status_valid &&
           (charger.state == DEFY_BATTERY_CHARGE_CHARGING ||
            charger.state == DEFY_BATTERY_CHARGE_DONE);
}

static void sleep_poll(struct k_work *work);
static K_WORK_DELAYABLE_DEFINE(sleep_work, sleep_poll);

static void schedule_poll(void) { k_work_reschedule(&sleep_work, K_MSEC(POLL_MS)); }

static void sleep_poll(struct k_work *work) {
    ARG_UNUSED(work);
    if (defy_half_leds_battery_active() || charger_active()) {
        defy_hybrid_peripheral_cancel_sleep();
        schedule_poll();
        return;
    }
    int err = defy_hybrid_peripheral_prepare_sleep(CONFIG_DEFY_HALF_SLEEP_TIMEOUT_MS);

    if (err < 0) {
        k_work_reschedule(&sleep_work, K_MSEC(err == -EINPROGRESS ? STOPPING_POLL_MS : POLL_MS));
        return;
    }

    if (defy_half_power_prepare() < 0) {
        defy_hybrid_peripheral_cancel_sleep();
        schedule_poll();
        return;
    }

    if (!matrix_ready() || !outputs_ready() || defy_half_power_pending()) {
        /* Without its callback the matrix reads no keys. */
        if (defy_half_power_cancel() < 0) {
            sys_reboot(SYS_REBOOT_COLD);
        }
        defy_hybrid_peripheral_cancel_sleep();
        schedule_poll();
        return;
    }

    /* From here a failure restarts the half cold rather than leave it half stopped. */
    if (zmk_pm_suspend_devices() < 0 || !outputs_suspended() || !matrix_ready() ||
        defy_half_power_pending()) {
        sys_reboot(SYS_REBOOT_COLD);
    }

    defy_half_power_enter();
}

static int sleep_init(void) {
    schedule_poll();
    return 0;
}

SYS_INIT(sleep_init, APPLICATION, 90);
