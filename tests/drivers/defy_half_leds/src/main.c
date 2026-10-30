/* SPDX-License-Identifier: MIT */

#include <errno.h>
#include <string.h>
#include <zephyr/device.h>
#include <zephyr/drivers/led_strip.h>
#include <zephyr/kernel.h>
#include <zephyr/ztest.h>
#include <zmk/events/split_peripheral_status_changed.h>
#include <dygma/defy_battery.h>
#include <dygma/defy_lighting.h>

#include "fake_uptime.h"

static struct defy_battery_sample sample;
static int sample_error;
static unsigned int sample_reads;
static uint32_t sample_generation;
static unsigned int update_submissions;
static unsigned int refresh_requests;

int defy_battery_hci_refresh(void) {
    refresh_requests++;
    return 0;
}
static struct defy_lighting_state accepted_lighting;
static struct led_rgb rendered[64];
static unsigned int renders;
static struct {
    struct k_work_delayable *work;
    int64_t due;
    unsigned int calls;
} scheduled[8];

DEVICE_DT_DEFINE(DT_NODELABEL(defy_leds), NULL, NULL, NULL, NULL, POST_KERNEL, 50, NULL);

static int test_submit(struct k_work *work) {
    ARG_UNUSED(work);
    update_submissions++;
    return 1;
}

static int test_reschedule(struct k_work_delayable *work, k_timeout_t delay) {
    for (size_t i = 0; i < ARRAY_SIZE(scheduled); i++) {
        if (!scheduled[i].work || scheduled[i].work == work) {
            scheduled[i].work = work;
            scheduled[i].due = now + k_ticks_to_ms_ceil64(delay.ticks);
            scheduled[i].calls++;
            return 1;
        }
    }
    zassert_unreachable();
    return -ENOMEM;
}

/* As k_work_schedule(): work still pending keeps its time. */
static int test_schedule(struct k_work_delayable *work, k_timeout_t delay) {
    for (size_t i = 0; i < ARRAY_SIZE(scheduled); i++) {
        if (scheduled[i].work == work && scheduled[i].due > now) {
            return 0;
        }
    }
    return test_reschedule(work, delay);
}

static int test_cancel(struct k_work_delayable *work) {
    for (size_t i = 0; i < ARRAY_SIZE(scheduled); i++) {
        if (scheduled[i].work == work) {
            scheduled[i].due = -1;
        }
    }
    return 0;
}

/* The LED strip's answer to the next render. */
static int render_error;

static int test_render(const struct device *device, struct led_rgb *data, size_t count) {
    zassert_equal(device, DEVICE_DT_GET(DT_NODELABEL(defy_leds)));
    zassert_equal(count, ARRAY_SIZE(rendered));
    memcpy(rendered, data, sizeof(rendered));
    renders++;
    return render_error;
}

int defy_battery_hci_get(struct defy_battery_sample *result) {
    sample_reads++;
    if (!sample_error) {
        *result = sample;
    }
    return sample_error;
}

int defy_battery_hci_get_generation(struct defy_battery_sample *result, uint32_t *generation) {
    *generation = sample_generation;
    return defy_battery_hci_get(result);
}

bool defy_lighting_peripheral_get(struct defy_lighting_state *state) {
    if (!accepted_lighting.boot) {
        return false;
    }
    *state = accepted_lighting;
    return true;
}

const struct zmk_event_type zmk_event_zmk_split_peripheral_status_changed;

struct zmk_split_peripheral_status_changed *as_zmk_split_peripheral_status_changed(
    const zmk_event_t *event) {
    return &CONTAINER_OF(event, struct zmk_split_peripheral_status_changed_event, header)->data;
}

#define k_work_reschedule test_reschedule
#define k_work_schedule test_schedule
#define k_work_cancel_delayable test_cancel
#define k_work_submit test_submit
#define led_strip_update_rgb test_render
#include "../../../../lighting/defy_startup_leds.c"
/* One translation unit here, whose log module defy_startup_leds.c registered. */
#undef LOG_MODULE_DECLARE
#define LOG_MODULE_DECLARE(...)
#include "../../../../lighting/defy_radio_install_leds.c"
#include "../../../../lighting/defy_half_leds.c"
#undef led_strip_update_rgb
#undef k_work_cancel_delayable
#undef k_work_schedule
#undef k_work_reschedule
#undef k_work_submit
#undef k_uptime_get

/* The everyday colours of the Neuron's lighting state in these tests. */
static const struct defy_lighting_colour everyday_keys = {24, 16, 8};
static const struct defy_lighting_colour everyday_underglow = {0, 0, 6};

/* What a lighting state shows, on or off, every key in one colour. */
static struct defy_lighting_view view_of(bool enabled, struct defy_lighting_colour keys,
                                         struct defy_lighting_colour underglow) {
    struct defy_lighting_view view = {.enabled = enabled, .underglow = underglow};
    for (size_t i = 0; i < ARRAY_SIZE(view.keys); i++) {
        view.keys[i] = keys;
    }
    return view;
}

/* The Neuron's lighting state, on or off, in the everyday colours. */
static int lighting_set(bool enabled) {
    const struct defy_lighting_view view = view_of(enabled, everyday_keys, everyday_underglow);
    return defy_half_leds_lighting_set(&view);
}

static int64_t due(struct k_work_delayable *work) {
    for (size_t i = 0; i < ARRAY_SIZE(scheduled); i++) {
        if (scheduled[i].work == work) {
            return scheduled[i].due;
        }
    }
    return -1;
}

static unsigned int schedule_count(struct k_work_delayable *work) {
    for (size_t i = 0; i < ARRAY_SIZE(scheduled); i++) {
        if (scheduled[i].work == work) {
            return scheduled[i].calls;
        }
    }
    return 0;
}

static void expect_bar(size_t count, struct led_rgb colour) {
    for (size_t i = 0; i < ARRAY_SIZE(rendered); i++) {
        struct led_rgb expected = i < count ? colour : (struct led_rgb){0};
        zassert_equal(rendered[i].r, expected.r, "red key %u", (unsigned int)i);
        zassert_equal(rendered[i].g, expected.g, "green key %u", (unsigned int)i);
        zassert_equal(rendered[i].b, expected.b, "blue key %u", (unsigned int)i);
    }
}

static const struct led_rgb cell_green = {.g = 128};
static const struct led_rgb cell_amber = {.r = 128, .g = 32};
static const struct led_rgb cell_red = {.r = 128};

/* The first count battery cells, bottom up, in colour; every other LED dark. */
static void expect_cells(size_t count, struct led_rgb colour) {
    static const uint8_t bottom_up[] = {20, 13, 6};
    for (size_t i = 0; i < ARRAY_SIZE(rendered); i++) {
        struct led_rgb expected = {0};
        for (size_t c = 0; c < count; c++) {
            if (i == bottom_up[c]) {
                expected = colour;
            }
        }
        zassert_equal(rendered[i].r, expected.r, "red LED %u", (unsigned int)i);
        zassert_equal(rendered[i].g, expected.g, "green LED %u", (unsigned int)i);
        zassert_equal(rendered[i].b, expected.b, "blue LED %u", (unsigned int)i);
    }
}

/*
 * The breathing level at ms, 80 to 208, before scaling, on the same curve as Dygma's
 * firmware: a triangle wave over 256 << shift ms, eased by 3t^2 - 2t^3 in 8-bit steps.
 * Its battery display shifts the time by 4, its pairing screen by 3.
 */
static uint8_t stock_breath(int64_t ms, unsigned int shift) {
    unsigned int phase = (ms >> shift) & 0xff;
    unsigned int t = phase < 128 ? 2 * phase : 510 - 2 * phase;
    unsigned int t2 = t * t / 256;
    unsigned int t3 = t2 * t / 256;

    return 80 + (3 * t2 - 2 * t3) / 2;
}

/* The bottom cell breathing red, scaled so that 208 is 128. */
static void expect_breathing(void) {
    expect_cells(1, (struct led_rgb){.r = stock_breath(now, 4) * 128 / 208});
}

static void before(void *fixture) {
    ARG_UNUSED(fixture);
    now = 1000;
    sample = (struct defy_battery_sample){
        .sequence = 1,
        .words = {[DEFY_BATTERY_DEVNAME] = 0x4031,
                  [DEFY_BATTERY_DESIGNCAP] = 0x1284,
                  [DEFY_BATTERY_REPSOC] = 50 * 256,
                  [DEFY_BATTERY_VCELL] = 48000},
    };
    sample_error = 0;
    sample_generation = 1;
    accepted_lighting = (struct defy_lighting_state){.boot = 42, .revision = 1};
    update_submissions = 0;
    sample_reads = renders = 0;
    render_error = 0;
    lighting_failed = false;
    memset(scheduled, 0, sizeof(scheduled));
    memset(&answer, 0, sizeof(answer));
    memset(&sessions, 0, sizeof(sessions));
    memset(&radio_trace, 0, sizeof(radio_trace));
    connected = dark = blink = false;
    lighting_override = connection_showing = false;
    lighting = (struct defy_lighting_view){0};
    reached = lit = battery_cells = 0;
    battery_until = 0;
    refresh_requests = 0;
    battery_view = BATTERY_UNAVAILABLE;
    warning_generation = warning_sequence = 0;
    warning_low_count = warning_high_count = 0;
    warning_low = warning_pending = false;
    warning_until = host_until = host_cleared_until = 0;
    host_feedback = (struct defy_host_feedback){0};
    before_failure = DEFY_STARTUP_RUNNING;
    zassert_ok(defy_startup_init());
}

