/*
 * Packs one pixel for the WS2812 PIO program, which shifts each word out MSB
 * first: 24 bits for three channels, 32 for four. The first channel of the
 * color-mapping goes in the top byte. A white channel takes the part of the
 * pixel common to red, green and blue, as led_rgbw.h describes.
 *
 * SPDX-License-Identifier: MIT
 */

#ifndef DYGMA_WS2812_PIO_PACK_H
#define DYGMA_WS2812_PIO_PACK_H

#include <stddef.h>
#include <stdint.h>

#include <zephyr/drivers/led_strip.h>

#include "led_rgbw.h"

static inline uint32_t ws2812_pio_pack(const struct led_rgb *pixel, const uint8_t *color_mapping,
                                       size_t num_colors) {
    bool white = led_rgbw_has_white(color_mapping, num_colors);
    uint32_t word = 0;

    for (size_t i = 0; i < num_colors; i++) {
        word |= (uint32_t)led_rgbw_channel(pixel, color_mapping[i], white) << (24 - 8 * i);
    }

    return word;
}

#endif
