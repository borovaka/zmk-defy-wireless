/*
 * A half's LEDs: one frame that stacks every display the half shows, and the
 * displays the Neuron and the half's own drivers ask for, over the start-up
 * display of defy_startup_leds.c.
 *
 * - The Neuron's lighting preference: off keeps the start-up display dark,
 *   on keeps it lit while the half reaches the Neuron.
 * - The battery display: stock Dygma's three battery cells, for five seconds.
 * - The low-battery warning: stock's drain on the same cells, once two
 *   samples in a row read 15% or less.
 * - The host overlay and the profile chooser, from the Neuron.
 *
 * All but the preference light a few LEDs, brighter than the start-up
 * display. show() lays down which display is on top. docs/battery.md and
 * docs/lighting.md describe what they show.
 *
 * SPDX-License-Identifier: MIT
 */

#include <errno.h>
#include <string.h>

#include <zephyr/device.h>
#include <zephyr/drivers/led_strip.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/util.h>

#include <dygma/defy_battery.h>
#include <dygma/defy_battery_hci.h>
#include <dygma/defy_feedback.h>
#include <dygma/defy_half.h>
#include <dygma/defy_half_leds.h>
#include <dygma/defy_lighting.h>
#include <dygma/defy_wired_protocol.h>

#include "defy_half_leds_internal.h"

LOG_MODULE_DECLARE(defy_startup, LOG_LEVEL_INF);

BUILD_ASSERT(!IS_ENABLED(CONFIG_ZMK_RGB_UNDERGLOW), "ZMK's RGB underglow drives the same LEDs");

#define LENGTH DEFY_HALF_LEDS_LENGTH
#define KEYS DEFY_HALF_LEDS_KEYS
#define CELL_LEVEL DEFY_HALF_LEDS_CELL_LEVEL

const uint8_t defy_half_leds_rows[DEFY_LIGHTING_ROWS] = DEFY_LIGHTING_ROW_LEDS;

static const struct device *const leds = DEVICE_DT_GET(DT_NODELABEL(defy_leds));

K_MUTEX_DEFINE(defy_half_leds_lock);
static struct led_rgb pixels[LENGTH];
static bool ready;

/* What the Neuron's lighting state shows, once it has sent one. */
static bool lighting_override;
static struct defy_lighting_view lighting;
/* The last draw of the lighting failed, so the same view is drawn again. */
static bool lighting_failed;

bool defy_half_leds_start(void) {
    ready = device_is_ready(leds);
    return ready;
}

/* Whether the ordinary lighting is dark, by the lighting preference or the
 * start-up display's timeout. */
static bool lighting_hidden(void) {
    return defy_startup_leds_hidden(lighting_override, lighting.enabled);
}

/* Whether two views light the half the same: on or off, in the same colours. */
static bool same_look(const struct defy_lighting_view *a, const struct defy_lighting_view *b) {
    if (a->enabled != b->enabled || !defy_lighting_colour_equal(a->underglow, b->underglow)) {
        return false;
    }
    for (size_t i = 0; i < ARRAY_SIZE(a->keys); i++) {
        if (!defy_lighting_colour_equal(a->keys[i], b->keys[i])) {
            return false;
        }
    }
    return true;
}

int defy_half_leds_lighting_set(const struct defy_lighting_view *view) {
    k_mutex_lock(&defy_half_leds_lock, K_FOREVER);
    if (!ready) {
        k_mutex_unlock(&defy_half_leds_lock);
        return -ENODEV;
    }
    bool override = view != NULL;
    int err = 0;
    if (lighting_failed || lighting_override != override ||
        (override && !same_look(&lighting, view))) {
        lighting_override = override;
        if (override) {
            lighting = *view;
        }
        err = defy_half_leds_show();
        lighting_failed = err < 0;
    }
    k_mutex_unlock(&defy_half_leds_lock);
    return err;
}

/* Steps the animation on top, and redraws as a display ends. */
static void displays_step(struct k_work *work);
static K_WORK_DELAYABLE_DEFINE(displays_work, displays_step);

