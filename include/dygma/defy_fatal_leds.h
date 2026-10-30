/*
 * A half's per-key LEDs, written to the IS31FL3743B through the RP2040's
 * registers alone: for fatal errors, and for the start-up display when the
 * LED drivers are not ready. See diagnostics/defy_fatal_leds.c.
 *
 * SPDX-License-Identifier: MIT
 */

#ifndef DYGMA_DEFY_FATAL_LEDS_H
#define DYGMA_DEFY_FATAL_LEDS_H

#include <stdint.h>

#include <zephyr/fatal.h>
#include <zephyr/toolchain.h>

/* Level of a lit channel, out of 255. */
#define DEFY_FATAL_LEDS_LEVEL 24

/*
 * Every per-key LED with the same levels on its four channels, in the chip's
 * CS order. Saves and restores the SPI0 registers it changes, but must not
 * run while another user of SPI0 is mid-transfer.
 */
void defy_fatal_leds_show(uint8_t c0, uint8_t c1, uint8_t c2, uint8_t c3);

/*
 * The second colour of a fault's flash: blue for a CPU exception, white for a stack overflow,
 * green otherwise.
 */
static inline FUNC_NO_STACK_PROTECTOR void defy_fatal_leds_show_reason(unsigned int reason) {
    uint8_t c[4] = {0};

    c[reason == K_ERR_CPU_EXCEPTION ? 2 : reason == K_ERR_STACK_CHK_FAIL ? 3 : 1] =
        DEFY_FATAL_LEDS_LEVEL;
    defy_fatal_leds_show(c[0], c[1], c[2], c[3]);
}

#endif
