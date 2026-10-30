/* SPDX-License-Identifier: MIT */

#ifndef DYGMA_DEFY_FEEDBACK_H
#define DYGMA_DEFY_FEEDBACK_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <dygma/defy_control.h>
#include <dygma/defy_wired_frame.h>

/*
 * The host display, DEFY_CONTROL_HOST_FEEDBACK: the host overlay, or the
 * profile chooser with up to ten profiles in 16-bit masks and a count of
 * cleared profiles.
 */
#define DEFY_HOST_FEEDBACK_VERSION 3u
#define DEFY_HOST_FEEDBACK_SIZE 20u
#define DEFY_HOST_FEEDBACK_DURATION_MS 2000u
#define DEFY_HOST_FEEDBACK_PROFILE_COUNT 10u
/* The profiles on each half's number keys: 1 to 5 on the left, 6 to 10 on the right. */
#define DEFY_HOST_FEEDBACK_HALF_PROFILES 5u
#define DEFY_HOST_PREFERRED_BLE (1u << 0)
#define DEFY_HOST_SELECTED_BLE (1u << 1)
#define DEFY_HOST_PROFILE_CONNECTED (1u << 2)
#define DEFY_HOST_USB_READY (1u << 3)
#define DEFY_HOST_SELECTED_USB (1u << 4)
#define DEFY_HOST_PREFERRED_NONE (1u << 5)
#define DEFY_HOST_CHOOSER (1u << 6)
/* The Neuron sits in the dock. */
#define DEFY_HOST_DOCKED (1u << 7)

struct defy_host_feedback {
    uint32_t boot;
    uint32_t sequence;
    uint16_t remaining_ms;
    uint8_t profile;
    uint8_t flags;
    uint8_t profile_count;
    /* Profiles the Neuron has cleared since it started; a change flashes the chooser. */
    uint8_t clears;
    uint16_t paired_profiles;
    uint16_t connected_profiles;
};

static inline bool defy_host_feedback_valid(const struct defy_host_feedback *state) {
    if (!state || !state->boot || !state->sequence || !state->remaining_ms ||
        state->remaining_ms > DEFY_HOST_FEEDBACK_DURATION_MS ||
        state->profile >= DEFY_HOST_FEEDBACK_PROFILE_COUNT ||
        (state->flags & (DEFY_HOST_PREFERRED_BLE | DEFY_HOST_PREFERRED_NONE)) ==
            (DEFY_HOST_PREFERRED_BLE | DEFY_HOST_PREFERRED_NONE) ||
        (state->flags & (DEFY_HOST_SELECTED_BLE | DEFY_HOST_SELECTED_USB)) ==
            (DEFY_HOST_SELECTED_BLE | DEFY_HOST_SELECTED_USB) ||
        ((state->flags & DEFY_HOST_SELECTED_BLE) &&
         !(state->flags & DEFY_HOST_PROFILE_CONNECTED)) ||
        ((state->flags & DEFY_HOST_SELECTED_USB) && !(state->flags & DEFY_HOST_USB_READY))) {
        return false;
    }
    if (!(state->flags & DEFY_HOST_CHOOSER)) {
        return !state->profile_count && !state->paired_profiles && !state->connected_profiles;
    }
    return state->profile_count && state->profile_count <= DEFY_HOST_FEEDBACK_PROFILE_COUNT &&
           state->profile < state->profile_count &&
           !(state->paired_profiles >> state->profile_count) &&
           !(state->connected_profiles & ~state->paired_profiles);
}

/* Whether two host displays are the same request: every field but remaining_ms. */
static inline bool defy_host_feedback_equal(const struct defy_host_feedback *a,
                                            const struct defy_host_feedback *b) {
    return a->boot == b->boot && a->sequence == b->sequence && a->profile == b->profile &&
           a->flags == b->flags && a->profile_count == b->profile_count &&
           a->clears == b->clears && a->paired_profiles == b->paired_profiles &&
           a->connected_profiles == b->connected_profiles;
}

/* The host display over the cable, for the half on side. */
struct defy_wired_host_feedback {
    uint8_t side;
    uint32_t session;
    struct defy_host_feedback state;
};

struct defy_wired_control;

int defy_host_feedback_encode(const struct defy_host_feedback *state,
                              uint8_t data[DEFY_HOST_FEEDBACK_SIZE]);
int defy_host_feedback_decode(const void *data, size_t length,
                              struct defy_host_feedback *state);
int defy_wired_host_feedback_encode(const struct defy_wired_host_feedback *command,
                                    uint8_t frame[DEFY_WIRED_FRAME_SIZE]);
/* Reads the rest of a 'DL' frame whose header control is, as
 * defy_wired_control_check() read it. */
int defy_wired_host_feedback_decode(const uint8_t *frame, const struct defy_wired_control *control,
                                    struct defy_wired_host_feedback *command);
/* The Neuron's selection changed: shows it on both halves, over every link. */
void defy_host_feedback_show_selection(void);

#endif
