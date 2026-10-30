/* SPDX-License-Identifier: MIT */

#include <string.h>
#include <zephyr/ztest.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/drivers/gpio/gpio_emul.h>
#include <zephyr/init.h>
#include <zmk/usb.h>
#include <dygma/defy_update_guard.h>

static int test_configure(const struct gpio_dt_spec *spec, gpio_flags_t flags);
static int test_get(const struct gpio_dt_spec *spec);
static int test_schedule(struct k_work_delayable *work, k_timeout_t delay);

#undef SYS_INIT
#define SYS_INIT(...)
#define gpio_pin_configure_dt test_configure
#define gpio_pin_get_dt test_get
#define k_work_schedule test_schedule
#include "../../../../status/defy_neuron_dock.c"
#undef gpio_pin_configure_dt
#undef gpio_pin_get_dt
#undef k_work_schedule

#define LEFT DEFY_NEURON_CABLE_LEFT
#define RIGHT DEFY_NEURON_CABLE_RIGHT
#define LEFT_PIN 29
#define RIGHT_PIN 28

static const struct device *const port = DEVICE_DT_GET(DT_NODELABEL(gpio0));
static enum zmk_usb_conn_state usb;
static int configure_error[2];
static unsigned int configures, reads, schedules;
static int64_t scheduled_ms;

enum zmk_usb_conn_state zmk_usb_get_conn_state(void) { return usb; }

static int test_configure(const struct gpio_dt_spec *spec, gpio_flags_t flags) {
    size_t line = spec - lines;
    zassert_true(line < ARRAY_SIZE(lines));
    zassert_equal(flags, GPIO_INPUT);
    configures++;
    return configure_error[line] ? configure_error[line] : gpio_pin_configure_dt(spec, flags);
}

static int test_get(const struct gpio_dt_spec *spec) {
    reads++;
    return gpio_pin_get_dt(spec);
}

static int test_schedule(struct k_work_delayable *work, k_timeout_t delay) {
    zassert_equal_ptr(work, &sample_work);
    schedules++;
    scheduled_ms = k_ticks_to_ms_floor64(delay.ticks);
    return 1;
}

static void level(uint8_t mask) {
    zassert_ok(gpio_emul_input_set(port, LEFT_PIN, mask & LEFT));
    zassert_ok(gpio_emul_input_set(port, RIGHT_PIN, mask & RIGHT));
}

static void tick(void) {
    sample(&sample_work.work);
    zassert_equal(scheduled_ms, SAMPLE_MS);
}

static void start(void) {
    zassert_ok(dock_init());
    level(0);
}

/* Two equal samples, so mask becomes the accepted cables. */
static void settle(uint8_t mask) {
    level(mask);
    tick();
    tick();
    zassert_equal(defy_neuron_cables(), mask);
}

static void snapshot(uint8_t out[DEFY_NEURON_DOCK_STATUS_SIZE]) {
    zassert_equal(defy_neuron_dock_status(out, DEFY_NEURON_DOCK_STATUS_SIZE),
                  DEFY_NEURON_DOCK_STATUS_SIZE);
}

static void before(void *fixture) {
    ARG_UNUSED(fixture);
    defy_update_release(DEFY_UPDATE_HALVES);
    defy_update_release(DEFY_UPDATE_NEURON);
    atomic_clear(&status);
    sampled = false;
    usb = ZMK_USB_CONN_NONE;
    configure_error[0] = configure_error[1] = 0;
    configures = reads = schedules = 0;
    scheduled_ms = -1;
    zassert_ok(gpio_pin_configure(port, LEFT_PIN, GPIO_DISCONNECTED));
    zassert_ok(gpio_pin_configure(port, RIGHT_PIN, GPIO_DISCONNECTED));
}

ZTEST(neuron_dock, test_init_makes_both_lines_inputs_without_pull) {
    start();
    zassert_equal(configures, 2);
    for (gpio_pin_t pin = RIGHT_PIN; pin <= LEFT_PIN; pin++) {
        gpio_flags_t flags;
        zassert_ok(gpio_emul_flags_get(port, pin, &flags));
        zassert_equal(flags, GPIO_INPUT, "pin %u", pin);
    }
    zassert_equal(schedules, 1);
    zassert_equal(scheduled_ms, 0);
    zassert_equal(reads, 0);
    zassert_equal(defy_neuron_cables(), 0);
}

ZTEST(neuron_dock, test_configuration_failure_never_samples) {
    for (size_t line = 0; line < 2; line++) {
        before(NULL);
        configure_error[line] = -EIO;
        zassert_ok(dock_init());
        zassert_equal(configures, line + 1);
        zassert_equal(schedules, 0);
        uint8_t out[DEFY_NEURON_DOCK_STATUS_SIZE];
        snapshot(out);
        zassert_equal(out[3], 0);
        zassert_equal(defy_neuron_cables(), 0);
    }
}

ZTEST(neuron_dock, test_level_counts_after_two_equal_samples) {
    start();
    level(LEFT);
    tick();
    zassert_equal(defy_neuron_cables(), 0);
    tick();
    zassert_equal(defy_neuron_cables(), LEFT);
    level(LEFT | RIGHT);
    tick();
    zassert_equal(defy_neuron_cables(), LEFT);
    tick();
    zassert_equal(defy_neuron_cables(), LEFT | RIGHT);
    level(0);
    tick();
    zassert_equal(defy_neuron_cables(), LEFT | RIGHT);
    tick();
    zassert_equal(defy_neuron_cables(), 0);
    zassert_equal(reads, 12);
    zassert_equal(configures, 2);
}