#if IS_ENABLED(CONFIG_DEFY_BATTERY) || IS_ENABLED(CONFIG_DEFY_LIGHTING)
/* The frame of an animation that began at start; *next is when the following one begins. */
static int64_t animation_frame(int64_t now, int64_t start, int64_t period, int64_t *next) {
    int64_t frame = (now - start) / period;

    *next = start + (frame + 1) * period;
    return frame;
}

#define BREATH_FLOOR 80u
#define BREATH_TOP (BREATH_FLOOR + 256u / 2)

/* 3x^2 - 2x^3 in 8-bit steps, 0 to 256 as x goes from 0 to 254: FastLED's
 * ease8InOutCubic (MIT), without its clamp of 256 to 255. */
static unsigned int ease_in_out_cubic(uint8_t x) {
    unsigned int squared = (x * x) >> 8;
    unsigned int cubed = (squared * x) >> 8;

    return 3 * squared - 2 * cubed;
}

/*
 * Breathing from 80 to 208, the same curve as Dygma's firmware, scaled so that 208 gives
 * peak. A breath takes 256 << shift ms: about four seconds at 4, as the battery breathes,
 * and two at 3, as stock's pairing screen does.
 */
static uint8_t breath(int64_t now, unsigned int shift, uint8_t peak) {
    uint8_t phase = (uint8_t)(now >> shift);
    /* Up from 0 to 254 over the first half of the breath, and back down over the second. */
    uint8_t rise = phase < 128 ? 2 * phase : 2 * (255 - phase);
    unsigned int level = BREATH_FLOOR + ease_in_out_cubic(rise) / 2;

    return level * peak / BREATH_TOP;
}
#endif

#if IS_ENABLED(CONFIG_DEFY_BATTERY)
/*
 * Stock Dygma's three battery cells, bottom to top: the innermost key of the
 * third, second and top rows on either half, as in defy-key-leds.h.
 */
static const uint8_t cells[] = {20, 13, 6};

#define BATTERY_SHOW_MS 5000
/* The battery display shows a sample no older than this. */
#define FRESH_MS 1000
#define CHARGING_FRAME_MS 160
#define BREATHE_FRAME_MS 40
#define WARNING_FRAME_MS 250
/* Three times through full, two, one and no cells. */
#define WARNING_MS (3 * 4 * WARNING_FRAME_MS)
/* The warning needs WARNING_SAMPLES samples in a row at WARNING_LEVEL or less,
 * and arms again after as many at REARM_LEVEL or more. */
#define WARNING_SAMPLES 2
#define WARNING_LEVEL 15
#define REARM_LEVEL 25

enum battery_view {
    BATTERY_UNAVAILABLE,
    BATTERY_CELLS,
    BATTERY_LOW,
    BATTERY_CHARGING,
    BATTERY_FAULT,
};

static int64_t battery_until;
static enum battery_view battery_view;
static size_t battery_cells;
static uint32_t battery_request_sequence;
static uint32_t battery_request_generation;
static int64_t battery_requested_at;
static uint32_t warning_generation;
static uint32_t warning_sequence;
static uint8_t warning_low_count;
static uint8_t warning_high_count;
static bool warning_low;
static bool warning_pending;
static int64_t warning_until;
static void battery_updated(struct k_work *work);
static K_WORK_DEFINE(battery_update_work, battery_updated);

/* The first count cells in colour, every other LED dark. */
static void show_cells(struct led_rgb *frame, size_t count, struct led_rgb colour) {
    memset(frame, 0, LENGTH * sizeof(frame[0]));
    for (size_t i = 0; i < count; i++) {
        frame[cells[i]] = colour;
    }
}

static bool battery_shows(int64_t now) { return battery_until > now; }