ZTEST(half_leds, test_half_charge_lights_two_cells_without_animation) {
    zassert_ok(defy_half_leds_battery_show());
    expect_cells(2, cell_green);
    zassert_equal(sample_reads, 1);
    /* No frame steps before the display ends. */
    zassert_equal(due(&displays_work), now + 5000);
}

ZTEST(half_leds, test_stock_thresholds_fill_cells_bottom_up) {
    const uint8_t levels[] = {21, 40, 41, 70, 71, 100};
    const uint8_t counts[] = {1, 1, 2, 2, 3, 3};
    for (size_t i = 0; i < ARRAY_SIZE(levels); i++) {
        sample.words[DEFY_BATTERY_REPSOC] = levels[i] * 256u;
        zassert_ok(defy_half_leds_battery_show());
        expect_cells(counts[i], cell_green);
    }
    const uint16_t low[] = {20 * 256, 21 * 256 - 1};
    for (size_t i = 0; i < ARRAY_SIZE(low); i++) {
        sample.words[DEFY_BATTERY_REPSOC] = low[i];
        zassert_ok(defy_half_leds_battery_show());
        expect_breathing();
        zassert_equal(due(&displays_work), now + 40);
    }
}

ZTEST(half_leds, test_missing_or_invalid_sample_is_amber) {
    sample_error = -ENODATA;
    zassert_ok(defy_half_leds_battery_show());
    expect_cells(1, cell_amber);
    sample_error = 0;
    sample.words[DEFY_BATTERY_VCELL] = 0;
    zassert_ok(defy_half_leds_battery_show());
    expect_cells(1, cell_amber);
}

ZTEST(half_leds, test_dark_state_restored_without_extending_deadline) {
    now = due(&dark_work);
    go_dark(&dark_work.work);
    int64_t original_due = due(&dark_work);
    unsigned int original_count = schedule_count(&dark_work);
    zassert_ok(defy_half_leds_battery_show());
    expect_cells(2, cell_green);
    zassert_true(dark);
    now += 5000;
    displays_step(&displays_work.work);
    expect_bar(0, (struct led_rgb){0});
    zassert_equal(due(&dark_work), original_due);
    zassert_equal(schedule_count(&dark_work), original_count);
}

ZTEST(half_leds, test_expiry_restores_latest_connection_state) {
    zassert_ok(defy_half_leds_battery_show());
    now += 1000;
    struct zmk_split_peripheral_status_changed_event event = {.data.connected = true};
    zassert_equal(on_split_status(&event.header), ZMK_EV_EVENT_BUBBLE);
    expect_cells(2, cell_green);
    int64_t connection_due = due(&dark_work);
    now += 4000;
    displays_step(&displays_work.work);
    expect_bar(64, (struct led_rgb){.g = 24, .b = 24});
    zassert_equal(due(&dark_work), connection_due);
}

ZTEST(half_leds, test_repeated_press_refreshes_and_old_expiry_does_not_clear) {
    zassert_ok(defy_half_leds_battery_show());
    now += 4000;
    sample.words[DEFY_BATTERY_REPSOC] = 100 * 256;
    zassert_ok(defy_half_leds_battery_show());
    now += 1000;
    displays_step(&displays_work.work);
    expect_cells(3, cell_green);
    zassert_equal(due(&displays_work), now + 4000);
    now += 4000;
    displays_step(&displays_work.work);
    expect_bar(64, (struct led_rgb){.b = 24});
    zassert_equal(sample_reads, 2);
}

ZTEST(half_leds, test_failure_and_install_preempt_active_overlay) {
    zassert_ok(defy_half_leds_battery_show());
    defy_startup_report(DEFY_STARTUP_RADIO_INSTALLING);
    expect_bar(0, (struct led_rgb){0});
    zassert_equal(defy_half_leds_battery_show(), -EBUSY);
    defy_startup_report(DEFY_STARTUP_RADIO_FAILED);
    zassert_equal(rendered[63].r, 24);
    zassert_equal(rendered[63].g, 0);
    zassert_equal(defy_half_leds_battery_show(), -EBUSY);
    zassert_equal(sample_reads, 1);
}

ZTEST(half_leds, test_early_radio_stages_keep_priority) {
    defy_startup_report(DEFY_STARTUP_RADIO_BOOTLOADER);
    zassert_equal(defy_half_leds_battery_show(), -EBUSY);
    defy_startup_report(DEFY_STARTUP_RADIO_READY);
    radio_trace.complete = true;
    zassert_ok(defy_half_leds_battery_show());
    expect_cells(2, cell_green);
}

/* Each report row's first count LEDs in colour, the rest of the row dark. */
static void expect_row(size_t first, size_t length, size_t count, struct led_rgb colour) {
    for (size_t i = first; i < first + length; i++) {
        struct led_rgb expected = i - first < count ? colour : (struct led_rgb){0};
        zassert_mem_equal(&rendered[i], &expected, sizeof(expected), "LED %u", (unsigned int)i);
    }
}

ZTEST(half_leds, test_failure_report_shows_the_sessions_and_the_first_answer) {
    defy_startup_radio_answer(0, 0);
    defy_startup_radio_answer(2, 0x82);
    defy_startup_radio_answer(2, 0x85);
    defy_startup_radio_session(-EPROTO);
    now += 500;
    defy_startup_radio_session(0);
    defy_startup_report(DEFY_STARTUP_RADIO_FAILED);
    /* The first session failed at 1000 ms with a protocol error; two sessions in all. */
    expect_row(0, 7, 2, (struct led_rgb){.r = LEVEL});
    expect_row(7, 7, 7, (struct led_rgb){.r = LEVEL, .g = LEVEL, .b = LEVEL});
    expect_row(14, 7, 5, (struct led_rgb){.r = LEVEL, .g = LEVEL, .b = LEVEL});
    /* Dygma's protocol in state 0x82. */
    expect_row(21, 6, 3, (struct led_rgb){.r = LEVEL, .g = LEVEL});
    expect_row(27, 8, 2, (struct led_rgb){.r = LEVEL, .b = LEVEL});
    zassert_mem_equal(&rendered[63], &(struct led_rgb){.r = LEVEL}, sizeof(struct led_rgb));
}

ZTEST(half_leds, test_failure_blink_resumes_when_the_display_wakes) {
    const struct led_rgb failed = {.r = LEVEL}, running = {.b = LEVEL};

    defy_startup_report(DEFY_STARTUP_RADIO_FAILED);
    zassert_mem_equal(&rendered[KEYS], &failed, sizeof(failed));
    now = due(&blink_work);
    blink_toggle(&blink_work.work);
    zassert_mem_equal(&rendered[KEYS], &running, sizeof(running));
    /* Dark after ten minutes, and the blink stops. */
    now = due(&dark_work);
    go_dark(&dark_work.work);
    blink_toggle(&blink_work.work);
    zassert_true(due(&blink_work) <= now);
    /* A cable in and out again wakes the display, and the underglow blinks on. */
    defy_startup_split_connected(true);
    defy_startup_split_connected(false);
    zassert_equal(due(&blink_work), now + 500);
    zassert_mem_equal(&rendered[KEYS], &running, sizeof(running));
    now = due(&blink_work);
    blink_toggle(&blink_work.work);
    zassert_mem_equal(&rendered[KEYS], &failed, sizeof(failed));
}

ZTEST(half_leds, test_active_getter_tracks_expiry_without_waiting_for_worker) {
    zassert_false(defy_half_leds_battery_active());
    zassert_ok(defy_half_leds_battery_show());
    zassert_true(defy_half_leds_battery_active());
    now += 4999;
    zassert_true(defy_half_leds_battery_active());
    now++;
    zassert_false(defy_half_leds_battery_active());
    zassert_not_equal(battery_until, 0);
    displays_step(&displays_work.work);
    zassert_false(defy_half_leds_battery_active());
}

ZTEST(half_leds, test_protected_stage_overlay_does_not_veto_sleep) {
    zassert_ok(defy_half_leds_battery_show());
    zassert_true(defy_half_leds_battery_active());
    defy_startup_report(DEFY_STARTUP_RADIO_INSTALLING);
    zassert_false(defy_half_leds_battery_active());
    defy_startup_report(DEFY_STARTUP_RADIO_FAILED);
    zassert_false(defy_half_leds_battery_active());
}

ZTEST(half_leds, test_hybrid_call_and_zmk_event_set_the_same_connection) {
    defy_startup_report(DEFY_STARTUP_RADIO_READY);
    defy_startup_split_connected(true);
    expect_bar(ARRAY_SIZE(rendered), (struct led_rgb){.g = LEVEL, .b = LEVEL});
    unsigned int previous_renders = renders;
    struct zmk_split_peripheral_status_changed_event event = {.data.connected = true};
    zassert_equal(on_split_status(&event.header), ZMK_EV_EVENT_BUBBLE);
    zassert_equal(renders, previous_renders);
    event.data.connected = false;
    zassert_equal(on_split_status(&event.header), ZMK_EV_EVENT_BUBBLE);
    expect_bar(ARRAY_SIZE(rendered), (struct led_rgb){.g = LEVEL});
    defy_startup_split_connected(true);
    expect_bar(ARRAY_SIZE(rendered), (struct led_rgb){.g = LEVEL, .b = LEVEL});
    defy_startup_split_connected(false);
    expect_bar(ARRAY_SIZE(rendered), (struct led_rgb){.g = LEVEL});
}

