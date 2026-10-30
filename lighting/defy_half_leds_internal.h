/*
 * Between the files that draw a half's LEDs, and no others.
 *
 * defy_half_leds.c owns the LEDs, their frame and the lock, and stacks the
 * displays into the frame; its show() lays down the order. The start-up
 * display, defy_startup_leds.c, is the lowest, and with
 * CONFIG_DEFY_RADIO_INSTALL defy_radio_install_leds.c draws the radio
 * install's part of its failure report. Every function here runs with
 * defy_half_leds_lock held.
 *
 * SPDX-License-Identifier: MIT
 */

#ifndef DEFY_HALF_LEDS_INTERNAL_H
#define DEFY_HALF_LEDS_INTERNAL_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <zephyr/devicetree.h>
#include <zephyr/drivers/led_strip.h>
#include <zephyr/kernel.h>

#include <dygma/defy_lighting.h>

/* The per-key LEDs, then the underglow. */
#define DEFY_HALF_LEDS_LENGTH DT_PROP(DT_NODELABEL(defy_leds), chain_length)
#define DEFY_HALF_LEDS_KEYS DT_PROP(DT_NODELABEL(key_leds), chain_length)

/* The start-up display: dim, as it lights every LED and the half may be on battery. */
#define DEFY_HALF_LEDS_LEVEL 24
/* The half's own displays: a few LEDs, for a few seconds. */
#define DEFY_HALF_LEDS_CELL_LEVEL 128

extern struct k_mutex defy_half_leds_lock;

/* Per-key LEDs per row, top to bottom: DEFY_LIGHTING_ROW_LEDS. */
extern const uint8_t defy_half_leds_rows[DEFY_LIGHTING_ROWS];

/* From defy_half_leds.c. */

/* Checks the LEDs, once, before the first show(): whether they are ready.
 * Until they are, nothing shows. */
bool defy_half_leds_start(void);
/* Stacks the displays into the frame and writes it to the LEDs: 0, or the LED strip's error. */
int defy_half_leds_show(void);

/* From defy_startup_leds.c. */

/* Whether the half's own displays may show: the radio is up, or not yet asked. */
bool defy_startup_leds_allowed(void);
/* Whether the start-up display is dark, given whether the Neuron's lighting
 * preference has arrived and what it is. */
bool defy_startup_leds_hidden(bool preference_known, bool preference);
/* Draws the start-up display over the whole frame, with everyday, what the
 * Neuron's lighting state shows, or NULL before it has arrived. */
void defy_startup_leds_draw(struct led_rgb *frame, bool hidden,
                            const struct defy_lighting_view *everyday);

/* From defy_radio_install_leds.c. */

/* How many LEDs of a failure report row about the bootloader sessions to
 * light, and in which colour. */
void defy_radio_install_leds_row(size_t row, size_t *count, struct led_rgb *colour);
/* Whether an install attempt ended, whose trace then replaces the report. */
bool defy_radio_install_leds_traced(void);
void defy_radio_install_leds_draw_trace(struct led_rgb *frame);
/* Bytes of the radio firmware written so far, for the trace. */
void defy_radio_install_leds_progress(size_t written, size_t size);
/* Logs the trace, with the start-up stages. */
void defy_radio_install_leds_log(void);

#endif
