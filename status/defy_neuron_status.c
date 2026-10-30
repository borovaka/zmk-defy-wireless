/*
 * The Neuron's status LED (docs/lighting.md): green while a USB host has
 * configured HID and is not suspended, amber blinking while the half updater
 * holds the halves. PWM sets the brightness, from
 * CONFIG_DEFY_NEURON_STATUS_LED_BRIGHTNESS.
 *
 * SPDX-License-Identifier: MIT
 */

#include <zephyr/device.h>
#include <zephyr/drivers/pwm.h>
#include <zephyr/init.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/atomic.h>

#include <dygma/defy_hybrid.h>
#include <dygma/defy_neuron_status.h>
#include <zmk/event_manager.h>
#include <zmk/events/usb_conn_state_changed.h>
#include <zmk/usb.h>

/* Amber's half period while the halves are held. */
#define BLINK_MS 500

static const struct pwm_dt_spec green_led = PWM_DT_SPEC_GET(DT_NODELABEL(led_green));
static const struct pwm_dt_spec red_led = PWM_DT_SPEC_GET(DT_NODELABEL(led_red));
static atomic_t initialized;

static int set_led(const struct pwm_dt_spec *led, bool on) {
    uint32_t pulse =
        on ? (uint32_t)((uint64_t)led->period * CONFIG_DEFY_NEURON_STATUS_LED_BRIGHTNESS / 100) : 0;
    return pwm_set_pulse_dt(led, pulse);
}

static void update_status(struct k_work *work);
static K_WORK_DELAYABLE_DEFINE(status_work, update_status);

static void update_status(struct k_work *work) {
    ARG_UNUSED(work);
    if (!atomic_get(&initialized)) {
        return;
    }
    bool red = false;
    bool green;
    if (IS_ENABLED(CONFIG_DEFY_HYBRID) && defy_hybrid_central_is_maintenance()) {
        /* Typing is paused; the phase follows the clock, so a USB event between blinks keeps it. */
        int64_t now = k_uptime_get();
        red = green = (now / BLINK_MS) % 2 == 0;
        k_work_schedule(&status_work, K_MSEC(BLINK_MS - now % BLINK_MS));
    } else {
        green = zmk_usb_is_hid_ready() && zmk_usb_get_status() != USB_DC_SUSPEND;
    }
    set_led(&red_led, red);
    set_led(&green_led, green);
}

void defy_neuron_status_update(void) { k_work_reschedule(&status_work, K_NO_WAIT); }

static int usb_changed(const zmk_event_t *event) {
    ARG_UNUSED(event);
    defy_neuron_status_update();
    return ZMK_EV_EVENT_BUBBLE;
}

static int status_init(void) {
    if (!pwm_is_ready_dt(&green_led) || !pwm_is_ready_dt(&red_led)) {
        return 0;
    }
    int red_error = set_led(&red_led, false);
    int green_error = set_led(&green_led, false);
    if (!red_error && !green_error) {
        atomic_set(&initialized, 1);
        defy_neuron_status_update();
    }
    return 0;
}

ZMK_LISTENER(defy_neuron_status, usb_changed);
ZMK_SUBSCRIPTION(defy_neuron_status, zmk_usb_conn_state_changed);
SYS_INIT(status_init, APPLICATION, 50);