ZTEST(half_leds, test_lighting_off_survives_normal_status_changes) {
    zassert_ok(lighting_set(false));
    expect_bar(0, (struct led_rgb){0});
    defy_startup_report(DEFY_STARTUP_RADIO_READY);
    expect_bar(0, (struct led_rgb){0});
    struct zmk_split_peripheral_status_changed_event event = {.data.connected = true};
    zassert_equal(on_split_status(&event.header), ZMK_EV_EVENT_BUBBLE);
    expect_bar(0, (struct led_rgb){0});
    event.data.connected = false;
    zassert_equal(on_split_status(&event.header), ZMK_EV_EVENT_BUBBLE);
    expect_bar(0, (struct led_rgb){0});
    now = due(&dark_work);
    go_dark(&dark_work.work);
    event.data.connected = true;
    zassert_equal(on_split_status(&event.header), ZMK_EV_EVENT_BUBBLE);
    expect_bar(0, (struct led_rgb){0});
    zassert_false(defy_half_leds_battery_active());
}

ZTEST(half_leds, test_connected_lighting_on_bypasses_dark_without_extending_deadlines) {
    struct zmk_split_peripheral_status_changed_event event = {.data.connected = true};
    zassert_equal(on_split_status(&event.header), ZMK_EV_EVENT_BUBBLE);
    now = due(&dark_work);
    go_dark(&dark_work.work);
    expect_bar(0, (struct led_rgb){0});
    int64_t original_due = due(&dark_work);
    unsigned int original_count = schedule_count(&dark_work);
    zassert_ok(lighting_set(true));
    expect_bar(ARRAY_SIZE(rendered), (struct led_rgb){.g = 24, .b = 24});
    zassert_true(dark);
    zassert_equal(due(&dark_work), original_due);
    zassert_equal(schedule_count(&dark_work), original_count);
    zassert_equal(on_split_status(&event.header), ZMK_EV_EVENT_BUBBLE);
    expect_bar(ARRAY_SIZE(rendered), (struct led_rgb){.g = 24, .b = 24});
    zassert_true(dark);
    zassert_equal(due(&dark_work), original_due);
    zassert_equal(schedule_count(&dark_work), original_count);
    now = due(&dark_work);
    go_dark(&dark_work.work);
    expect_bar(ARRAY_SIZE(rendered), (struct led_rgb){.g = 24, .b = 24});
    zassert_false(defy_half_leds_battery_active());
}

/* The keys in the everyday key colour, the underglow in its own. */
static void expect_everyday(struct defy_lighting_colour keys,
                            struct defy_lighting_colour underglow) {
    for (size_t i = 0; i < ARRAY_SIZE(rendered); i++) {
        struct defy_lighting_colour expected = i < KEYS ? keys : underglow;
        zassert_equal(rendered[i].r, expected.r, "red LED %u", (unsigned int)i);
        zassert_equal(rendered[i].g, expected.g, "green LED %u", (unsigned int)i);
        zassert_equal(rendered[i].b, expected.b, "blue LED %u", (unsigned int)i);
    }
}

/* Connects with the radio up, and lets the two seconds of cyan pass. */
static void connect_and_settle(void) {
    defy_startup_report(DEFY_STARTUP_RADIO_READY);
    defy_startup_split_connected(true);
    expect_bar(ARRAY_SIZE(rendered), (struct led_rgb){.g = LEVEL, .b = LEVEL});
    zassert_equal(due(&connection_work), now + 2000);
    now = due(&connection_work);
    connection_shown(&connection_work.work);
}

ZTEST(half_leds, test_connection_shows_cyan_then_the_everyday_colours) {
    zassert_ok(lighting_set(true));
    connect_and_settle();
    expect_everyday(everyday_keys, everyday_underglow);
    unsigned int previous_renders = renders;
    connection_shown(&connection_work.work);
    zassert_equal(renders, previous_renders);
}

ZTEST(half_leds, test_cyan_stays_until_the_lighting_state_arrives) {
    connect_and_settle();
    expect_bar(ARRAY_SIZE(rendered), (struct led_rgb){.g = LEVEL, .b = LEVEL});
    zassert_ok(lighting_set(true));
    expect_everyday(everyday_keys, everyday_underglow);
    zassert_ok(defy_half_leds_lighting_set(NULL));
    zassert_false(lighting_override);
    expect_bar(ARRAY_SIZE(rendered), (struct led_rgb){.g = LEVEL, .b = LEVEL});
}

ZTEST(half_leds, test_new_colours_render_and_the_same_look_does_not) {
    zassert_ok(lighting_set(true));
    connect_and_settle();
    unsigned int previous_renders = renders;
    struct defy_lighting_view view = view_of(true, everyday_keys, everyday_underglow);
    zassert_ok(defy_half_leds_lighting_set(&view));
    zassert_equal(renders, previous_renders);
    view = view_of(true, (struct defy_lighting_colour){1, 2, 3}, (struct defy_lighting_colour){0});
    zassert_ok(defy_half_leds_lighting_set(&view));
    expect_everyday(view.keys[0], view.underglow);
    view.enabled = false;
    zassert_ok(defy_half_leds_lighting_set(&view));
    expect_bar(0, (struct led_rgb){0});
}

ZTEST(half_leds, test_a_failed_draw_is_reported_and_drawn_again) {
    zassert_ok(lighting_set(true));
    connect_and_settle();
    struct defy_lighting_view view =
        view_of(true, (struct defy_lighting_colour){1, 2, 3}, everyday_underglow);
    render_error = -EIO;
    zassert_equal(defy_half_leds_lighting_set(&view), -EIO);
    /* The link's retry brings the same view, which draws again. */
    render_error = 0;
    unsigned int previous_renders = renders;
    zassert_ok(defy_half_leds_lighting_set(&view));
    zassert_equal(renders, previous_renders + 1);
    zassert_ok(defy_half_leds_lighting_set(&view));
    zassert_equal(renders, previous_renders + 1);
}

ZTEST(half_leds, test_each_key_takes_its_own_colour_from_the_view) {
    zassert_ok(lighting_set(true));
    connect_and_settle();
    unsigned int previous_renders = renders;
    struct defy_lighting_view view = view_of(true, everyday_keys, everyday_underglow);
    view.keys[0] = (struct defy_lighting_colour){9, 0, 0};
    view.keys[KEYS - 1] = (struct defy_lighting_colour){0, 0, 9};
    zassert_ok(defy_half_leds_lighting_set(&view));
    zassert_equal(renders, previous_renders + 1);
    zassert_equal(rendered[0].r, 9);
    zassert_equal(rendered[0].g, 0);
    zassert_equal(rendered[1].r, everyday_keys.r);
    zassert_equal(rendered[KEYS - 1].b, 9);
    zassert_equal(rendered[KEYS].b, everyday_underglow.b);
    /* A change to one key alone draws again. */
    view.keys[17].g = 1;
    zassert_ok(defy_half_leds_lighting_set(&view));
    zassert_equal(renders, previous_renders + 2);
    zassert_equal(rendered[17].g, 1);
}

ZTEST(half_leds, test_each_connection_shows_cyan_again) {
    zassert_ok(lighting_set(true));
    connect_and_settle();
    defy_startup_split_connected(false);
    expect_bar(ARRAY_SIZE(rendered), (struct led_rgb){.g = LEVEL});
    zassert_false(connection_showing);
    zassert_equal(due(&connection_work), -1);
    now += 1000;
    defy_startup_split_connected(true);
    expect_bar(ARRAY_SIZE(rendered), (struct led_rgb){.g = LEVEL, .b = LEVEL});
    zassert_equal(due(&connection_work), now + 2000);
    now = due(&connection_work);
    connection_shown(&connection_work.work);
    expect_everyday(everyday_keys, everyday_underglow);
}

ZTEST(half_leds, test_a_failed_radio_keeps_cyan_on_the_cable) {
    zassert_ok(lighting_set(true));
    defy_startup_report(DEFY_STARTUP_RADIO_FAILED);
    defy_startup_split_connected(true);
    now = due(&connection_work);
    connection_shown(&connection_work.work);
    expect_bar(ARRAY_SIZE(rendered), (struct led_rgb){.g = LEVEL, .b = LEVEL});
}

ZTEST(half_leds, test_disconnected_lighting_on_expires_and_reconnect_restores_preference) {
    defy_startup_report(DEFY_STARTUP_RADIO_READY);
    struct zmk_split_peripheral_status_changed_event event = {.data.connected = true};
    zassert_equal(on_split_status(&event.header), ZMK_EV_EVENT_BUBBLE);
    zassert_ok(lighting_set(true));
    now += 1000;
    event.data.connected = false;
    zassert_equal(on_split_status(&event.header), ZMK_EV_EVENT_BUBBLE);
    expect_bar(ARRAY_SIZE(rendered), (struct led_rgb){.g = LEVEL});
    int64_t expiry = due(&dark_work);
    zassert_equal(expiry, now + 120000);
    now = expiry;
    go_dark(&dark_work.work);
    expect_bar(0, (struct led_rgb){0});
    zassert_true(lighting_override);
    zassert_true(lighting.enabled);
    zassert_ok(lighting_set(true));
    expect_bar(0, (struct led_rgb){0});
    zassert_equal(due(&dark_work), expiry);
    event.data.connected = true;
    zassert_equal(on_split_status(&event.header), ZMK_EV_EVENT_BUBBLE);
    expect_bar(ARRAY_SIZE(rendered), (struct led_rgb){.g = LEVEL, .b = LEVEL});
    now = due(&dark_work);
    go_dark(&dark_work.work);
    expect_bar(ARRAY_SIZE(rendered), (struct led_rgb){.g = LEVEL, .b = LEVEL});
}

