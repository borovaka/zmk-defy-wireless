/* SPDX-License-Identifier: MIT */

#include <dygma/defy_lighting.h>
#include <dygma/defy_wired_protocol.h>
#include <errno.h>
#include <zephyr/sys/byteorder.h>
#include <zephyr/sys/util.h>

/* The state: version, opcode, enabled, boot, revision, colours, then groups and level. */
enum {
    STATE_ENABLED = 2,
    STATE_BOOT = 3,
    STATE_REVISION = 7,
    STATE_COLOURS = 11,
    STATE_GROUPS = STATE_COLOURS + DEFY_LIGHTING_COLOURS_SIZE,
};
/* A group: version, opcode, boot, then its fields. */
enum { GROUP_BOOT = 2, GROUP_FIELDS = 6 };
/* The wake report: version, opcode, side, from_sleep, boot, age. */
enum { WAKE_SIDE = 2, WAKE_FROM_SLEEP = 3, WAKE_BOOT = 4, WAKE_AGE = 8 };

BUILD_ASSERT(STATE_GROUPS + DEFY_LIGHTING_GROUPS_FIELD_SIZE == DEFY_LIGHTING_CONTROL_SIZE,
             "the state ends with its groups and level");
BUILD_ASSERT(GROUP_FIELDS + DEFY_LIGHTING_GROUP_FIELDS_SIZE == DEFY_LIGHTING_GROUP_SIZE,
             "a group message is its header, boot and fields");
BUILD_ASSERT(WAKE_AGE + 4 == DEFY_LIGHTING_WAKE_SIZE, "the wake report ends with its age");
BUILD_ASSERT(DEFY_LIGHTING_GROUPS <= 16, "the state's groups field has 16 bits");

int defy_lighting_control_encode(const struct defy_lighting_state *state, uint8_t opcode,
                                 uint8_t data[DEFY_LIGHTING_CONTROL_SIZE]) {
    if (!data || !defy_lighting_state_valid(state) ||
        (opcode != DEFY_CONTROL_LIGHTING_SET && opcode != DEFY_CONTROL_LIGHTING_ACK)) {
        return -EINVAL;
    }
    data[0] = DEFY_LIGHTING_VERSION;
    data[1] = opcode;
    data[STATE_ENABLED] = state->enabled;
    sys_put_le32(state->boot, data + STATE_BOOT);
    sys_put_le32(state->revision, data + STATE_REVISION);
    defy_lighting_colours_put(state, data + STATE_COLOURS);
    defy_lighting_groups_put(state, data + STATE_GROUPS);
    return 0;
}

int defy_lighting_control_decode(const void *data, size_t length, uint8_t opcode,
                                 struct defy_lighting_state *state) {
    if (!data || !state || length != DEFY_LIGHTING_CONTROL_SIZE) {
        return -EINVAL;
    }
    const uint8_t *bytes = data;
    if (bytes[0] != DEFY_LIGHTING_VERSION || bytes[1] != opcode || bytes[STATE_ENABLED] > 1 ||
        (opcode != DEFY_CONTROL_LIGHTING_SET && opcode != DEFY_CONTROL_LIGHTING_ACK)) {
        return -EPROTO;
    }
    struct defy_lighting_state value = {
        .boot = sys_get_le32(bytes + STATE_BOOT),
        .revision = sys_get_le32(bytes + STATE_REVISION),
        .enabled = bytes[STATE_ENABLED],
    };
    defy_lighting_colours_get(bytes + STATE_COLOURS, &value);
    defy_lighting_groups_get(bytes + STATE_GROUPS, &value);
    if (!defy_lighting_state_valid(&value)) {
        return -EPROTO;
    }
    *state = value;
    return 0;
}

int defy_lighting_group_encode(const struct defy_lighting_group *group,
                               uint8_t data[DEFY_LIGHTING_GROUP_SIZE]) {
    if (!data || !defy_lighting_group_valid(group)) {
        return -EINVAL;
    }
    data[0] = DEFY_LIGHTING_VERSION;
    data[1] = DEFY_CONTROL_LIGHTING_GROUP;
    sys_put_le32(group->boot, data + GROUP_BOOT);
    defy_lighting_group_fields_put(group, data + GROUP_FIELDS);
    return 0;
}

int defy_lighting_group_decode(const void *data, size_t length, struct defy_lighting_group *group) {
    if (!data || !group || length != DEFY_LIGHTING_GROUP_SIZE) {
        return -EINVAL;
    }
    const uint8_t *bytes = data;
    if (bytes[0] != DEFY_LIGHTING_VERSION || bytes[1] != DEFY_CONTROL_LIGHTING_GROUP) {
        return -EPROTO;
    }
    struct defy_lighting_group value = {.boot = sys_get_le32(bytes + GROUP_BOOT)};
    defy_lighting_group_fields_get(bytes + GROUP_FIELDS, &value);
    if (!defy_lighting_group_valid(&value)) {
        return -EPROTO;
    }
    *group = value;
    return 0;
}

