/*
 * IS31FL3743B driver against the emulated chip, with the Defy's LED layout.
 *
 * SPDX-License-Identifier: MIT
 */

#include <string.h>

#include <zephyr/device.h>
#include <zephyr/drivers/emul.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/drivers/gpio/gpio_emul.h>
#include <zephyr/drivers/led_strip.h>
#include <zephyr/pm/device.h>
#include <zephyr/ztest.h>

#include "emul.h"

#define LEDS     DT_NODELABEL(key_leds)
#define NUM_LEDS DT_PROP(LEDS, chain_length)

static const struct device *const leds = DEVICE_DT_GET(LEDS);
static const struct gpio_dt_spec sdb = GPIO_DT_SPEC_GET(LEDS, sdb_gpios);

/*
 * First PWM register of each LED, written out in full, apart from the slot
 * arithmetic behind defy-key-leds.h.
 */
static const uint8_t expected_first_register[NUM_LEDS] = {
    0x01, 0x13, 0x25, 0x37, 0x49, 0x5b, 0x6d, /* line 0 */
    0x05, 0x17, 0x29, 0x3b, 0x4d, 0x5f, 0x71, /* line 1 */
    0x09, 0x1b, 0x2d, 0x3f, 0x51, 0x63, 0x75, /* line 2 */
    0x7f, 0x91, 0x83, 0x95, 0x87, 0x99,       /* line 3 */
    0x0d, 0x1f, 0x31, 0x43, 0x55, 0x67, 0x79, 0x8b, /* line 4, thumbs */
};

static struct led_rgb pixels[NUM_LEDS];
static int sdb_at_boot;
static unsigned int transfers_at_boot;

static struct is31fl3743b_emul_data *chip(void) { return EMUL_DT_GET(LEDS)->data; }

static int sdb_level(void) { return gpio_emul_output_get(sdb.port, sdb.pin); }

static void *suite_setup(void) {
    sdb_at_boot = sdb_level();
    transfers_at_boot = chip()->transfers;
    return NULL;
}

/* Each test starts dark, with a blank register model. */
static void before(void *fixture) {
    ARG_UNUSED(fixture);
    memset(pixels, 0, sizeof(pixels));
    zassert_ok(led_strip_update_rgb(leds, pixels, NUM_LEDS));
    memset(chip(), 0, sizeof(*chip()));
}

ZTEST(is31fl3743b, test_boot_leaves_chip_shut_down) {
    zassert_equal(sdb_at_boot, 0, "SDB high at boot");
    zassert_equal(transfers_at_boot, 0, "SPI traffic at boot");
}

ZTEST(is31fl3743b, test_power_up_writes_stock_configuration) {
    pixels[0] = (struct led_rgb){.r = 0x10, .g = 0x20, .b = 0x30};
    zassert_ok(led_strip_update_rgb(leds, pixels, NUM_LEDS));

    zassert_equal(sdb_level(), 1, "chip not powered up");
    zassert_equal(chip()->pages[2][0x02], 0xb3, "pull register");
    zassert_equal(chip()->pages[2][0x01], 0xff, "global current");
    zassert_equal(chip()->pages[2][0x00], 0x29, "configuration");
    for (int reg = 0x01; reg <= 0xc6; reg++) {
        zassert_equal(chip()->pages[1][reg], 0xff, "scaling register 0x%02x", reg);
    }
    zassert_equal(chip()->pages[1][0xc7], 0, "scaling write ran past 0xc6");
}

ZTEST(is31fl3743b, test_pixels_land_on_stock_registers) {
    for (int i = 0; i < NUM_LEDS; i++) {
        pixels[i] = (struct led_rgb){.r = 0x40 + i, .g = 0x80 + i, .b = 0};
    }
    zassert_ok(led_strip_update_rgb(leds, pixels, NUM_LEDS));

    const uint8_t *pwm = chip()->pages[0];
    int lit = 0;

    for (int i = 0; i < NUM_LEDS; i++) {
        uint8_t reg = expected_first_register[i];

        zassert_equal(pwm[reg], 0x40 + i, "LED %d red", i);
        zassert_equal(pwm[reg + 1], 0x80 + i, "LED %d green", i);
        zassert_equal(pwm[reg + 2], 0, "LED %d blue", i);
        zassert_equal(pwm[reg + 3], 0, "LED %d white", i);
    }
    for (int reg = 0x01; reg <= 0xc6; reg++) {
        lit += pwm[reg] != 0;
    }
    zassert_equal(lit, NUM_LEDS * 2, "dots lit outside the LED map");
}