/* The battery display's cells. */
static int64_t draw_battery(struct led_rgb *frame, int64_t now) {
    const struct led_rgb green = {.g = CELL_LEVEL};
    int64_t next = INT64_MAX;

    switch (battery_view) {
    case BATTERY_CELLS:
        show_cells(frame, battery_cells, green);
        break;
    case BATTERY_LOW:
        show_cells(frame, 1, (struct led_rgb){.r = breath(now, 4, CELL_LEVEL)});
        animation_frame(now, battery_requested_at, BREATHE_FRAME_MS, &next);
        break;
    case BATTERY_CHARGING:
        show_cells(frame,
                   animation_frame(now, battery_requested_at, CHARGING_FRAME_MS, &next) % 4,
                   green);
        break;
    case BATTERY_FAULT:
        show_cells(frame, ARRAY_SIZE(cells), (struct led_rgb){.r = CELL_LEVEL});
        break;
    case BATTERY_UNAVAILABLE:
        show_cells(frame, 1, (struct led_rgb){.r = CELL_LEVEL, .g = CELL_LEVEL / 4});
        break;
    }
    return next;
}

/* Starts a waiting low-battery warning if the latest sample still reads low. */
static void start_warning(int64_t now) {
    struct defy_battery_sample sample;
    struct defy_battery_reading reading;

    if (warning_pending && defy_battery_hci_get(&sample) == 0 &&
        defy_battery_sample_reading(&sample, &reading) == 0 && reading.level <= WARNING_LEVEL) {
        warning_pending = false;
        warning_until = now + WARNING_MS;
    }
}

static bool warning_shows(int64_t now) { return warning_until > now; }

/* Stock Dygma's drain: the cells empty from the top, a frame each. */
static int64_t draw_warning(struct led_rgb *frame, int64_t now) {
    int64_t next;
    int64_t step = animation_frame(now, warning_until - WARNING_MS, WARNING_FRAME_MS, &next);

    show_cells(frame, ARRAY_SIZE(cells) - step % 4, (struct led_rgb){.r = CELL_LEVEL});
    return next;
}
#else
static inline bool battery_shows(int64_t now) {
    ARG_UNUSED(now);
    return false;
}

static inline int64_t draw_battery(struct led_rgb *frame, int64_t now) {
    ARG_UNUSED(frame);
    ARG_UNUSED(now);
    return INT64_MAX;
}

static inline void start_warning(int64_t now) { ARG_UNUSED(now); }

static inline bool warning_shows(int64_t now) {
    ARG_UNUSED(now);
    return false;
}

static inline int64_t draw_warning(struct led_rgb *frame, int64_t now) {
    ARG_UNUSED(frame);
    ARG_UNUSED(now);
    return INT64_MAX;
}
#endif

#if IS_ENABLED(CONFIG_DEFY_LIGHTING)
static struct defy_host_feedback host_feedback;
static int64_t host_until;
/* When the chooser's flash for a cleared profile ends. */
static int64_t host_cleared_until;

/*
 * The Neuron is docked: the innermost key of the second row. It is the middle
 * battery cell, which neither the host overlay nor the chooser uses.
 */
#define DOCKED_PIXEL 13
/* The host overlay's output marker: the innermost key of the top row. */
#define OUTPUT_PIXEL 6
/* The pairing screen's USB key: the outer end of the top row. */
#define USB_PIXEL 0
#define CHOOSER_FRAME_MS 40
#define CLEARED_MS 1000

/*
 * The top-row LED of a profile on the side's half, 0 if the other half has it: the left
 * half shows profiles 1 to 5 and the right half 6 to 10. LED 1 is next to the outer end
 * on both halves, keycap 1 on the left and 0 on the right.
 */
static size_t profile_pixel(enum defy_wired_side side, uint8_t index) {
    const uint8_t half = DEFY_HOST_FEEDBACK_HALF_PROFILES;

    if (side != DEFY_WIRED_RIGHT) {
        return index < half ? index + 1 : 0;
    }
    return index >= half ? 2 * half - index : 0;
}

BUILD_ASSERT(LENGTH > KEYS, "the chooser chases round the underglow");

