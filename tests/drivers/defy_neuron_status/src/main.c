/* SPDX-License-Identifier: MIT */

#include <zephyr/ztest.h>
#include <zephyr/device.h>
#include <zephyr/drivers/pwm.h>
#include <zephyr/init.h>
#include <zephyr/logging/log.h>
#include <zmk/event_manager.h>
#include <zmk/events/usb_conn_state_changed.h>
#include <zmk/usb.h>

static bool test_pwm_ready(const struct pwm_dt_spec *spec);
static int test_pwm_set(const struct pwm_dt_spec *spec, uint32_t pulse);

#undef SYS_INIT
#define SYS_INIT(...)
#include "../../../../../zmk-workspace/zmk/app/src/usb.c"
#include "fake_uptime.h"
#define pwm_is_ready_dt test_pwm_ready
#define pwm_set_pulse_dt test_pwm_set
#include "../../../../status/defy_neuron_status.c"
#undef pwm_is_ready_dt
#undef pwm_set_pulse_dt
#undef k_uptime_get

DEVICE_DT_DEFINE(DT_NODELABEL(test_pwm), NULL, NULL, NULL, NULL, POST_KERNEL,
                 CONFIG_KERNEL_INIT_PRIORITY_DEVICE, NULL);

/* The pulse that lights a channel: a tenth of the 100 us period. */
#define LIT 10000u

static bool green_ready;
static bool red_ready;
static int red_init_error;
static int64_t green_pulse;
static int64_t red_pulse;
static unsigned int green_writes;
static unsigned int red_writes;
static usb_dc_status_callback installed_callback;
static bool suspend_during_write;
static bool maintenance;

bool defy_hybrid_central_is_maintenance(void) { return maintenance; }

int usb_enable(usb_dc_status_callback callback) {
    installed_callback = callback;
    return 0;
}

static void flush_events(void) {
    for (int i = 0; i < 2; ++i) {
        struct k_work_sync usb_sync;
        struct k_work_sync led_sync;
        k_work_flush(&usb_status_notifier_work, &usb_sync);
        k_work_flush_delayable(&status_work, &led_sync);
    }
}

static bool test_pwm_ready(const struct pwm_dt_spec *spec) {
    zassert_true(spec == &green_led || spec == &red_led);
    return spec == &green_led ? green_ready : red_ready;
}

static int test_pwm_set(const struct pwm_dt_spec *spec, uint32_t pulse) {
    zassert_equal(spec->period, PWM_USEC(100));
    zassert_equal(spec->flags, PWM_POLARITY_NORMAL);
    zassert_true(pulse == 0 || pulse == LIT);
    bool initializing = !atomic_get(&initialized);
    if (initializing) {
        zassert_equal(pulse, 0);
    }
    if (spec == &red_led) {
        zassert_equal(spec->channel, 1);
        ++red_writes;
        if (initializing && red_init_error) {
            return red_init_error;
        }
        red_pulse = pulse;
        return 0;
    }
    zassert_equal_ptr(spec, &green_led);
    zassert_equal(spec->channel, 0);
    ++green_writes;
    if (suspend_during_write) {
        suspend_during_write = false;
        installed_callback(USB_DC_SUSPEND, NULL);
    }
    green_pulse = pulse;
    return 0;
}

static void status(enum usb_dc_status_code code) {
    installed_callback(code, NULL);
    flush_events();
}

static void start_indicator(void) {
    zassert_ok(status_init());
    flush_events();
}

static void hold_halves(bool held) {
    maintenance = held;
    defy_neuron_status_update();
    flush_events();
}

static void before(void *fixture) {
    ARG_UNUSED(fixture);
    flush_events();
    atomic_clear(&initialized);
    usb_status = USB_DC_UNKNOWN;
    is_configured = false;
    now = 0;
    maintenance = false;
    green_ready = red_ready = true;
    red_init_error = 0;
    green_pulse = red_pulse = -1;
    green_writes = red_writes = 0;
    suspend_during_write = false;
    installed_callback = NULL;
    zassert_ok(zmk_usb_init());
    zassert_not_null(installed_callback);
}

static void after(void *fixture) {
    ARG_UNUSED(fixture);
    struct k_work_sync sync;
    maintenance = false;
    k_work_cancel_delayable_sync(&status_work, &sync);
}