ZTEST(half_leds, test_duplicate_disconnects_do_not_extend_or_restart_fallback) {
    struct zmk_split_peripheral_status_changed_event event = {.data.connected = true};
    zassert_equal(on_split_status(&event.header), ZMK_EV_EVENT_BUBBLE);
    zassert_ok(lighting_set(true));
    event.data.connected = false;
    zassert_equal(on_split_status(&event.header), ZMK_EV_EVENT_BUBBLE);
    int64_t expiry = due(&dark_work);
    unsigned int schedules = schedule_count(&dark_work);
    unsigned int previous_renders = renders;
    now = expiry - 1;
    zassert_equal(on_split_status(&event.header), ZMK_EV_EVENT_BUBBLE);
    zassert_equal(renders, previous_renders);
    zassert_equal(due(&dark_work), expiry);
    zassert_equal(schedule_count(&dark_work), schedules);
    now = expiry;
    go_dark(&dark_work.work);
    expect_bar(0, (struct led_rgb){0});
    now += 1000;
    previous_renders = renders;
    zassert_equal(on_split_status(&event.header), ZMK_EV_EVENT_BUBBLE);
    expect_bar(0, (struct led_rgb){0});
    zassert_equal(renders, previous_renders);
    zassert_equal(due(&dark_work), expiry);
    zassert_equal(schedule_count(&dark_work), schedules);
}

ZTEST(half_leds, test_battery_expiry_restores_lighting_off) {
    zassert_ok(lighting_set(false));
    zassert_ok(defy_half_leds_battery_show());
    expect_cells(2, cell_green);
    struct zmk_split_peripheral_status_changed_event event = {.data.connected = true};
    zassert_equal(on_split_status(&event.header), ZMK_EV_EVENT_BUBBLE);
    expect_cells(2, cell_green);
    now += 5000;
    displays_step(&displays_work.work);
    expect_bar(0, (struct led_rgb){0});
    zassert_false(defy_half_leds_battery_active());
}

ZTEST(half_leds, test_battery_expiry_restores_lighting_on_after_dark_deadline) {
    struct zmk_split_peripheral_status_changed_event event = {.data.connected = true};
    zassert_equal(on_split_status(&event.header), ZMK_EV_EVENT_BUBBLE);
    zassert_ok(lighting_set(true));
    now = due(&dark_work) - 2000;
    zassert_ok(defy_half_leds_battery_show());
    now += 2000;
    go_dark(&dark_work.work);
    expect_cells(2, cell_green);
    now += 3000;
    displays_step(&displays_work.work);
    expect_bar(ARRAY_SIZE(rendered), (struct led_rgb){.g = 24, .b = 24});
    zassert_false(defy_half_leds_battery_active());
}

ZTEST(half_leds, test_battery_overlay_preserves_disconnected_lighting_deadline) {
    struct zmk_split_peripheral_status_changed_event event = {.data.connected = true};
    zassert_equal(on_split_status(&event.header), ZMK_EV_EVENT_BUBBLE);
    zassert_ok(lighting_set(true));
    event.data.connected = false;
    zassert_equal(on_split_status(&event.header), ZMK_EV_EVENT_BUBBLE);
    int64_t expiry = due(&dark_work);
    unsigned int schedules = schedule_count(&dark_work);
    now = expiry - 2000;
    zassert_ok(defy_half_leds_battery_show());
    now = expiry;
    go_dark(&dark_work.work);
    expect_cells(2, cell_green);
    now += 3000;
    displays_step(&displays_work.work);
    expect_bar(0, (struct led_rgb){0});
    zassert_true(lighting.enabled);
    zassert_equal(due(&dark_work), expiry);
    zassert_equal(schedule_count(&dark_work), schedules);
}

ZTEST(half_leds, test_lighting_choices_do_not_override_protected_indications) {
    static const enum defy_startup_stage protected[] = {
        DEFY_STARTUP_RADIO_BOOTLOADER,
        DEFY_STARTUP_RADIO_INSTALLING,
        DEFY_STARTUP_RADIO_FAILED,
    };
    for (size_t i = 0; i < ARRAY_SIZE(protected); i++) {
        defy_startup_report(protected[i]);
        if (protected[i] == DEFY_STARTUP_RADIO_INSTALLING) {
            defy_startup_progress(1, 2);
        }
        struct led_rgb expected[ARRAY_SIZE(rendered)];
        memcpy(expected, rendered, sizeof(expected));
        zassert_ok(lighting_set(false));
        zassert_mem_equal(rendered, expected, sizeof(rendered));
        zassert_ok(lighting_set(true));
        zassert_mem_equal(rendered, expected, sizeof(rendered));
        zassert_equal(defy_half_leds_battery_show(), -EBUSY);
        go_dark(&dark_work.work);
        expect_bar(0, (struct led_rgb){0});
    }
}

static struct defy_host_feedback host_state(uint8_t flags) {
    return (struct defy_host_feedback){
        .boot = 42, .sequence = 1, .remaining_ms = 2000, .profile = 2, .flags = flags,
    };
}

static const struct led_rgb unlit = {0};
static const struct led_rgb green = {.g = 128}, amber = {.r = 128, .g = 32};
static const struct led_rgb white = {.r = 128, .g = 128, .b = 128}, yellow = {.r = 128, .g = 128};
static const struct led_rgb blue = {.b = 128}, purple = {.r = 128, .b = 128};
static const struct led_rgb cyan = {.g = 128, .b = 128}, red = {.r = 64};
static const struct led_rgb dim_blue = {.b = 32}, dim_green = {.g = 32};

/*
 * The top-row LED of each of ten profiles on the left half, which the suite renders as, and
 * on the right, 0 where the other half has it. LED 1 is next to the outer end: key 1 on the
 * left, key 0 on the right.
 */
static const uint8_t profile_leds[10] = {1, 2, 3, 4, 5, 0, 0, 0, 0, 0};
static const uint8_t right_profile_leds[10] = {0, 0, 0, 0, 0, 5, 4, 3, 2, 1};

/* The overlay: the profile's key, if this half has it, and the output marker on LED 6. */
static void expect_host(uint8_t profile, bool profile_connected, struct led_rgb marker) {
    for (size_t i = 0; i < ARRAY_SIZE(rendered); i++) {
        struct led_rgb expected = {0};
        if (profile_leds[profile] && i == profile_leds[profile]) {
            expected = profile_connected ? green : amber;
        } else if (i == 6) {
            expected = marker;
        }
        zassert_mem_equal(&rendered[i], &expected, sizeof(expected), "pixel %u", (unsigned int)i);
    }
}

ZTEST(half_leds, test_each_half_has_its_own_five_profiles) {
    for (uint8_t profile = 0; profile < 10; profile++) {
        zassert_equal(profile_pixel(DEFY_WIRED_LEFT, profile), profile_leds[profile]);
        zassert_equal(profile_pixel(DEFY_WIRED_RIGHT, profile), right_profile_leds[profile]);
    }
}

ZTEST(half_leds, test_host_profile_and_actual_fallback_are_separate) {
    struct defy_host_feedback state =
        host_state(DEFY_HOST_PREFERRED_BLE | DEFY_HOST_SELECTED_USB | DEFY_HOST_USB_READY);
    zassert_ok(defy_half_leds_host_show(&state));
    expect_host(2, false, purple);
    zassert_equal(due(&displays_work), now + 2000);
    zassert_false(defy_half_leds_battery_active());
}

ZTEST(half_leds, test_host_overlay_lights_each_profile_on_the_half_that_has_it) {
    unsigned int shown = 0;
    for (uint8_t profile = 0; profile < 10; profile++) {
        struct defy_host_feedback state = host_state(
            DEFY_HOST_PREFERRED_BLE | DEFY_HOST_SELECTED_BLE | DEFY_HOST_PROFILE_CONNECTED);
        state.sequence = profile + 1;
        state.profile = profile;
        zassert_ok(defy_half_leds_host_show(&state));
        expect_host(profile, true, blue);
        shown += profile_leds[profile] != 0;
    }
    zassert_equal(shown, 5);
}

