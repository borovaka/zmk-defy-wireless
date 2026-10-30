/*
 * A half's start-up, as far as getting Bluetooth up goes. With
 * CONFIG_DEFY_HALF_LEDS, lighting/defy_startup_leds.c defines these and
 * shows the stages on the half's LEDs and in its log; without it, reporting
 * compiles to nothing. dygma/defy_half_leds.h has the rest of what the half
 * shows on its LEDs.
 *
 * SPDX-License-Identifier: MIT
 */

#ifndef DYGMA_DEFY_STARTUP_H
#define DYGMA_DEFY_STARTUP_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <zephyr/toolchain.h>

struct defy_radio_factory;

/*
 * Whether the half reaches the Neuron, shown in cyan. The hybrid transport calls
 * it with its combined cable and Bluetooth status; a half with one transport
 * takes it from ZMK's split peripheral status event instead.
 */
#if defined(CONFIG_DEFY_HALF_LEDS)
void defy_startup_split_connected(bool connected);
#else
static inline void defy_startup_split_connected(bool connected) { ARG_UNUSED(connected); }
#endif

enum defy_startup_stage {
    /* Zephyr and the half's drivers are up. */
    DEFY_STARTUP_RUNNING,
    /* Dygma's radio bootloader answered. */
    DEFY_STARTUP_RADIO_BOOTLOADER,
    /* The radio firmware is being written; see defy_startup_progress(). */
    DEFY_STARTUP_RADIO_INSTALLING,
    /* The radio runs this build's firmware: Bluetooth is up. */
    DEFY_STARTUP_RADIO_READY,
    /* The radio never came up: no Bluetooth. */
    DEFY_STARTUP_RADIO_FAILED,
};

#if defined(CONFIG_DEFY_HALF_LEDS)
void defy_startup_report(enum defy_startup_stage stage);

/* Bytes of the radio firmware written so far; implies DEFY_STARTUP_RADIO_INSTALLING. */
void defy_startup_progress(size_t written, size_t size);

/* The radio's two-byte answer to a status poll, each time it changes. */
void defy_startup_radio_answer(uint8_t first, uint8_t second);
#else
static inline void defy_startup_report(enum defy_startup_stage stage) { ARG_UNUSED(stage); }

static inline void defy_startup_progress(size_t written, size_t size) {
    ARG_UNUSED(written);
    ARG_UNUSED(size);
}

static inline void defy_startup_radio_answer(uint8_t first, uint8_t second) {
    ARG_UNUSED(first);
    ARG_UNUSED(second);
}
#endif

#if defined(CONFIG_DEFY_HALF_LEDS) && defined(CONFIG_DEFY_RADIO_INSTALL)
/* A bootloader session began, with defy_radio_factory_begin()'s result. */
void defy_startup_radio_session(int err);
/* The session's install or check ended with err; attempted if it installed. */
void defy_startup_radio_install(const struct defy_radio_factory *factory, int err, bool attempted);
#else
static inline void defy_startup_radio_session(int err) { ARG_UNUSED(err); }

static inline void defy_startup_radio_install(const struct defy_radio_factory *factory, int err,
                                            bool attempted) {
    ARG_UNUSED(factory);
    ARG_UNUSED(err);
    ARG_UNUSED(attempted);
}
#endif

#endif