ZTEST(neuron_dock, test_flickering_line_never_counts_and_steady_one_still_does) {
    start();
    settle(LEFT);
    for (int i = 0; i < 6; i++) {
        level(i % 2 ? LEFT : RIGHT);
        tick();
        zassert_equal(defy_neuron_cables(), LEFT);
    }
    for (int i = 0; i < 6; i++) {
        level(RIGHT | (i % 2 ? LEFT : 0));
        tick();
        zassert_equal(defy_neuron_cables() & RIGHT, i ? RIGHT : 0);
        zassert_equal(defy_neuron_cables() & LEFT, LEFT);
    }
}

ZTEST(neuron_dock, test_half_updater_hold_keeps_last_value_without_reading) {
    start();
    settle(LEFT);
    zassert_true(defy_update_claim(DEFY_UPDATE_HALVES));
    zassert_ok(gpio_pin_configure(port, LEFT_PIN, GPIO_OUTPUT_LOW));
    zassert_ok(gpio_pin_configure(port, RIGHT_PIN, GPIO_OUTPUT_LOW));
    unsigned int before_hold = reads;
    tick();
    tick();
    zassert_equal(reads, before_hold);
    zassert_equal(defy_neuron_cables(), LEFT);
    uint8_t out[DEFY_NEURON_DOCK_STATUS_SIZE];
    snapshot(out);
    zassert_equal(out[3], BIT(0) | BIT(1));
    zassert_equal(out[1], LEFT);

    zassert_ok(gpio_pin_configure(port, LEFT_PIN, GPIO_INPUT));
    zassert_ok(gpio_pin_configure(port, RIGHT_PIN, GPIO_INPUT));
    defy_update_release(DEFY_UPDATE_HALVES);
    /* A sample from before the hold does not pair with one after it. */
    level(0);
    tick();
    zassert_equal(defy_neuron_cables(), LEFT);
    snapshot(out);
    zassert_equal(out[3], BIT(0));
    tick();
    zassert_equal(defy_neuron_cables(), 0);
    zassert_equal(configures, 2);
}

ZTEST(neuron_dock, test_neuron_update_does_not_hold_the_lines) {
    start();
    zassert_true(defy_update_claim(DEFY_UPDATE_NEURON));
    settle(LEFT | RIGHT);
    zassert_equal(reads, 4);
}

ZTEST(neuron_dock, test_place_for_every_line_and_usb_state) {
    static const enum defy_neuron_place expected[4][3] = {
        [0] = {DEFY_NEURON_ALONE, DEFY_NEURON_CHARGER, DEFY_NEURON_COMPUTER},
        [LEFT] = {DEFY_NEURON_DOCKED, DEFY_NEURON_DOCKED, DEFY_NEURON_COMPUTER},
        [RIGHT] = {DEFY_NEURON_DOCKED, DEFY_NEURON_DOCKED, DEFY_NEURON_COMPUTER},
        [LEFT | RIGHT] = {DEFY_NEURON_ALONE, DEFY_NEURON_CHARGER, DEFY_NEURON_COMPUTER},
    };
    start();
    for (uint8_t cables = 0; cables < 4; cables++) {
        settle(cables);
        for (int state = ZMK_USB_CONN_NONE; state <= ZMK_USB_CONN_HID; state++) {
            usb = state;
            zassert_equal(defy_neuron_place(), expected[cables][state], "cables %u usb %d",
                          cables, state);
            uint8_t out[DEFY_NEURON_DOCK_STATUS_SIZE];
            snapshot(out);
            zassert_equal(out[4], state);
            zassert_equal(out[5], expected[cables][state]);
        }
    }
}

ZTEST(neuron_dock, test_snapshot_reports_raw_and_debounced_lines) {
    start();
    usb = ZMK_USB_CONN_POWERED;
    settle(LEFT);
    level(LEFT | RIGHT);
    tick();
    uint8_t out[DEFY_NEURON_DOCK_STATUS_SIZE + 1];
    memset(out, 0xaa, sizeof(out));
    snapshot(out);
    const uint8_t expected[] = {1, LEFT | RIGHT, LEFT, BIT(0), ZMK_USB_CONN_POWERED,
                                DEFY_NEURON_DOCKED, 0, 0, 0xaa};
    zassert_mem_equal(out, expected, sizeof(expected));
    zassert_equal(defy_neuron_dock_status(out, DEFY_NEURON_DOCK_STATUS_SIZE - 1), 0);
    zassert_equal(defy_neuron_dock_status(NULL, sizeof(out)), 0);
}

ZTEST(neuron_dock, test_nothing_is_reported_before_init) {
    usb = ZMK_USB_CONN_POWERED;
    zassert_equal(defy_neuron_cables(), 0);
    zassert_equal(defy_neuron_place(), DEFY_NEURON_CHARGER);
    uint8_t out[DEFY_NEURON_DOCK_STATUS_SIZE];
    snapshot(out);
    const uint8_t expected[] = {1, 0, 0, 0, ZMK_USB_CONN_POWERED, DEFY_NEURON_CHARGER, 0, 0};
    zassert_mem_equal(out, expected, sizeof(expected));
}

ZTEST_SUITE(neuron_dock, NULL, NULL, before, NULL, NULL);