ZTEST(half_leds, test_host_output_marker_tells_usb_from_bluetooth_and_ready_from_not) {
    const struct {
        uint8_t flags;
        bool connected;
        struct led_rgb marker;
    } cases[] = {
        {DEFY_HOST_USB_READY | DEFY_HOST_SELECTED_USB, false, white},
        {DEFY_HOST_USB_READY | DEFY_HOST_SELECTED_USB | DEFY_HOST_PROFILE_CONNECTED, true, white},
        {DEFY_HOST_PROFILE_CONNECTED | DEFY_HOST_SELECTED_BLE, true, amber},
        {0, false, amber},
        {DEFY_HOST_PREFERRED_BLE | DEFY_HOST_PROFILE_CONNECTED | DEFY_HOST_SELECTED_BLE, true,
         blue},
        {DEFY_HOST_PREFERRED_BLE | DEFY_HOST_USB_READY | DEFY_HOST_SELECTED_USB, false, purple},
        {DEFY_HOST_PREFERRED_BLE, false, purple},
        {DEFY_HOST_PREFERRED_NONE | DEFY_HOST_USB_READY, false, unlit},
    };
    for (size_t i = 0; i < ARRAY_SIZE(cases); i++) {
        struct defy_host_feedback state = host_state(cases[i].flags);
        state.sequence = i + 1;
        state.profile = 7;
        zassert_ok(defy_half_leds_host_show(&state));
        expect_host(7, cases[i].connected, cases[i].marker);
    }
}

ZTEST(half_leds, test_host_expiry_restores_latest_lighting_and_darkness) {
    struct defy_host_feedback state = host_state(DEFY_HOST_PREFERRED_BLE);
    zassert_ok(defy_half_leds_host_show(&state));
    go_dark(&dark_work.work);
    now += 2000;
    displays_step(&displays_work.work);
    expect_bar(0, (struct led_rgb){0});
    state.sequence++;
    zassert_ok(defy_half_leds_host_show(&state));
    zassert_ok(lighting_set(true));
    now += 2000;
    displays_step(&displays_work.work);
    expect_bar(0, (struct led_rgb){0});
    struct zmk_split_peripheral_status_changed_event event = {.data.connected = true};
    zassert_equal(on_split_status(&event.header), ZMK_EV_EVENT_BUBBLE);
    for (size_t i = 0; i < ARRAY_SIZE(rendered); i++) {
        zassert_equal(rendered[i].b, LEVEL);
    }
}

ZTEST(half_leds, test_host_duplicate_old_sequence_and_expired_request_do_not_extend) {
    struct defy_host_feedback state = host_state(DEFY_HOST_PREFERRED_BLE);
    state.sequence = 3;
    zassert_ok(defy_half_leds_host_show(&state));
    unsigned int initial_renders = renders;
    int64_t initial_expiry = host_until;
    now += 500;
    zassert_ok(defy_half_leds_host_show(&state));
    zassert_equal(renders, initial_renders);
    zassert_equal(host_until, initial_expiry);
    state.sequence = 2;
    zassert_equal(defy_half_leds_host_show(&state), -ESTALE);
    now = initial_expiry;
    displays_step(&displays_work.work);
    initial_renders = renders;
    state.sequence = 3;
    zassert_ok(defy_half_leds_host_show(&state));
    zassert_equal(renders, initial_renders);
    zassert_equal(host_until, initial_expiry);
}

ZTEST(half_leds, test_host_same_request_rejects_changed_state_and_can_only_shorten_deadline) {
    struct defy_host_feedback state = host_state(DEFY_HOST_PREFERRED_BLE);
    zassert_ok(defy_half_leds_host_show(&state));
    int64_t original = host_until;
    now += 100;
    state.flags |= DEFY_HOST_SELECTED_BLE | DEFY_HOST_PROFILE_CONNECTED;
    zassert_equal(defy_half_leds_host_show(&state), -EPROTO);
    zassert_equal(host_until, original);
    state.flags = DEFY_HOST_PREFERRED_BLE;
    state.profile++;
    zassert_equal(defy_half_leds_host_show(&state), -EPROTO);
    state.profile--;
    state.remaining_ms = 400;
    zassert_ok(defy_half_leds_host_show(&state));
    zassert_equal(host_until, now + 400);
}

ZTEST(half_leds, test_host_new_request_supersedes_and_old_expiry_rechecks) {
    struct defy_host_feedback state = host_state(DEFY_HOST_PREFERRED_BLE);
    zassert_ok(defy_half_leds_host_show(&state));
    now += 1000;
    state.sequence++;
    state.profile = 4;
    zassert_ok(defy_half_leds_host_show(&state));
    now += 1000;
    displays_step(&displays_work.work);
    zassert_equal(host_until, now + 1000);
    expect_host(4, false, purple);
    state.boot++;
    state.sequence = 1;
    accepted_lighting.boot = state.boot;
    zassert_ok(defy_half_leds_host_show(&state));
    zassert_equal(host_until, now + 2000);
}

ZTEST(half_leds, test_host_requires_established_lighting_identity) {
    struct defy_host_feedback state = host_state(DEFY_HOST_PREFERRED_BLE);
    accepted_lighting.boot = 0;
    unsigned int original_renders = renders;
    zassert_equal(defy_half_leds_host_show(&state), -ESTALE);
    zassert_equal(renders, original_renders);
    zassert_equal(host_until, 0);
    zassert_equal(due(&displays_work), -1);
}

ZTEST(half_leds, test_host_new_lighting_boot_rejects_delayed_retired_request) {
    struct defy_host_feedback old = host_state(DEFY_HOST_PREFERRED_BLE);
    zassert_ok(defy_half_leds_host_show(&old));
    struct defy_host_feedback current = old;
    current.boot++;
    current.profile = 4;
    accepted_lighting.boot = current.boot;
    now += 500;
    zassert_ok(defy_half_leds_host_show(&current));
    int64_t current_expiry = host_until;
    unsigned int current_renders = renders;
    old.sequence++;
    zassert_equal(defy_half_leds_host_show(&old), -ESTALE);
    zassert_equal(host_feedback.boot, current.boot);
    zassert_equal(host_until, current_expiry);
    zassert_equal(renders, current_renders);
    expect_host(4, false, purple);
}

ZTEST(half_leds, test_battery_keeps_five_seconds_and_host_cannot_preempt) {
    zassert_ok(defy_half_leds_battery_show());
    int64_t expiry = battery_until;
    struct defy_host_feedback state = host_state(DEFY_HOST_PREFERRED_BLE);
    now += 1000;
    zassert_ok(defy_half_leds_host_show(&state));
    expect_cells(2, cell_green);
    zassert_equal(battery_until, expiry);
    now += 2000;
    displays_step(&displays_work.work);
    expect_cells(2, cell_green);
    now = expiry;
    displays_step(&displays_work.work);
    expect_bar(ARRAY_SIZE(rendered), (struct led_rgb){.b = LEVEL});
}

ZTEST(half_leds, test_host_overlay_protected_and_unready_rejections_do_not_schedule) {
    struct defy_host_feedback state = host_state(DEFY_HOST_PREFERRED_BLE);
    ready = false;
    zassert_equal(defy_half_leds_host_show(&state), -ENODEV);
    ready = true;
    defy_startup_report(DEFY_STARTUP_RADIO_INSTALLING);
    zassert_equal(defy_half_leds_host_show(&state), -EBUSY);
    zassert_equal(host_until, 0);
    defy_startup_report(DEFY_STARTUP_RADIO_READY);
    zassert_ok(defy_half_leds_host_show(&state));
    defy_startup_report(DEFY_STARTUP_RADIO_FAILED);
    zassert_equal(defy_half_leds_host_show(&state), -EBUSY);
    zassert_equal(rendered[KEYS].r, LEVEL);
}

ZTEST(half_leds, test_host_rejects_invalid_state_without_mutation) {
    struct defy_host_feedback state = host_state(DEFY_HOST_PREFERRED_BLE);
    state.remaining_ms = 2001;
    zassert_equal(defy_half_leds_host_show(&state), -EINVAL);
    zassert_equal(defy_half_leds_host_show(NULL), -EINVAL);
    zassert_equal(host_until, 0);
    zassert_equal(due(&displays_work), -1);
}

static void observe_level(uint8_t level) {
    sample.sequence++;
    sample.words[DEFY_BATTERY_REPSOC] = level * 256u;
    unsigned int reads = sample_reads;
    unsigned int previous = renders;
    defy_half_leds_battery_updated();
    zassert_equal(sample_reads, reads);
    zassert_equal(renders, previous);
    battery_updated(&battery_update_work);
}

/* The warning's first frame: all three cells red. */
static void expect_warning(void) { expect_cells(3, cell_red); }

ZTEST(half_leds, test_low_warning_needs_two_samples_and_does_not_change_activity_deadlines) {
    int64_t original_dark = due(&dark_work);
    observe_level(15);
    zassert_false(warning_low);
    zassert_equal(warning_until, 0);
    observe_level(10);
    expect_warning();
    zassert_true(warning_low);
    zassert_equal(warning_until, now + 3000);
    zassert_equal(due(&displays_work), now + 250);
    zassert_equal(due(&dark_work), original_dark);
    zassert_false(defy_half_leds_battery_active());
    zassert_equal(battery_until, 0);
    now += 3000;
    displays_step(&displays_work.work);
    zassert_false(warning_shows(now));
    zassert_equal(rendered[0].b, LEVEL);
}

ZTEST(half_leds, test_low_duplicates_older_samples_and_old_generation_do_not_progress) {
    observe_level(15);
    battery_updated(&battery_update_work);
    sample.sequence--;
    battery_updated(&battery_update_work);
    zassert_equal(warning_low_count, 1);
    sample_generation = UINT32_MAX;
    sample.sequence += 10;
    battery_updated(&battery_update_work);
    zassert_equal(warning_generation, 1);
    zassert_equal(warning_low_count, 1);
    zassert_false(warning_low);
}

