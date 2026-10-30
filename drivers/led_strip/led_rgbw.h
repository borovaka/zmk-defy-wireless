/*
 * The channels of an LED strip pixel on LEDs with a white channel. The LED
 * strip API gives red, green and blue only. On a strip whose color-mapping has
 * a white channel, the part of a pixel common to red, green and blue lights the
 * white LED instead: white takes the smallest of the three, and each colour
 * channel the rest. Grey lights the white LED alone, and a pixel with a colour
 * channel at 0 shows as it would without one. On the Defy's LEDs, red, green
 * and blue mixed to white show a tint that the white LED does not.
 *
 * SPDX-License-Identifier: MIT
 */

#ifndef DYGMA_LED_RGBW_H
#define DYGMA_LED_RGBW_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <zephyr/drivers/led_strip.h>
#include <zephyr/dt-bindings/led/led.h>

/* Whether a strip's color-mapping has a white channel. */
static inline bool led_rgbw_has_white(const uint8_t *color_mapping, size_t num_colors) {
    for (size_t i = 0; i < num_colors; i++) {
        if (color_mapping[i] == LED_COLOR_ID_WHITE) {
            return true;
        }
    }

    return false;
}

/*
 * The value of channel color, an LED_COLOR_ID_*, for pixel. white says whether
 * the strip has a white channel. Channels other than red, green, blue and
 * white stay off.
 */
static inline uint8_t led_rgbw_channel(const struct led_rgb *pixel, uint8_t color, bool white) {
    uint8_t common = 0;

    if (white) {
        common = pixel->r < pixel->g ? pixel->r : pixel->g;
        common = common < pixel->b ? common : pixel->b;
    }

    switch (color) {
    case LED_COLOR_ID_RED:
        return pixel->r - common;
    case LED_COLOR_ID_GREEN:
        return pixel->g - common;
    case LED_COLOR_ID_BLUE:
        return pixel->b - common;
    case LED_COLOR_ID_WHITE:
        return common;
    default:
        return 0;
    }
}

#endif
