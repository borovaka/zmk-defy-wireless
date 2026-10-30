/*
 * The Defy's joined LED strip over two fake strips: per-key LEDs, then the
 * underglow, whose supply is an emulated GPIO.
 *
 * SPDX-License-Identifier: MIT
 */

#include <string.h>

#include <zephyr/device.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/drivers/gpio/gpio_emul.h>
#include <zephyr/drivers/led_strip.h>
#include <zephyr/pm/device.h>
#include <zephyr/ztest.h>

#include "fake_led_strip.h"

#define LEDS           DT_NODELABEL(leds)
#define NUM_LEDS       DT_PROP(LEDS, chain_length)
#define NUM_KEYS       DT_PROP(DT_NODELABEL(fake_keys), chain_length)
#define NUM_UNDERGLOW  DT_PROP(DT_NODELABEL(fake_underglow), chain_length)
#define FIRST_UNDERGLOW NUM_KEYS

static const struct device *const leds = DEVICE_DT_GET(LEDS);
static const struct device *const keys = DEVICE_DT_GET(DT_NODELABEL(fake_keys));
static const struct device *const underglow = DEVICE_DT_GET(DT_NODELABEL(fake_underglow));
static const struct gpio_dt_spec power = GPIO_DT_SPEC_GET(LEDS, underglow_power_gpios);

static struct led_rgb pixels[NUM_LEDS];
static int power_at_boot;
static unsigned int updates_at_boot;
static int power_when_sent;

static struct fake_led_strip_data *fake(const struct device *dev) { return dev->data; }

static int power_level(void) { return gpio_emul_output_get(power.port, power.pin); }

static void record_power(const struct device *dev) {
    ARG_UNUSED(dev);
    power_when_sent = power_level();
}

static void reset_fake(const struct device *dev) {
    struct fake_led_strip_data *data = fake(dev);

    memset(data->frame, 0, sizeof(data->frame));
    data->count = 0;
    data->updates = 0;
}

static void *suite_setup(void) {
    power_at_boot = power_level();
    updates_at_boot = fake(keys)->updates + fake(underglow)->updates;
    fake(underglow)->on_update = record_power;
    return NULL;
}

/* Each test starts resumed and dark, with fresh fakes. */
static void before(void *fixture) {
    ARG_UNUSED(fixture);
    (void)pm_device_action_run(leds, PM_DEVICE_ACTION_RESUME);
    memset(pixels, 0, sizeof(pixels));
    zassert_ok(led_strip_update_rgb(leds, pixels, NUM_LEDS));
    reset_fake(keys);
    reset_fake(underglow);
    power_when_sent = -1;
}

ZTEST(defy_leds, test_boot_leaves_underglow_unpowered) {
    zassert_equal(power_at_boot, 0, "underglow powered at boot");
    zassert_equal(updates_at_boot, 0, "frames sent at boot");
}

ZTEST(defy_leds, test_pixels_split_between_parts) {
    for (int i = 0; i < NUM_LEDS; i++) {
        pixels[i] = (struct led_rgb){.r = i + 1, .g = 0x40 + i, .b = 0x80 + i};
    }
    zassert_ok(led_strip_update_rgb(leds, pixels, NUM_LEDS));

    zassert_equal(fake(keys)->count, NUM_KEYS);
    zassert_mem_equal(fake(keys)->frame, pixels, NUM_KEYS * sizeof(pixels[0]));
    zassert_equal(fake(underglow)->count, NUM_UNDERGLOW);
    zassert_mem_equal(fake(underglow)->frame, &pixels[FIRST_UNDERGLOW],
                      NUM_UNDERGLOW * sizeof(pixels[0]));
}

ZTEST(defy_leds, test_underglow_powered_only_while_lit) {
    /* Only a key lit: the underglow stays off and is sent nothing. */
    pixels[0].r = 1;
    zassert_ok(led_strip_update_rgb(leds, pixels, NUM_LEDS));
    zassert_equal(power_level(), 0, "underglow powered for a key LED");
    zassert_equal(fake(underglow)->updates, 0, "frame sent to an unpowered underglow");
    zassert_equal(fake(keys)->updates, 1);

    /* An underglow LED lit: power first, then the frame. */
    pixels[FIRST_UNDERGLOW + 2].b = 1;
    zassert_ok(led_strip_update_rgb(leds, pixels, NUM_LEDS));
    zassert_equal(power_level(), 1, "underglow not powered while lit");
    zassert_equal(fake(underglow)->updates, 1);
    zassert_equal(power_when_sent, 1, "frame sent before the supply was on");

    /* Dark again: power off, no frame. */
    pixels[FIRST_UNDERGLOW + 2].b = 0;
    zassert_ok(led_strip_update_rgb(leds, pixels, NUM_LEDS));
    zassert_equal(power_level(), 0, "underglow left powered while dark");
    zassert_equal(fake(underglow)->updates, 1, "frame sent to an unpowered underglow");
}

ZTEST(defy_leds, test_suspend_cuts_underglow_power) {
    pixels[FIRST_UNDERGLOW].g = 5;
    zassert_ok(led_strip_update_rgb(leds, pixels, NUM_LEDS));
    zassert_equal(power_level(), 1);

    zassert_ok(pm_device_action_run(leds, PM_DEVICE_ACTION_SUSPEND));
    zassert_equal(power_level(), 0, "underglow left powered while suspended");

    /* Lit frames while suspended reach the per-key LEDs, not the underglow. */
    pixels[0].r = 3;
    zassert_ok(led_strip_update_rgb(leds, pixels, NUM_LEDS));
    zassert_equal(power_level(), 0, "underglow powered while suspended");
    zassert_equal(fake(underglow)->updates, 1, "frame sent while suspended");
    zassert_equal(fake(keys)->updates, 2);

    /* After resume, the next lit frame powers the underglow up again. */
    zassert_ok(pm_device_action_run(leds, PM_DEVICE_ACTION_RESUME));
    zassert_equal(power_level(), 0);
    zassert_ok(led_strip_update_rgb(leds, pixels, NUM_LEDS));
    zassert_equal(power_level(), 1, "underglow not powered after resume");
    zassert_equal(fake(underglow)->updates, 2);
    zassert_equal(fake(underglow)->frame[0].g, 5);
}

ZTEST(defy_leds, test_short_frame_reaches_keys_only) {
    pixels[0].r = 1;
    pixels[FIRST_UNDERGLOW].r = 1;
    zassert_ok(led_strip_update_rgb(leds, pixels, 2));
    zassert_equal(fake(keys)->count, 2);
    zassert_equal(fake(underglow)->updates, 0);
    zassert_equal(power_level(), 0);
}

ZTEST_SUITE(defy_leds, NULL, suite_setup, before, NULL, NULL);