ZTEST(half_leds, test_low_generation_reset_needs_two_new_samples_and_wrap_is_valid) {
    observe_level(15);
    sample_generation++;
    sample.sequence = UINT32_MAX;
    battery_updated(&battery_update_work);
    zassert_equal(warning_low_count, 1);
    sample.sequence = 1;
    battery_updated(&battery_update_work);
    zassert_true(warning_low);
    expect_warning();
}

ZTEST(half_leds, test_low_invalid_stale_and_missing_samples_break_confirmation) {
    observe_level(15);
    sample.age_ms = DEFY_BATTERY_MAX_AGE_MS;
    observe_level(10);
    zassert_equal(warning_low_count, 0);
    sample.age_ms = 0;
    observe_level(10);
    sample.status = DEFY_BATTERY_SAMPLE_IO_ERROR;
    observe_level(10);
    zassert_equal(warning_low_count, 0);
    sample.status = DEFY_BATTERY_SAMPLE_OK;
    observe_level(10);
    sample_error = -ENODATA;
    battery_updated(&battery_update_work);
    zassert_equal(warning_low_count, 0);
    zassert_false(warning_low);
}

ZTEST(half_leds, test_low_warns_once_until_two_high_samples_and_middle_does_not_rearm) {
    observe_level(10);
    observe_level(10);
    now += 3000;
    displays_step(&displays_work.work);
    observe_level(10);
    zassert_false(warning_shows(now));
    observe_level(20);
    observe_level(10);
    observe_level(10);
    zassert_false(warning_shows(now));
    observe_level(25);
    zassert_true(warning_low);
    observe_level(20);
    observe_level(25);
    zassert_true(warning_low);
    observe_level(40);
    zassert_false(warning_low);
    observe_level(15);
    observe_level(15);
    expect_warning();
}

ZTEST(half_leds, test_low_off_and_idle_dark_defer_without_rendering_or_timers) {
    zassert_ok(lighting_set(false));
    unsigned int previous = renders;
    observe_level(10);
    observe_level(10);
    zassert_equal(renders, previous);
    zassert_true(warning_pending);
    zassert_equal(due(&displays_work), -1);
    zassert_false(defy_half_leds_battery_active());
    zassert_ok(lighting_set(true));
    expect_warning();
    now += 3000;
    displays_step(&displays_work.work);
    observe_level(30);
    observe_level(30);
    lighting_override = false;
    go_dark(&dark_work.work);
    previous = renders;
    observe_level(10);
    observe_level(10);
    zassert_equal(renders, previous);
    zassert_true(warning_pending);
    zassert_equal(warning_until, 0);
}

ZTEST(half_leds, test_low_pending_cannot_use_stale_sample_when_lights_return) {
    zassert_ok(lighting_set(false));
    observe_level(10);
    observe_level(10);
    sample.age_ms = DEFY_BATTERY_MAX_AGE_MS;
    zassert_ok(lighting_set(true));
    zassert_equal(warning_until, 0);
    zassert_equal(rendered[0].b, LEVEL);
}

ZTEST(half_leds, test_low_warning_waits_for_battery_and_host_overlays) {
    zassert_ok(defy_half_leds_battery_show());
    observe_level(10);
    observe_level(10);
    zassert_true(warning_pending);
    expect_breathing();
    now += 5000;
    displays_step(&displays_work.work);
    expect_warning();
    struct defy_host_feedback state = host_state(DEFY_HOST_PREFERRED_BLE);
    zassert_ok(defy_half_leds_host_show(&state));
    expect_host(2, false, purple);
    zassert_equal(due(&displays_work), now + 2000);
    now += 3000;
    displays_step(&displays_work.work);
    zassert_false(warning_shows(now));
    zassert_equal(due(&displays_work), -1);
}

ZTEST(half_leds, test_low_protected_stage_keeps_warning_pending) {
    defy_startup_report(DEFY_STARTUP_RADIO_INSTALLING);
    observe_level(10);
    observe_level(10);
    zassert_true(warning_pending);
    zassert_equal(warning_until, 0);
    defy_startup_report(DEFY_STARTUP_RADIO_READY);
    expect_warning();
}

ZTEST(half_leds, test_low_cache_reset_cancels_visible_warning_without_rearming_episode) {
    observe_level(10);
    observe_level(10);
    sample_error = -ENODATA;
    battery_updated(&battery_update_work);
    zassert_equal(warning_until, 0);
    zassert_equal(due(&displays_work), -1);
    zassert_true(warning_low);
    sample_error = 0;
    sample_generation++;
    observe_level(10);
    observe_level(10);
    zassert_equal(warning_until, 0);
}

ZTEST(half_leds, test_low_warning_drains_three_times_then_stops) {
    observe_level(10);
    observe_level(10);
    int64_t start = now;
    zassert_equal(warning_until, start + 3000);
    for (int step = 0; step < 12; step++) {
        expect_cells(3 - step % 4, cell_red);
        zassert_equal(due(&displays_work), start + (step + 1) * 250);
        now = due(&displays_work);
        displays_step(&displays_work.work);
    }
    zassert_equal(now, start + 3000);
    zassert_false(warning_shows(now));
    expect_bar(ARRAY_SIZE(rendered), (struct led_rgb){.b = LEVEL});
    zassert_equal(due(&displays_work), -1);
}

ZTEST(half_leds, test_low_warning_animation_stops_while_lighting_is_off) {
    observe_level(10);
    observe_level(10);
    int64_t start = now;
    now += 250;
    displays_step(&displays_work.work);
    expect_cells(2, cell_red);
    zassert_ok(lighting_set(false));
    expect_bar(0, (struct led_rgb){0});
    zassert_equal(due(&displays_work), -1);
    now = start + 600;
    zassert_ok(lighting_set(true));
    expect_cells(1, cell_red);
    zassert_equal(due(&displays_work), start + 750);
    now = start + 3000;
    displays_step(&displays_work.work);
    expect_bar(ARRAY_SIZE(rendered), (struct led_rgb){.b = LEVEL});
    zassert_equal(due(&displays_work), -1);
}

ZTEST(half_leds, test_low_level_breathes_on_stock_curve_within_bounds) {
    sample.words[DEFY_BATTERY_REPSOC] = 15 * 256u;
    now = 4096;
    zassert_ok(defy_half_leds_battery_show());
    expect_breathing();
    uint8_t lowest = rendered[20].r, highest = rendered[20].r;
    unsigned int steps = 0;
    while (due(&displays_work) < battery_until) {
        zassert_equal(due(&displays_work), now + 40);
        now = due(&displays_work);
        displays_step(&displays_work.work);
        expect_breathing();
        lowest = MIN(lowest, rendered[20].r);
        highest = MAX(highest, rendered[20].r);
        steps++;
    }
    zassert_equal(steps, 124);
    zassert_equal(lowest, 49);
    zassert_equal(highest, 128);
}

/* An unfiltered analog sample, whose level comes from the comparator threshold alone. */
static void analog_sample(uint16_t threshold, uint16_t reference) {
    sample = (struct defy_battery_sample){
        .sequence = 1,
        .status = DEFY_BATTERY_SAMPLE_ANALOG_OK,
        .words = {[DEFY_BATTERY_ANALOG_FORMAT] = DEFY_BATTERY_ANALOG_CHARGER_TAG,
                  [DEFY_BATTERY_ANALOG_THRESHOLD] = threshold,
                  [DEFY_BATTERY_ANALOG_REFERENCE] = reference},
    };
    zassert_ok(defy_battery_analog_mv(threshold, reference,
                                    &sample.words[DEFY_BATTERY_ANALOG_MILLIVOLTS]));
}

static void charger_sample(uint8_t status, uint8_t valid) {
    analog_sample(55, DEFY_BATTERY_ANALOG_REFERENCE_VDD);
    sample.words[0] = DEFY_BATTERY_ANALOG_FILTERED_TAG;
    sample.words[1] |= DEFY_BATTERY_LEVEL_UNAVAILABLE << 8;
    sample.words[4] = (valid & 1 ? 0x94 : 0) | ((valid & 2 ? status : 0) << 8);
    sample.words[7] = (uint16_t)valid << 8;
}

ZTEST(half_leds, test_charging_steps_through_cells_every_160_ms) {
    charger_sample(0x10, 3);
    zassert_ok(defy_half_leds_battery_show());
    expect_cells(1, cell_amber);
    zassert_equal(due(&displays_work), battery_until);
    int64_t start = now;
    sample.sequence++;
    battery_updated(NULL);
    for (int step = 0; step < 9; step++) {
        expect_cells(step % 4, cell_green);
        zassert_equal(due(&displays_work), start + (step + 1) * 160);
        now = due(&displays_work);
        displays_step(&displays_work.work);
    }
    expect_cells(1, cell_green);
    /* A late tick shows the frame for its time and keeps the 160 ms steps. */
    now += 150;
    displays_step(&displays_work.work);
    expect_cells(1, cell_green);
    zassert_equal(due(&displays_work), start + 10 * 160);
    now += 30;
    displays_step(&displays_work.work);
    expect_cells(2, cell_green);
    zassert_equal(due(&displays_work), start + 11 * 160);
    zassert_equal(battery_until, start + 5000);
}