ZTEST(neuron_status, test_startup_is_dark_before_enumeration) {
    start_indicator();
    zassert_equal(green_pulse, 0);
    zassert_equal(red_pulse, 0);
    status(USB_DC_CONNECTED);
    zassert_equal(green_pulse, 0);
}

ZTEST(neuron_status, test_configured_hid_is_dimmed_green) {
    start_indicator();
    status(USB_DC_CONFIGURED);
    zassert_true(zmk_usb_is_hid_ready());
    zassert_equal(green_pulse, LIT);
    zassert_equal(red_pulse, 0);
}

ZTEST(neuron_status, test_suspend_is_dark_even_while_hid_ready) {
    start_indicator();
    status(USB_DC_CONFIGURED);
    status(USB_DC_SUSPEND);
    zassert_true(zmk_usb_is_hid_ready());
    zassert_equal(green_pulse, 0);
    status(USB_DC_RESUME);
    zassert_equal(green_pulse, LIT);
}

ZTEST(neuron_status, test_stale_event_payload_does_not_light_suspended_usb) {
    start_indicator();
    status(USB_DC_CONFIGURED);
    status(USB_DC_SUSPEND);
    zassert_ok(raise_zmk_usb_conn_state_changed(
        (struct zmk_usb_conn_state_changed){.conn_state = ZMK_USB_CONN_HID}));
    flush_events();
    zassert_equal(green_pulse, 0);
}

ZTEST(neuron_status, test_preinit_event_is_reconciled_by_init) {
    status(USB_DC_CONFIGURED);
    zassert_equal(green_writes, 0);
    start_indicator();
    zassert_equal(green_pulse, LIT);
}

ZTEST(neuron_status, test_event_during_render_gets_latest_state) {
    start_indicator();
    suspend_during_write = true;
    status(USB_DC_CONFIGURED);
    zassert_equal(zmk_usb_get_status(), USB_DC_SUSPEND);
    zassert_equal(green_pulse, 0);
}

ZTEST(neuron_status, test_unready_device_is_nonfatal_and_never_rendered) {
    green_ready = false;
    start_indicator();
    status(USB_DC_CONFIGURED);
    zassert_false(atomic_get(&initialized));
    zassert_equal(green_writes, 0);
    zassert_equal(red_writes, 0);
}

ZTEST(neuron_status, test_red_setup_failure_disables_indicator) {
    red_init_error = -EIO;
    start_indicator();
    status(USB_DC_CONFIGURED);
    hold_halves(true);
    zassert_false(atomic_get(&initialized));
    zassert_equal(green_pulse, 0);
}

ZTEST(neuron_status, test_held_halves_blink_amber_then_green_returns) {
    start_indicator();
    status(USB_DC_CONFIGURED);
    hold_halves(true);
    zassert_equal(red_pulse, LIT);
    zassert_equal(green_pulse, LIT);
    now = BLINK_MS;
    flush_events();
    zassert_equal(red_pulse, 0);
    zassert_equal(green_pulse, 0);
    now = 2 * BLINK_MS + 1;
    flush_events();
    zassert_equal(red_pulse, LIT);
    zassert_equal(green_pulse, LIT);
    hold_halves(false);
    zassert_equal(red_pulse, 0);
    zassert_equal(green_pulse, LIT);
}

ZTEST(neuron_status, test_usb_event_keeps_the_blink_phase) {
    start_indicator();
    status(USB_DC_CONFIGURED);
    now = BLINK_MS + 100;
    hold_halves(true);
    zassert_equal(red_pulse, 0);
    now = BLINK_MS + 300;
    status(USB_DC_RESUME);
    zassert_equal(red_pulse, 0);
    zassert_equal(green_pulse, 0);
}

ZTEST(neuron_status, test_held_halves_blink_without_usb) {
    start_indicator();
    hold_halves(true);
    zassert_equal(red_pulse, LIT);
    zassert_equal(green_pulse, LIT);
    hold_halves(false);
    zassert_equal(red_pulse, 0);
    zassert_equal(green_pulse, 0);
}

ZTEST_SUITE(neuron_status, NULL, NULL, before, after, NULL);
