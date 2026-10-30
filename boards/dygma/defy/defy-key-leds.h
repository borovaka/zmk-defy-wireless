/*
 * First IS31FL3743B PWM register of each per-key LED, in LED order: row-major
 * over the rows of DEFY_LIGHTING_ROW_LEDS in dygma/defy_lighting.h.
 *
 * Each LED sits in a slot of the driver's matrix, as the board wires it. Slot s
 * is SW(s / 4 + 1), CS(4 * (s % 4) + 1) to CS(4 * (s % 4) + 4), so its first
 * register is (s / 4) * 18 + 4 * (s % 4) + 1.
 *
 * SPDX-License-Identifier: MIT
 */

#pragma once

#define DEFY_KEY_LED_COUNT 35

#define DEFY_KEY_LED_PWM_REGISTERS                                                                 \
    [01 13 25 37 49 5b 6d                                                                          \
     05 17 29 3b 4d 5f 71                                                                          \
     09 1b 2d 3f 51 63 75                                                                          \
     7f 91 83 95 87 99                                                                             \
     0d 1f 31 43 55 67 79 8b]
