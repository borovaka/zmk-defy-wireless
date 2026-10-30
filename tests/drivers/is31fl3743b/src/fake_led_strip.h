/*
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include <zephyr/device.h>
#include <zephyr/drivers/led_strip.h>

#define FAKE_LED_STRIP_MAX_PIXELS 8

struct fake_led_strip_data {
    struct led_rgb frame[FAKE_LED_STRIP_MAX_PIXELS];
    size_t count;
    unsigned int updates;
    /* Called on each update, after recording the frame. */
    void (*on_update)(const struct device *dev);
};
