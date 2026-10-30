/*
 * A half's LED interface: what the Neuron and the half's own drivers show on
 * its LEDs, in lighting/defy_half_leds.c, which also lays down which display
 * is on top. While the start-up display shows the radio's bootloader, its
 * install or its failure, the battery and host displays return -EBUSY, and
 * the lighting preference waits. The calls return -ENODEV when the LEDs are
 * not ready. Without CONFIG_DEFY_HALF_LEDS, or without the battery or
 * lighting support a call needs, they do nothing and return -ENOTSUP.
 *
 * SPDX-License-Identifier: MIT
 */

#ifndef DYGMA_DEFY_HALF_LEDS_H
#define DYGMA_DEFY_HALF_LEDS_H

#include <stdbool.h>
#include <errno.h>

#include <zephyr/toolchain.h>

struct defy_host_feedback;
struct defy_lighting_view;

/*
 * What the Neuron's lighting state shows: off keeps the lights dark, on keeps
 * them lit while the half reaches the Neuron, in the view's colours once the
 * connection has shown. Before the first call, or after one with NULL, the
 * start-up display's own timeout applies. Returns -ENODEV while the LEDs are
 * not ready, or the LED strip's error, after which the same view is drawn
 * again.
 */
#if defined(CONFIG_DEFY_HALF_LEDS)
int defy_half_leds_lighting_set(const struct defy_lighting_view *view);
#else
static inline int defy_half_leds_lighting_set(const struct defy_lighting_view *view) {
    ARG_UNUSED(view);
    return -ENOTSUP;
}
#endif

/* The host overlay or the profile chooser, for state->remaining_ms. */
#if defined(CONFIG_DEFY_HALF_LEDS) && defined(CONFIG_DEFY_LIGHTING)
int defy_half_leds_host_show(const struct defy_host_feedback *state);
#else
static inline int defy_half_leds_host_show(const struct defy_host_feedback *state) {
    ARG_UNUSED(state);
    return -ENOTSUP;
}
#endif

#if defined(CONFIG_DEFY_HALF_LEDS) && defined(CONFIG_DEFY_BATTERY)
/* The battery display: the battery cells for five seconds, and a fresh sample from the radio. */
int defy_half_leds_battery_show(void);
/* Whether the battery display shows, which holds off sleep. */
bool defy_half_leds_battery_active(void);
/* The radio's battery sample changed; called from any context. */
void defy_half_leds_battery_updated(void);
#else
static inline int defy_half_leds_battery_show(void) { return -ENOTSUP; }
static inline bool defy_half_leds_battery_active(void) { return false; }
static inline void defy_half_leds_battery_updated(void) {}
#endif

#endif
