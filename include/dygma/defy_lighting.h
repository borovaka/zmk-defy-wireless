/* SPDX-License-Identifier: MIT */

#ifndef DYGMA_DEFY_LIGHTING_H
#define DYGMA_DEFY_LIGHTING_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <dygma/defy_control.h>

/* The version of the lighting state, its acknowledgement, the groups and the wake report. */
#define DEFY_LIGHTING_VERSION 3u
#define DEFY_LIGHTING_CONTROL_SIZE 20u
#define DEFY_LIGHTING_GROUP_SIZE 15u
#define DEFY_LIGHTING_WAKE_SIZE 12u
/* How many groups the Neuron can define, and the per-key LEDs a group covers on a half. */
#define DEFY_LIGHTING_GROUPS 16u
#define DEFY_LIGHTING_KEY_LEDS 35u
/* Per-key LEDs per row, top to bottom, each row counted from the half's outer edge. */
#define DEFY_LIGHTING_ROW_LEDS {7, 7, 7, 6, 8}
#define DEFY_LIGHTING_ROWS 5u
_Static_assert(7 + 7 + 7 + 6 + 8 == DEFY_LIGHTING_KEY_LEDS, "the rows hold every per-key LED");
/* A read of the half's lighting characteristic: its state or its wake report. */
#define DEFY_LIGHTING_READ_SIZE                                                                    \
    (DEFY_LIGHTING_CONTROL_SIZE > DEFY_LIGHTING_WAKE_SIZE ? DEFY_LIGHTING_CONTROL_SIZE             \
                                                          : DEFY_LIGHTING_WAKE_SIZE)

/* A colour of the everyday lighting, 0 to 255 per channel as the LEDs take it. */
struct defy_lighting_colour {
    uint8_t r;
    uint8_t g;
    uint8_t b;
};

/*
 * The Neuron's lighting: whether the lights are on, and the colours a
 * connected half shows when no other display is on top, one for the per-key
 * LEDs and one for the underglow, both already scaled by the brightness.
 * Black leaves those LEDs dark. groups names the groups drawn over the key
 * colour, one bit each, and level is the brightness that scales their colours,
 * 1 to 255.
 */
struct defy_lighting_state {
    uint32_t boot;
    uint32_t revision;
    bool enabled;
    struct defy_lighting_colour keys;
    struct defy_lighting_colour underglow;
    uint16_t groups;
    uint8_t level;
};

/*
 * A colour for some of one half's per-key LEDs, from the Neuron's keymap. The
 * Neuron sends a half each group a state names before the state; boot ties it
 * to that Neuron's states. leds has a bit for each per-key LED, in LED order.
 */
struct defy_lighting_group {
    uint32_t boot;
    uint8_t index;
    struct defy_lighting_colour colour;
    uint64_t leds;
};

/* What a lighting state shows on a half once its groups are drawn over the key colour. */
struct defy_lighting_view {
    bool enabled;
    struct defy_lighting_colour keys[DEFY_LIGHTING_KEY_LEDS];
    struct defy_lighting_colour underglow;
};

static inline bool defy_lighting_colour_equal(struct defy_lighting_colour a,
                                              struct defy_lighting_colour b) {
    return a.r == b.r && a.g == b.g && a.b == b.b;
}

/* A colour at full strength scaled to a brightness of 0 to 255, rounded. */
static inline struct defy_lighting_colour defy_lighting_scale(struct defy_lighting_colour colour,
                                                              uint8_t level) {
    return (struct defy_lighting_colour){
        .r = (colour.r * level + 127) / 255,
        .g = (colour.g * level + 127) / 255,
        .b = (colour.b * level + 127) / 255,
    };
}

/*
 * A half's wake report: its side, whether a key woke it from sleep, a number for this start
 * of the half, and its uptime, which is the wake's age.
 */
struct defy_lighting_wake {
    uint8_t side;
    bool from_sleep;
    uint32_t boot;
    uint32_t age_ms;
};

static inline bool defy_lighting_equal(const struct defy_lighting_state *a,
                                       const struct defy_lighting_state *b) {
    return a->boot == b->boot && a->revision == b->revision && a->enabled == b->enabled &&
           defy_lighting_colour_equal(a->keys, b->keys) &&
           defy_lighting_colour_equal(a->underglow, b->underglow) && a->groups == b->groups &&
           a->level == b->level;
}