/*
 * Stock Dygma's pairing screen, each half with its own five profiles; docs/lighting.md has
 * the colours. Returns when the frame next changes.
 */
static int64_t show_chooser(struct led_rgb *frame, int64_t now) {
    const struct led_rgb white = {.r = CELL_LEVEL, .g = CELL_LEVEL, .b = CELL_LEVEL};
    const struct led_rgb blue = {.b = CELL_LEVEL};
    uint8_t count = host_feedback.profile_count;
    uint8_t selected = host_feedback.profile;
    int64_t next = INT64_MAX;

    memset(frame, 0, LENGTH * sizeof(frame[0]));
    frame[USB_PIXEL] = (struct led_rgb){.r = CELL_LEVEL, .g = CELL_LEVEL};
    for (uint8_t i = 0; i < count; i++) {
        size_t key = profile_pixel(DEFY_HALF_ID, i);
        bool paired = host_feedback.paired_profiles & BIT(i);
        bool connected = host_feedback.connected_profiles & BIT(i);

        if (!key) {
            continue;
        }
        if (i == selected && connected) {
            frame[key] = (struct led_rgb){.g = CELL_LEVEL};
        } else if (i == selected && paired) {
            uint8_t level = breath(now, 4, CELL_LEVEL / 2);

            frame[key] = (struct led_rgb){.r = level, .g = level, .b = level};
        } else if (i == selected) {
            frame[key] = (struct led_rgb){.b = breath(now, 3, CELL_LEVEL)};
        } else {
            frame[key] = connected ? (struct led_rgb){.g = CELL_LEVEL / 4}
                         : paired  ? white
                                   : (struct led_rgb){.b = CELL_LEVEL / 4};
        }
        if (paired) {
            frame[defy_half_leds_rows[0] + key] = (struct led_rgb){.r = CELL_LEVEL / 2};
        }
    }
    if (!(host_feedback.connected_profiles & BIT(selected))) {
        int64_t step = animation_frame(now, 0, CHOOSER_FRAME_MS, &next);

        frame[KEYS + step % (LENGTH - KEYS)] = blue;
        frame[KEYS + (step + 1) % (LENGTH - KEYS)] = blue;
    }
    return next;
}

/*
 * The host overlay: the selected profile's key and the preferred output's marker, as
 * docs/lighting.md describes.
 */
static void show_host(struct led_rgb *frame) {
    const struct led_rgb green = {.g = CELL_LEVEL}, amber = {.r = CELL_LEVEL, .g = CELL_LEVEL / 4};
    const struct led_rgb blue = {.b = CELL_LEVEL}, purple = {.r = CELL_LEVEL, .b = CELL_LEVEL};
    const struct led_rgb white = {.r = CELL_LEVEL, .g = CELL_LEVEL, .b = CELL_LEVEL};
    uint8_t flags = host_feedback.flags;
    bool connected = flags & DEFY_HOST_PROFILE_CONNECTED;
    size_t key = profile_pixel(DEFY_HALF_ID, host_feedback.profile);

    memset(frame, 0, LENGTH * sizeof(frame[0]));
    if (key) {
        frame[key] = connected ? green : amber;
    }
    if (flags & DEFY_HOST_PREFERRED_BLE) {
        frame[OUTPUT_PIXEL] = connected ? blue : purple;
    } else if (!(flags & DEFY_HOST_PREFERRED_NONE)) {
        frame[OUTPUT_PIXEL] = flags & DEFY_HOST_USB_READY ? white : amber;
    }
}

/* The host display: the overlay, or the profile chooser. */
static bool host_shows(int64_t now) { return host_until > now; }

static bool chooser_shows(int64_t now) {
    return host_until > now && (host_feedback.flags & DEFY_HOST_CHOOSER);
}