void defy_lighting_view(const struct defy_lighting_state *state,
                        const struct defy_lighting_group groups[DEFY_LIGHTING_GROUPS],
                        uint16_t held, struct defy_lighting_view *view) {
    view->enabled = state->enabled;
    view->underglow = state->underglow;
    for (size_t led = 0; led < DEFY_LIGHTING_KEY_LEDS; led++) {
        view->keys[led] = state->keys;
    }
    uint16_t shown = state->groups & held;
    for (size_t i = 0; i < DEFY_LIGHTING_GROUPS; i++) {
        if (!(shown & BIT(i))) {
            continue;
        }
        struct defy_lighting_colour colour = defy_lighting_scale(groups[i].colour, state->level);
        for (size_t led = 0; led < DEFY_LIGHTING_KEY_LEDS; led++) {
            if (groups[i].leds & BIT64(led)) {
                view->keys[led] = colour;
            }
        }
    }
}

int defy_lighting_wake_encode(const struct defy_lighting_wake *wake,
                              uint8_t data[DEFY_LIGHTING_WAKE_SIZE]) {
    if (!wake || !data || wake->side > DEFY_WIRED_RIGHT || !wake->boot) {
        return -EINVAL;
    }
    data[0] = DEFY_LIGHTING_VERSION;
    data[1] = DEFY_CONTROL_LIGHTING_WAKE;
    data[WAKE_SIDE] = wake->side;
    data[WAKE_FROM_SLEEP] = wake->from_sleep;
    sys_put_le32(wake->boot, data + WAKE_BOOT);
    sys_put_le32(wake->age_ms, data + WAKE_AGE);
    return 0;
}

int defy_lighting_wake_decode(const void *data, size_t length, struct defy_lighting_wake *wake) {
    if (!data || !wake || length != DEFY_LIGHTING_WAKE_SIZE) {
        return -EINVAL;
    }
    const uint8_t *bytes = data;
    if (bytes[0] != DEFY_LIGHTING_VERSION || bytes[1] != DEFY_CONTROL_LIGHTING_WAKE ||
        bytes[WAKE_SIDE] > DEFY_WIRED_RIGHT || bytes[WAKE_FROM_SLEEP] > 1 ||
        !sys_get_le32(bytes + WAKE_BOOT)) {
        return -EPROTO;
    }
    *wake = (struct defy_lighting_wake){
        .side = bytes[WAKE_SIDE],
        .from_sleep = bytes[WAKE_FROM_SLEEP],
        .boot = sys_get_le32(bytes + WAKE_BOOT),
        .age_ms = sys_get_le32(bytes + WAKE_AGE),
    };
    return 0;
}

#define RETRY_FIRST_MS 250u
#define RETRY_LONGEST_MS 4000u

void defy_lighting_retry_start(struct defy_lighting_retry *retry,
                               const struct defy_lighting_state *desired, int64_t now) {
    *retry = (struct defy_lighting_retry){.target = *desired, .due = now};
}

bool defy_lighting_retry_follow(struct defy_lighting_retry *retry,
                                const struct defy_lighting_state *desired, int64_t now) {
    if (defy_lighting_equal(&retry->target, desired)) {
        return false;
    }
    defy_lighting_retry_start(retry, desired, now);
    return true;
}

bool defy_lighting_retry_pending(const struct defy_lighting_retry *retry) {
    return !retry->confirmed && retry->attempts < DEFY_LIGHTING_MAX_ATTEMPTS;
}

bool defy_lighting_retry_due(const struct defy_lighting_retry *retry, int64_t now) {
    return defy_lighting_retry_pending(retry) && now >= retry->due;
}

void defy_lighting_retry_sent(struct defy_lighting_retry *retry, int64_t now) {
    retry->due = now + MIN(RETRY_LONGEST_MS, RETRY_FIRST_MS << retry->attempts);
    retry->attempts++;
}

void defy_lighting_retry_acknowledged(struct defy_lighting_retry *retry,
                                      const struct defy_lighting_state *state) {
    if (defy_lighting_equal(&retry->target, state)) {
        retry->confirmed = true;
    }
}

void defy_lighting_retry_stop(struct defy_lighting_retry *retry) {
    retry->attempts = DEFY_LIGHTING_MAX_ATTEMPTS;
}