ZTEST(half_leds, test_charge_done_shows_three_steady_green_cells) {
    charger_sample(0x20, 3);
    zassert_ok(defy_half_leds_battery_show());
    expect_cells(1, cell_amber);
    sample.sequence++;
    battery_updated(NULL);
    expect_cells(3, cell_green);
    zassert_equal(due(&displays_work), battery_until);
    zassert_equal(battery_until, now + 5000);
}

ZTEST(half_leds, test_charger_fault_shows_three_red_cells) {
    const uint8_t states[] = {0x31, 0x37};
    for (size_t i = 0; i < ARRAY_SIZE(states); i++) {
        charger_sample(states[i], 3);
        sample.sequence++;
        zassert_ok(defy_half_leds_battery_show());
        expect_cells(1, cell_amber);
        sample.sequence++;
        battery_updated(NULL);
        expect_cells(3, cell_red);
        zassert_equal(due(&displays_work), battery_until);
    }
}

ZTEST(half_leds, test_ready_or_faulted_charge_without_level_stays_unavailable) {
    const uint8_t states[] = {0x00, 0x11, 0x22};
    for (size_t i = 0; i < ARRAY_SIZE(states); i++) {
        charger_sample(states[i], 3);
        sample.sequence++;
        zassert_ok(defy_half_leds_battery_show());
        sample.sequence++;
        battery_updated(NULL);
        expect_cells(1, cell_amber);
        zassert_equal(due(&displays_work), battery_until);
    }
}

ZTEST(half_leds, test_charger_in_boost_remains_unavailable) {
    charger_sample(0x10, 3);
    sample.words[4] |= 0x0800;
    zassert_ok(defy_half_leds_battery_show());
    sample.sequence++;
    battery_updated(NULL);
    expect_cells(1, cell_amber);
}

ZTEST(half_leds, test_charger_state_survives_independent_voltage_error) {
    charger_sample(0x10, 3);
    sample.status = DEFY_BATTERY_SAMPLE_ANALOG_ERROR;
    zassert_ok(defy_half_leds_battery_show());
    now += 480;
    sample.sequence++;
    battery_updated(NULL);
    expect_cells(3, cell_green);
    zassert_equal(due(&displays_work), now + 160);
}

ZTEST(half_leds, test_fresh_reading_updates_visible_display_without_extending_timer) {
    sample.age_ms = 30000;
    zassert_ok(defy_half_leds_battery_show());
    zassert_equal(refresh_requests, 1);
    expect_cells(1, cell_amber);
    int64_t deadline = battery_until;
    now += 500;
    sample.sequence++;
    sample.age_ms = 0;
    battery_updated(NULL);
    expect_cells(2, cell_green);
    zassert_equal(battery_until, deadline);
    sample_error = -EIO;
    sample.sequence++;
    battery_updated(NULL);
    expect_cells(1, cell_amber);
}

ZTEST(half_leds, test_charging_then_pending_then_estimate_clears_old_indicators) {
    charger_sample(0x10, 3);
    zassert_ok(defy_half_leds_battery_show());
    sample.sequence++;
    battery_updated(NULL);
    expect_cells(0, cell_green);
    zassert_equal(due(&displays_work), now + 160);
    for (int i = 0; i < 2; i++) {
        uint32_t sequence = sample.sequence + 1;
        charger_sample(i == 0 ? 0x00 : 0x02, 3);
        sample.sequence = sequence;
        now += 1000;
        battery_updated(NULL);
        expect_cells(1, cell_amber);
        zassert_equal(due(&displays_work), battery_until);
    }
    sample.sequence++;
    sample.words[1] = 55 | 70u << 8;
    battery_updated(NULL);
    expect_cells(2, cell_green);
    sample.sequence++;
    sample.words[4] = 0x2094;
    sample.words[1] = 55 | DEFY_BATTERY_LEVEL_UNAVAILABLE << 8;
    battery_updated(NULL);
    expect_cells(3, cell_green);
}

ZTEST(half_leds, test_filtered_estimate_is_unavailable_after_radio_reset_or_stale_cache) {
    charger_sample(0x00, 3);
    sample.words[1] = 55 | 70u << 8;
    zassert_ok(defy_half_leds_battery_show());
    expect_cells(2, cell_green);
    sample_generation++;
    sample.sequence = 1;
    sample.words[1] = 55 | DEFY_BATTERY_LEVEL_UNAVAILABLE << 8;
    battery_updated(NULL);
    expect_cells(1, cell_amber);
    sample.words[1] = 55 | 70u << 8;
    sample.age_ms = 1500;
    zassert_ok(defy_half_leds_battery_show());
    expect_cells(1, cell_amber);
}

ZTEST(half_leds, test_old_measurement_delivered_after_request_cannot_restore_charging) {
    charger_sample(0x10, 3);
    zassert_ok(defy_half_leds_battery_show());
    now += 100;
    sample.sequence++;
    sample.age_ms = 200;
    battery_updated(NULL);
    expect_cells(1, cell_amber);
    sample.age_ms = 0;
    sample_generation = 0;
    battery_updated(NULL);
    expect_cells(1, cell_amber);
}

ZTEST(half_leds, test_animation_work_stops_when_the_display_ends) {
    sample.words[DEFY_BATTERY_REPSOC] = 10 * 256u;
    zassert_ok(defy_half_leds_battery_show());
    zassert_equal(due(&displays_work), now + 40);
    now = battery_until - 10;
    displays_step(&displays_work.work);
    expect_breathing();
    zassert_equal(due(&displays_work), battery_until);
    now = battery_until;
    displays_step(&displays_work.work);
    expect_bar(ARRAY_SIZE(rendered), (struct led_rgb){.b = LEVEL});
    zassert_equal(due(&displays_work), -1);

    charger_sample(0x10, 3);
    zassert_ok(defy_half_leds_battery_show());
    sample.sequence++;
    battery_updated(NULL);
    zassert_equal(due(&displays_work), now + 160);
    now = battery_until;
    displays_step(&displays_work.work);
    expect_bar(ARRAY_SIZE(rendered), (struct led_rgb){.b = LEVEL});
    zassert_equal(due(&displays_work), -1);
    unsigned int previous = renders;
    battery_updated(NULL);
    zassert_equal(renders, previous);
}

/* Ten profiles: 1, 3, 5, 7, 8 and 10 paired, 3 and 10 connected, and 3 selected. */
static struct defy_host_feedback chooser_state(void) {
    struct defy_host_feedback state = host_state(DEFY_HOST_CHOOSER);
    state.profile_count = 10;
    state.paired_profiles = BIT(0) | BIT(2) | BIT(4) | BIT(6) | BIT(7) | BIT(9);
    state.connected_profiles = BIT(2) | BIT(9);
    return state;
}

/*
 * In chooser_state(), an empty profile on this half, one paired with its host away, and
 * an empty one on the other half.
 */
static const uint8_t empty_here = 1, away_here = 4, elsewhere = 8;

/* Two neighbouring blue underglow LEDs, one step on every 40 ms frame, or none. */
static void expect_chase(bool on) {
    size_t count = ARRAY_SIZE(rendered) - KEYS;
    size_t first = (now / 40) % count;
    for (size_t i = 0; i < count; i++) {
        bool lit = on && (i == first || i == (first + 1) % count);
        zassert_mem_equal(&rendered[KEYS + i], lit ? &blue : &unlit, sizeof(blue),
                          "underglow %u at %lld", (unsigned int)i, now);
    }
}

ZTEST(half_leds, test_chooser_shows_this_halfs_five_profiles_in_every_state) {
    struct defy_host_feedback state = chooser_state();
    zassert_ok(lighting_set(false));
    zassert_ok(defy_half_leds_host_show(&state));
    /* From the outer end: keys 1 to 5, for profiles 1 to 5, with 3 selected. */
    const struct led_rgb top[] = {yellow, white, dim_blue, green, dim_blue, white, unlit};
    const struct led_rgb below[] = {unlit, red, unlit, red, unlit, red, unlit};
    zassert_mem_equal(rendered, top, sizeof(top));
    zassert_mem_equal(rendered + 7, below, sizeof(below));
    for (size_t i = 14; i < ARRAY_SIZE(rendered); i++) {
        zassert_mem_equal(&rendered[i], &unlit, sizeof(unlit), "pixel %u", (unsigned int)i);
    }
    zassert_equal(due(&displays_work), now + 2000);
    now += 2000;
    displays_step(&displays_work.work);
    expect_bar(0, (struct led_rgb){0});
}

ZTEST(half_leds, test_a_chooser_of_five_profiles_shows_them_on_the_left_half) {
    struct defy_host_feedback state = host_state(DEFY_HOST_CHOOSER);
    state.profile = 4;
    state.profile_count = 5;
    state.paired_profiles = BIT(0) | BIT(4);
    state.connected_profiles = BIT(4);
    zassert_ok(lighting_set(false));
    zassert_ok(defy_half_leds_host_show(&state));
    const struct led_rgb top[] = {yellow, white, dim_blue, dim_blue, dim_blue, green, unlit};
    const struct led_rgb below[] = {unlit, red, unlit, unlit, unlit, red, unlit};
    zassert_mem_equal(rendered, top, sizeof(top));
    zassert_mem_equal(rendered + 7, below, sizeof(below));
    expect_chase(false);
}