static int64_t draw_host(struct led_rgb *frame, int64_t now) {
    bool chooser = host_feedback.flags & DEFY_HOST_CHOOSER;
    int64_t next = INT64_MAX;

    if (chooser) {
        next = show_chooser(frame, now);
    } else {
        show_host(frame);
    }
    if (host_feedback.flags & DEFY_HOST_DOCKED) {
        frame[DOCKED_PIXEL] = (struct led_rgb){.g = CELL_LEVEL, .b = CELL_LEVEL};
    }
    if (chooser && host_cleared_until > now) {
        /* Stock's flash after a profile is cleared: every key blue for a second. */
        for (size_t i = 0; i < KEYS; i++) {
            frame[i] = (struct led_rgb){.b = CELL_LEVEL};
        }
        next = MIN(next, host_cleared_until);
    }
    return next;
}

int defy_half_leds_host_show(const struct defy_host_feedback *state) {
    if (!defy_host_feedback_valid(state)) {
        return -EINVAL;
    }
    k_mutex_lock(&defy_half_leds_lock, K_FOREVER);
    if (!ready || !defy_startup_leds_allowed()) {
        int err = ready ? -EBUSY : -ENODEV;
        k_mutex_unlock(&defy_half_leds_lock);
        return err;
    }
    struct defy_lighting_state lighting;
    if (!defy_lighting_peripheral_get(&lighting) || lighting.boot != state->boot) {
        k_mutex_unlock(&defy_half_leds_lock);
        return -ESTALE;
    }
    int64_t now = k_uptime_get();
    int64_t until = now + state->remaining_ms;
    if (host_feedback.boot == state->boot) {
        int32_t difference = (int32_t)(state->sequence - host_feedback.sequence);
        if (difference < 0) {
            k_mutex_unlock(&defy_half_leds_lock);
            return -ESTALE;
        }
        if (difference == 0) {
            if (!defy_host_feedback_equal(state, &host_feedback)) {
                k_mutex_unlock(&defy_half_leds_lock);
                return -EPROTO;
            }
            if (host_until <= now) {
                k_mutex_unlock(&defy_half_leds_lock);
                return 0;
            }
            until = MIN(until, host_until);
            if (until == host_until) {
                k_mutex_unlock(&defy_half_leds_lock);
                return 0;
            }
        }
        /* A profile was cleared while the chooser was showing. */
        if ((state->flags & DEFY_HOST_CHOOSER) && host_until > now &&
            state->clears != host_feedback.clears) {
            host_cleared_until = now + CLEARED_MS;
        }
    }
    host_feedback = *state;
    host_until = until;
    defy_half_leds_show();
    k_mutex_unlock(&defy_half_leds_lock);
    return 0;
}
#else
static inline bool host_shows(int64_t now) {
    ARG_UNUSED(now);
    return false;
}

static inline bool chooser_shows(int64_t now) {
    ARG_UNUSED(now);
    return false;
}

static inline int64_t draw_host(struct led_rgb *frame, int64_t now) {
    ARG_UNUSED(frame);
    ARG_UNUSED(now);
    return INT64_MAX;
}
#endif

/* Wakes the displays at next, the frame on top's next change, or sooner as
 * the battery display or the host display ends. */
static void schedule_displays(int64_t next, int64_t now) {
    int64_t until = next;
#if IS_ENABLED(CONFIG_DEFY_BATTERY)
    if (battery_until > now) {
        until = MIN(until, battery_until);
    }
#endif
#if IS_ENABLED(CONFIG_DEFY_LIGHTING)
    if (host_until > now) {
        until = MIN(until, host_until);
    }
#endif
    if (until != INT64_MAX) {
        k_work_reschedule(&displays_work, K_MSEC(until - now));
    } else {
        k_work_cancel_delayable(&displays_work);
    }
}

/*
 * Stacks the displays into one frame and writes it. Each display takes the
 * whole frame, so the one on top is what lights. Lowest first:
 *
 * 1. The start-up display, which the lighting preference or its timeout can
 *    darken, with the radio's failure report.
 *
 * Then only while the radio is up or not yet asked:
 *
 * 2. The low-battery warning. It waits for the battery display and the host display to end,
 *    and for the lights to come back.
 * 3. The host overlay or the profile chooser.
 * 4. The battery display's cells, unless the chooser shows; the host overlay does
 *    not hide them.
 *
 * Called with the lock held.
 */