/* The part common to red, green and blue lights the white LED instead. */
ZTEST(is31fl3743b, test_white_takes_the_common_part) {
    pixels[0] = (struct led_rgb){.r = 0x18, .g = 0x18, .b = 0x18};
    pixels[1] = (struct led_rgb){.r = 0x30, .g = 0x20, .b = 0x10};
    pixels[2] = (struct led_rgb){.r = 0x05, .g = 0x40, .b = 0x80};
    pixels[3] = (struct led_rgb){.g = 0x18, .b = 0x18};
    zassert_ok(led_strip_update_rgb(leds, pixels, NUM_LEDS));

    static const uint8_t expected[][4] = {
        {0x00, 0x00, 0x00, 0x18},
        {0x20, 0x10, 0x00, 0x10},
        {0x00, 0x3b, 0x7b, 0x05},
        {0x00, 0x18, 0x18, 0x00},
    };
    const uint8_t *pwm = chip()->pages[0];

    for (int i = 0; i < ARRAY_SIZE(expected); i++) {
        zassert_mem_equal(&pwm[expected_first_register[i]], expected[i], 4, "LED %d", i);
    }
}

ZTEST(is31fl3743b, test_dark_frame_shuts_chip_down) {
    pixels[5].g = 1;
    zassert_ok(led_strip_update_rgb(leds, pixels, NUM_LEDS));
    zassert_equal(sdb_level(), 1);

    unsigned int transfers = chip()->transfers;

    pixels[5].g = 0;
    zassert_ok(led_strip_update_rgb(leds, pixels, NUM_LEDS));
    zassert_equal(sdb_level(), 0, "chip left powered while dark");
    zassert_equal(chip()->transfers, transfers, "SPI traffic while dark");
}

ZTEST(is31fl3743b, test_reconfigures_after_shutdown) {
    pixels[0].r = 1;
    zassert_ok(led_strip_update_rgb(leds, pixels, NUM_LEDS));
    pixels[0].r = 0;
    zassert_ok(led_strip_update_rgb(leds, pixels, NUM_LEDS));
    memset(chip(), 0, sizeof(*chip()));

    pixels[0].r = 2;
    zassert_ok(led_strip_update_rgb(leds, pixels, NUM_LEDS));
    zassert_equal(chip()->pages[2][0x00], 0x29, "configuration not sent again");
    zassert_equal(chip()->pages[0][0x01], 2);
}

ZTEST(is31fl3743b, test_suspend_shuts_down_and_resume_restores) {
    pixels[3].b = 7;
    zassert_ok(led_strip_update_rgb(leds, pixels, NUM_LEDS));
    zassert_ok(pm_device_action_run(leds, PM_DEVICE_ACTION_SUSPEND));
    zassert_equal(sdb_level(), 0, "chip left powered while suspended");
    memset(chip(), 0, sizeof(*chip()));

    /* Updates while suspended are kept but not sent. */
    pixels[3].b = 9;
    zassert_ok(led_strip_update_rgb(leds, pixels, NUM_LEDS));
    zassert_equal(chip()->transfers, 0, "SPI traffic while suspended");
    zassert_equal(sdb_level(), 0);

    zassert_ok(pm_device_action_run(leds, PM_DEVICE_ACTION_RESUME));
    zassert_equal(sdb_level(), 1, "chip not powered up on resume");
    zassert_equal(chip()->pages[2][0x00], 0x29, "configuration not sent on resume");
    zassert_equal(chip()->pages[0][expected_first_register[3] + 2], 9, "frame not restored");
}

ZTEST_SUITE(is31fl3743b, NULL, suite_setup, before, NULL, NULL);