/* The colours as a control message and a 'DL' frame carry them: keys, then
 * underglow, each red, green, blue. */
#define DEFY_LIGHTING_COLOURS_SIZE 6u

static inline void defy_lighting_colours_put(const struct defy_lighting_state *state,
                                             uint8_t data[DEFY_LIGHTING_COLOURS_SIZE]) {
    data[0] = state->keys.r;
    data[1] = state->keys.g;
    data[2] = state->keys.b;
    data[3] = state->underglow.r;
    data[4] = state->underglow.g;
    data[5] = state->underglow.b;
}

static inline void defy_lighting_colours_get(const uint8_t data[DEFY_LIGHTING_COLOURS_SIZE],
                                             struct defy_lighting_state *state) {
    state->keys = (struct defy_lighting_colour){data[0], data[1], data[2]};
    state->underglow = (struct defy_lighting_colour){data[3], data[4], data[5]};
}

/*
 * The state's fields after the colours, as a control message and a 'DL' frame
 * carry them: the groups, little-endian, then the level.
 */
#define DEFY_LIGHTING_GROUPS_FIELD_SIZE 3u

static inline void defy_lighting_groups_put(const struct defy_lighting_state *state,
                                            uint8_t data[DEFY_LIGHTING_GROUPS_FIELD_SIZE]) {
    data[0] = state->groups & 0xff;
    data[1] = state->groups >> 8;
    data[2] = state->level;
}

static inline void defy_lighting_groups_get(const uint8_t data[DEFY_LIGHTING_GROUPS_FIELD_SIZE],
                                            struct defy_lighting_state *state) {
    state->groups = data[0] | (uint16_t)data[1] << 8;
    state->level = data[2];
}

/*
 * A group's index, colour and LEDs, as a group message carries them after the
 * boot and a 'DL' frame after its boot: the index, the colour, then the LEDs
 * in DEFY_LIGHTING_GROUP_LEDS_SIZE bytes, little-endian.
 */
#define DEFY_LIGHTING_GROUP_FIELDS_SIZE 9u
#define DEFY_LIGHTING_GROUP_LEDS_SIZE 5u
_Static_assert(DEFY_LIGHTING_KEY_LEDS <= 8 * DEFY_LIGHTING_GROUP_LEDS_SIZE,
               "a group's LED mask has a bit for every per-key LED");

static inline void defy_lighting_group_fields_put(const struct defy_lighting_group *group,
                                                  uint8_t data[DEFY_LIGHTING_GROUP_FIELDS_SIZE]) {
    data[0] = group->index;
    data[1] = group->colour.r;
    data[2] = group->colour.g;
    data[3] = group->colour.b;
    for (size_t i = 0; i < DEFY_LIGHTING_GROUP_LEDS_SIZE; i++) {
        data[4 + i] = group->leds >> (8 * i);
    }
}

static inline void
defy_lighting_group_fields_get(const uint8_t data[DEFY_LIGHTING_GROUP_FIELDS_SIZE],
                               struct defy_lighting_group *group) {
    group->index = data[0];
    group->colour = (struct defy_lighting_colour){data[1], data[2], data[3]};
    group->leds = 0;
    for (size_t i = 0; i < DEFY_LIGHTING_GROUP_LEDS_SIZE; i++) {
        group->leds |= (uint64_t)data[4 + i] << (8 * i);
    }
}

/* Whether a group is one a half can take: a boot, an index below 16, LEDs that exist. */
static inline bool defy_lighting_group_valid(const struct defy_lighting_group *group) {
    return group && group->boot && group->index < DEFY_LIGHTING_GROUPS &&
           !(group->leds >> DEFY_LIGHTING_KEY_LEDS);
}

/* Whether a state is one a half can take: a boot, a revision, a level for its groups. */
static inline bool defy_lighting_state_valid(const struct defy_lighting_state *state) {
    return state && state->boot && state->revision && (!state->groups || state->level);
}

/* opcode is DEFY_CONTROL_LIGHTING_SET or DEFY_CONTROL_LIGHTING_ACK. */
int defy_lighting_control_encode(const struct defy_lighting_state *state, uint8_t opcode,
                                 uint8_t data[DEFY_LIGHTING_CONTROL_SIZE]);