int defy_half_leds_show(void) {
    if (!ready) {
        return -ENODEV;
    }

    int64_t now = k_uptime_get();
    bool hidden = lighting_hidden();
    int64_t next = INT64_MAX;

    defy_startup_leds_draw(pixels, hidden, lighting_override ? &lighting : NULL);
    if (defy_startup_leds_allowed()) {
        if (!hidden && !battery_shows(now) && !host_shows(now)) {
            start_warning(now);
        }
        if (!hidden && warning_shows(now)) {
            next = draw_warning(pixels, now);
        }
        if (host_shows(now)) {
            next = draw_host(pixels, now);
        }
        if (battery_shows(now) && !chooser_shows(now)) {
            next = draw_battery(pixels, now);
        }
    }
    schedule_displays(next, now);

    int err = led_strip_update_rgb(leds, pixels, LENGTH);

    if (err < 0) {
        LOG_WRN("LEDs not updated (%d)", err);
    }
    return err;
}

static void displays_step(struct k_work *work) {
    ARG_UNUSED(work);
    k_mutex_lock(&defy_half_leds_lock, K_FOREVER);
    defy_half_leds_show();
    k_mutex_unlock(&defy_half_leds_lock);
}

#if IS_ENABLED(CONFIG_DEFY_BATTERY)
void defy_half_leds_battery_updated(void) { k_work_submit(&battery_update_work); }

static void battery_display_sample(const struct defy_battery_sample *sample) {
    struct defy_battery_reading reading;
    struct defy_battery_charger charger;
    battery_view = BATTERY_UNAVAILABLE;
    if (!sample || sample->age_ms > FRESH_MS) {
        return;
    }
    bool filtered = sample->words[0] == DEFY_BATTERY_ANALOG_FILTERED_TAG &&
                    (sample->status == DEFY_BATTERY_SAMPLE_ANALOG_OK ||
                     sample->status == DEFY_BATTERY_SAMPLE_ANALOG_ERROR);
    if (filtered) {
        if (defy_battery_sample_charger(sample, &charger) || !charger.status_valid ||
            charger.boost) {
            return;
        }
        if (charger.state != DEFY_BATTERY_CHARGE_READY ||
            (charger.fault != 0 && charger.fault != DEFY_BATTERY_CHARGER_FAULT_SLEEP)) {
            if (charger.state == DEFY_BATTERY_CHARGE_CHARGING && !charger.fault) {
                battery_view = BATTERY_CHARGING;
            } else if (charger.state == DEFY_BATTERY_CHARGE_DONE && !charger.fault) {
                battery_view = BATTERY_CELLS;
                battery_cells = ARRAY_SIZE(cells);
            } else if (charger.state == DEFY_BATTERY_CHARGE_FAULT) {
                battery_view = BATTERY_FAULT;
            }
            return;
        }
    }
    if (defy_battery_sample_reading(sample, &reading)) {
        return;
    }
    /* Stock Dygma's thresholds; at 20% or less the bottom cell breathes red. */
    battery_cells = reading.level > 70 ? 3 : reading.level > 40 ? 2 : reading.level > 20 ? 1 : 0;
    battery_view = battery_cells > 0 ? BATTERY_CELLS : BATTERY_LOW;
}

/* Whether sample is the battery display's refresh: newer than the request, and taken since. */
static bool requested_sample(const struct defy_battery_sample *sample, uint32_t generation,
                             int64_t now) {
    return sample->age_ms <= now - battery_requested_at &&
           ((int32_t)(generation - battery_request_generation) > 0 ||
            (generation == battery_request_generation &&
             (int32_t)(sample->sequence - battery_request_sequence) > 0));
}

/* Ends the warning, redrawing if it showed. */
static void end_warning(int64_t now) {
    bool visible = warning_shows(now);

    warning_until = 0;
    if (visible) {
        defy_half_leds_show();
    }
}