ZTEST(half_leds, test_chooser_selected_empty_profile_breathes_blue_on_stock_curve) {
    struct defy_host_feedback state = chooser_state();
    state.profile = empty_here;
    zassert_ok(lighting_set(false));
    /*
     * One breath takes 2048 ms, as on stock's pairing screen, drawn in 40 ms frames. The
     * frames at 2040, 2560 and 3040 ms fall at a breath's start, a quarter of the way and its
     * peak: from 49, still brighter than the steady blue of an empty profile that is not
     * selected, to 128.
     */
    const int64_t frames[] = {2040, 2560, 3040};
    const uint8_t levels[] = {49, 88, 128};
    now = frames[0];
    zassert_ok(defy_half_leds_host_show(&state));
    for (size_t i = 0; i < ARRAY_SIZE(frames); i++) {
        struct led_rgb expected = {.b = levels[i]};
        now = frames[i];
        displays_step(&displays_work.work);
        zassert_equal(stock_breath(now, 3) * 128 / 208, levels[i]);
        zassert_mem_equal(&rendered[profile_leds[empty_here]], &expected, sizeof(expected),
                          "at %lld", now);
        expect_chase(true);
        zassert_equal(due(&displays_work), now + 40);
    }
    now = host_until;
    displays_step(&displays_work.work);
    expect_bar(0, (struct led_rgb){0});
    zassert_equal(due(&displays_work), -1);
}

ZTEST(half_leds, test_chooser_selected_paired_profile_breathes_white_slower_and_dimmer) {
    struct defy_host_feedback state = chooser_state();
    state.profile = away_here;
    zassert_ok(lighting_set(false));
    /*
     * One breath takes 4096 ms, at half the brightness of the blue one. The frames at 4080,
     * 5120 and 6080 ms fall at a breath's start, a quarter of the way and its peak: 24 to
     * 64. The Neuron sends the chooser again as it lasts.
     */
    const int64_t frames[] = {4080, 5120, 6080};
    const uint8_t levels[] = {24, 44, 64};
    for (size_t i = 0; i < ARRAY_SIZE(frames); i++) {
        struct led_rgb expected = {.r = levels[i], .g = levels[i], .b = levels[i]};
        now = frames[i];
        state.sequence = i + 1;
        zassert_ok(defy_half_leds_host_show(&state));
        zassert_equal(stock_breath(now, 4) * 64 / 208, levels[i]);
        zassert_mem_equal(&rendered[profile_leds[away_here]], &expected, sizeof(expected),
                          "at %lld", now);
        expect_chase(true);
        zassert_equal(due(&displays_work), now + 40);
    }
}

ZTEST(half_leds, test_chooser_selected_on_the_other_half_only_chases_here) {
    struct defy_host_feedback state = chooser_state();
    state.profile = elsewhere;
    zassert_ok(lighting_set(false));
    zassert_ok(defy_half_leds_host_show(&state));
    struct led_rgb keys[KEYS];
    memcpy(keys, rendered, sizeof(keys));
    for (int frame = 0; frame < 3; frame++) {
        zassert_mem_equal(rendered, keys, sizeof(keys));
        expect_chase(true);
        zassert_equal(due(&displays_work), now + 40);
        now += 40;
        displays_step(&displays_work.work);
    }
    /* Profile 3 is still connected, but no longer selected. */
    zassert_mem_equal(&rendered[3], &dim_green, sizeof(dim_green));
}

ZTEST(half_leds, test_chooser_animates_until_the_selected_profile_connects) {
    struct defy_host_feedback state = chooser_state();
    state.profile = away_here;
    zassert_ok(lighting_set(false));
    zassert_ok(defy_half_leds_host_show(&state));
    size_t led = profile_leds[away_here];
    struct led_rgb first = rendered[led];
    zassert_equal(due(&displays_work), now + 40);
    now += 40;
    displays_step(&displays_work.work);
    zassert_not_equal(rendered[led].b, first.b);
    state.sequence++;
    state.connected_profiles |= BIT(away_here);
    zassert_ok(defy_half_leds_host_show(&state));
    zassert_mem_equal(&rendered[led], &green, sizeof(green));
    expect_chase(false);
    zassert_equal(due(&displays_work), host_until);
    now = host_until;
    displays_step(&displays_work.work);
    expect_bar(0, (struct led_rgb){0});
    zassert_equal(due(&displays_work), -1);
}

ZTEST(half_leds, test_a_cleared_profile_flashes_every_key_blue_for_a_second) {
    struct defy_host_feedback state = chooser_state();
    zassert_ok(lighting_set(false));
    zassert_ok(defy_half_leds_host_show(&state));
    now += 500;
    state.sequence++;
    state.clears++;
    state.paired_profiles &= ~BIT(2);
    state.connected_profiles &= ~BIT(2);
    zassert_ok(defy_half_leds_host_show(&state));
    int64_t end = now + 1000;
    while (now < end) {
        for (size_t i = 0; i < KEYS; i++) {
            zassert_mem_equal(&rendered[i], &blue, sizeof(blue), "key %u", (unsigned int)i);
        }
        expect_chase(true);
        zassert_true(due(&displays_work) <= end);
        now = due(&displays_work);
        displays_step(&displays_work.work);
    }
    zassert_equal(now, end);
    zassert_mem_equal(&rendered[0], &yellow, sizeof(yellow));
    zassert_mem_equal(&rendered[profile_leds[empty_here]], &dim_blue, sizeof(dim_blue));
    /* The same frame again, or the next one with the same count, does not flash. */
    zassert_ok(defy_half_leds_host_show(&state));
    state.sequence++;
    zassert_ok(defy_half_leds_host_show(&state));
    zassert_mem_equal(&rendered[0], &yellow, sizeof(yellow));
}

ZTEST(half_leds, test_clear_count_flashes_only_when_it_changes_in_a_showing_chooser) {
    zassert_ok(lighting_set(false));
    struct defy_host_feedback state = chooser_state();
    state.clears = 5;
    zassert_ok(defy_half_leds_host_show(&state));
    zassert_mem_equal(&rendered[0], &yellow, sizeof(yellow));
    struct defy_host_feedback overlay = host_state(DEFY_HOST_PREFERRED_BLE);
    overlay.sequence = 2;
    overlay.clears = 6;
    zassert_ok(defy_half_leds_host_show(&overlay));
    zassert_mem_equal(&rendered[0], &unlit, sizeof(unlit));
    now += 2000;
    displays_step(&displays_work.work);
    state.sequence = 3;
    state.clears = 7;
    zassert_ok(defy_half_leds_host_show(&state));
    zassert_mem_equal(&rendered[0], &yellow, sizeof(yellow));
    state.boot++;
    accepted_lighting.boot = state.boot;
    state.sequence = 1;
    state.clears = 8;
    zassert_ok(defy_half_leds_host_show(&state));
    zassert_mem_equal(&rendered[0], &yellow, sizeof(yellow));
    zassert_equal(host_cleared_until, 0);
}

ZTEST(half_leds, test_chooser_overrides_battery_and_takes_a_new_request) {
    zassert_ok(defy_half_leds_battery_show());
    struct defy_host_feedback state = chooser_state();
    zassert_ok(defy_half_leds_host_show(&state));
    size_t led = profile_leds[away_here];
    zassert_mem_equal(&rendered[0], &yellow, sizeof(yellow));
    zassert_mem_equal(&rendered[led], &white, sizeof(white));
    state.paired_profiles ^= BIT(away_here);
    state.sequence++;
    zassert_ok(defy_half_leds_host_show(&state));
    zassert_mem_equal(&rendered[led], &dim_blue, sizeof(dim_blue));
    zassert_mem_equal(&rendered[defy_half_leds_rows[0] + led], &unlit, sizeof(unlit));
}

ZTEST(half_leds, test_chooser_over_breathing_display_replaces_its_ticks) {
    sample.words[DEFY_BATTERY_REPSOC] = 10 * 256u;
    zassert_ok(defy_half_leds_battery_show());
    zassert_equal(due(&displays_work), now + 40);
    struct defy_host_feedback state = chooser_state();
    zassert_ok(defy_half_leds_host_show(&state));
    zassert_equal(rendered[20].r, 0);
    zassert_equal(due(&displays_work), now + 2000);
    now += 2000;
    displays_step(&displays_work.work);
    expect_breathing();
    zassert_equal(due(&displays_work), now + 40);
}

/* The same state with the dock flag differs only in pixel 13, in cyan. */
static void expect_dock_marker(struct defy_host_feedback state) {
    zassert_ok(defy_half_leds_host_show(&state));
    struct led_rgb plain[ARRAY_SIZE(rendered)];
    memcpy(plain, rendered, sizeof(plain));
    zassert_mem_equal(&plain[13], &unlit, sizeof(unlit));
    state.sequence++;
    state.flags |= DEFY_HOST_DOCKED;
    zassert_ok(defy_half_leds_host_show(&state));
    for (size_t i = 0; i < ARRAY_SIZE(rendered); i++) {
        zassert_mem_equal(&rendered[i], i == 13 ? &cyan : &plain[i], sizeof(cyan), "pixel %u",
                          (unsigned int)i);
    }
}

ZTEST(half_leds, test_host_overlay_marks_the_dock_on_pixel_13) {
    expect_dock_marker(host_state(DEFY_HOST_PREFERRED_BLE));
}

ZTEST(half_leds, test_chooser_marks_the_dock_on_pixel_13) {
    zassert_ok(lighting_set(false));
    expect_dock_marker(chooser_state());
}

ZTEST_SUITE(half_leds, NULL, NULL, before, NULL, NULL);