int defy_lighting_control_decode(const void *data, size_t length, uint8_t opcode,
                                 struct defy_lighting_state *state);
/* A group as a DEFY_CONTROL_LIGHTING_GROUP message. */
int defy_lighting_group_encode(const struct defy_lighting_group *group,
                               uint8_t data[DEFY_LIGHTING_GROUP_SIZE]);
int defy_lighting_group_decode(const void *data, size_t length, struct defy_lighting_group *group);
/*
 * What state shows: its key colour on every per-key LED, then each group it
 * names, lowest index first, scaled by its level. groups holds the half's
 * groups by index; held has a bit for each it holds for state's boot.
 */
void defy_lighting_view(const struct defy_lighting_state *state,
                        const struct defy_lighting_group groups[DEFY_LIGHTING_GROUPS],
                        uint16_t held, struct defy_lighting_view *view);
int defy_lighting_wake_encode(const struct defy_lighting_wake *wake,
                              uint8_t data[DEFY_LIGHTING_WAKE_SIZE]);
int defy_lighting_wake_decode(const void *data, size_t length, struct defy_lighting_wake *wake);

/*
 * How the Neuron sends its lighting state to one half over one link: at once
 * after each change, then again 250 ms later, the wait doubling up to 4000 ms,
 * until the half acknowledges the state or DEFY_LIGHTING_MAX_ATTEMPTS were
 * sent. In lighting/defy_lighting_protocol.c.
 */
#define DEFY_LIGHTING_MAX_ATTEMPTS 6u

struct defy_lighting_retry {
    struct defy_lighting_state target;
    int64_t due;
    uint8_t attempts;
    bool confirmed;
};

/* Starts sending desired afresh. */
void defy_lighting_retry_start(struct defy_lighting_retry *retry,
                               const struct defy_lighting_state *desired, int64_t now);
/* Starts afresh when desired is not the state being sent; whether it did. */
bool defy_lighting_retry_follow(struct defy_lighting_retry *retry,
                                const struct defy_lighting_state *desired, int64_t now);
/* Whether the state still needs sending: unacknowledged, with attempts left. */
bool defy_lighting_retry_pending(const struct defy_lighting_retry *retry);
/* Whether it needs sending now. */
bool defy_lighting_retry_due(const struct defy_lighting_retry *retry, int64_t now);
/* Counts an attempt sent now, and waits before the next. */
void defy_lighting_retry_sent(struct defy_lighting_retry *retry, int64_t now);
/* The half acknowledged state, which ends the sending if it is the target. */
void defy_lighting_retry_acknowledged(struct defy_lighting_retry *retry,
                                      const struct defy_lighting_state *state);
/* Stops sending, as the half cannot take the state. */
void defy_lighting_retry_stop(struct defy_lighting_retry *retry);

/* The Neuron's lighting state, in lighting/defy_lighting_central.c. */
bool defy_lighting_central_get(struct defy_lighting_state *state);
/*
 * The group with index for the half on side, from the keymap, under the boot
 * of the Neuron's states; -ENOENT when there is no such group, -EAGAIN before
 * the settings have loaded.
 */
int defy_lighting_central_group(uint8_t side, uint8_t index, struct defy_lighting_group *group);
void defy_lighting_central_wake(const struct defy_lighting_wake *wake);
/* Runs one of &defy_lighting's commands, from dt-bindings/dygma/defy_lighting.h. */
int defy_lighting_command(uint32_t command);

/*
 * A half's lighting state, in lighting/defy_lighting_peripheral.c. apply() takes a state
 * that arrived over the link route: route_epoch numbers the Bluetooth connection, and the
 * cable passes the state's boot. It answers -EAGAIN until the half holds every group the
 * state names for its boot, and -ESTALE for an older or retired state. System work queue
 * only.
 */
int defy_lighting_peripheral_apply(const struct defy_lighting_state *state, enum defy_link route,
                                   uint32_t route_epoch);
/* Keeps a group that arrived over either link. */
int defy_lighting_peripheral_group(const struct defy_lighting_group *group);
bool defy_lighting_peripheral_get(struct defy_lighting_state *state);
void defy_lighting_peripheral_ble_epoch(uint32_t epoch);

#endif