/* Steps the warning's two-sample confirmation with a new sample. */
static void warning_sample(const struct defy_battery_sample *sample, uint32_t generation,
                           int64_t now) {
    struct defy_battery_reading reading;

    if (warning_generation != 0 && (int32_t)(generation - warning_generation) < 0) {
        return;
    }
    if (warning_generation != generation) {
        warning_generation = generation;
        warning_sequence = 0;
        warning_low_count = warning_high_count = 0;
    } else if (warning_sequence != 0 && (int32_t)(sample->sequence - warning_sequence) <= 0) {
        return;
    }
    warning_sequence = sample->sequence;
    int err = defy_battery_sample_reading(sample, &reading);
    if (err) {
        warning_low_count = warning_high_count = 0;
        warning_pending = false;
    } else if (reading.level <= WARNING_LEVEL) {
        warning_high_count = 0;
        warning_low_count = MIN(warning_low_count + 1, WARNING_SAMPLES);
        if (!warning_low && warning_low_count == WARNING_SAMPLES) {
            warning_low = true;
            warning_pending = true;
        }
    } else if (reading.level >= REARM_LEVEL) {
        warning_low_count = 0;
        warning_high_count = MIN(warning_high_count + 1, WARNING_SAMPLES);
        if (warning_high_count == WARNING_SAMPLES) {
            warning_low = false;
            warning_pending = false;
        }
    } else {
        warning_low_count = warning_high_count = 0;
    }
    if (err || !warning_low) {
        end_warning(now);
    }
    if (warning_pending && !lighting_hidden() && defy_startup_leds_allowed()) {
        defy_half_leds_show();
    }
}

static void battery_updated(struct k_work *work) {
    ARG_UNUSED(work);
    struct defy_battery_sample sample;
    uint32_t generation = 0;
    k_mutex_lock(&defy_half_leds_lock, K_FOREVER);
    int err = defy_battery_hci_get_generation(&sample, &generation);
    int64_t now = k_uptime_get();
    if (battery_shows(now)) {
        bool requested = !err && requested_sample(&sample, generation, now);
        battery_display_sample(requested ? &sample : NULL);
        defy_half_leds_show();
    }
    if (err) {
        warning_low_count = warning_high_count = 0;
        warning_pending = false;
        end_warning(now);
    } else {
        warning_sample(&sample, generation, now);
    }
    k_mutex_unlock(&defy_half_leds_lock);
}

bool defy_half_leds_battery_active(void) {
    k_mutex_lock(&defy_half_leds_lock, K_FOREVER);
    bool active = battery_until > k_uptime_get() && defy_startup_leds_allowed();
    k_mutex_unlock(&defy_half_leds_lock);
    return active;
}

int defy_half_leds_battery_show(void) {
    k_mutex_lock(&defy_half_leds_lock, K_FOREVER);
    if (!ready || !defy_startup_leds_allowed()) {
        int err = ready ? -EBUSY : -ENODEV;
        k_mutex_unlock(&defy_half_leds_lock);
        return err;
    }

    struct defy_battery_sample sample;
    uint32_t generation = 0;
    bool have_sample = defy_battery_hci_get_generation(&sample, &generation) == 0;
    battery_request_generation = generation;
    battery_request_sequence = have_sample ? sample.sequence : 0;
    battery_requested_at = k_uptime_get();
    /* A filtered sample without an estimate waits for the sample the refresh brings. */
    struct defy_battery_reading reading;
    bool unestimated = have_sample && sample.words[0] == DEFY_BATTERY_ANALOG_FILTERED_TAG &&
                       defy_battery_sample_reading(&sample, &reading) != 0;
    battery_display_sample(have_sample && !unestimated ? &sample : NULL);
    battery_until = k_uptime_get() + BATTERY_SHOW_MS;
    defy_half_leds_show();
    k_mutex_unlock(&defy_half_leds_lock);
    defy_battery_hci_refresh();
    return 0;
}
#endif
