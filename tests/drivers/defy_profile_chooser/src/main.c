/* SPDX-License-Identifier: MIT */

#include <string.h>
#include <zephyr/ztest.h>
#include <dygma/defy_feedback.h>
#include <dygma/defy_profile_chooser.h>
#include <dygma/defy_lighting.h>
#include <dygma/defy_neuron_dock.h>
#include <dygma/defy_wired_protocol.h>
#include <zephyr/random/random.h>
#include <zephyr/settings/settings.h>
#include <zmk/ble.h>
#include <zmk/endpoints.h>
#include <zmk/usb.h>

#include "fake_uptime.h"

static int active_profile;
static uint16_t paired, connected;
static unsigned int clear_calls, selected_calls, feedback_calls, ble_feedback_calls;
static int cleared_profile, select_error;
static enum zmk_transport preferred, selected;
static bool usb_ready;
static enum usb_dc_status_code usb_status;
static struct defy_host_feedback displayed;
static enum defy_neuron_place place;
static int64_t rescheduled_ms;
static unsigned int keymap_events;
static struct zmk_position_state_changed keymap_event;

static uint32_t test_random(void) { return 0x12345678; }
static int test_reschedule(struct k_work_delayable *work, k_timeout_t delay);
enum defy_neuron_place defy_neuron_place(void) { return place; }
bool zmk_ble_profile_is_open(uint8_t index) { return !(paired & BIT(index)); }
bool zmk_ble_profile_is_connected(uint8_t index) { return connected & BIT(index); }
bool zmk_ble_active_profile_is_connected(void) {
    return zmk_ble_profile_is_connected(active_profile);
}
int zmk_ble_active_profile_index(void) { return active_profile; }
int __real_zmk_ble_prof_select(uint8_t index) {
    selected_calls++;
    if (select_error) {
        return select_error;
    }
    active_profile = index;
    return 0;
}
int __real_zmk_ble_prof_next(void) {
    return __real_zmk_ble_prof_select((active_profile + 1) % ZMK_BLE_PROFILE_COUNT);
}
void zmk_ble_clear_bonds(void) {
    clear_calls++;
    cleared_profile = active_profile;
    paired &= ~BIT(active_profile);
    connected &= ~BIT(active_profile);
}
int __real_zmk_endpoint_set_preferred_transport(enum zmk_transport transport) {
    preferred = transport;
    return 0;
}
int __real_zmk_ble_prof_prev(void) {
    return __real_zmk_ble_prof_select((active_profile + ZMK_BLE_PROFILE_COUNT - 1) %
                                      ZMK_BLE_PROFILE_COUNT);
}
int __real_zmk_endpoint_toggle_preferred_transport(void) {
    return __real_zmk_endpoint_set_preferred_transport(
        preferred == ZMK_TRANSPORT_USB ? ZMK_TRANSPORT_BLE : ZMK_TRANSPORT_USB);
}
enum zmk_transport zmk_endpoint_get_preferred_transport(void) { return preferred; }
struct zmk_endpoint_instance zmk_endpoint_get_selected(void) {
    return (struct zmk_endpoint_instance){.transport = selected};
}
bool zmk_usb_is_hid_ready(void) { return usb_ready; }
enum usb_dc_status_code zmk_usb_get_status(void) { return usb_status; }
void defy_wired_host_feedback_show(const struct defy_host_feedback *state) {
    displayed = *state;
    feedback_calls++;
}
/* Both links get the same display, the cable's first. */
void defy_control_ble_host_feedback_show(const struct defy_host_feedback *state) {
    zassert_mem_equal(&displayed, state, sizeof(*state));
    ble_feedback_calls++;
}
void defy_wired_lighting_changed(void) {}
void defy_control_ble_lighting_changed(void) {}
int settings_save_one(const char *name, const void *value, size_t length) { return 0; }
/* Never called: these tests load no settings. */
int settings_name_steq(const char *name, const char *key, const char **next) { return 0; }

#define k_work_reschedule test_reschedule
#define sys_rand32_get test_random
#include "../../../../lighting/defy_lighting_central.c"
#include "../../../../lighting/defy_profile_chooser.c"
#include "../../../../lighting/defy_feedback_central.c"
#undef sys_rand32_get
#undef k_work_reschedule
#undef k_uptime_get

static int test_reschedule(struct k_work_delayable *work, k_timeout_t delay) {
    /* Presses also restart the lighting's idle timer, which these tests leave alone. */
    if (work == &chooser_work) {
        rescheduled_ms = k_ticks_to_ms_floor64(delay.ticks);
    }
    return 1;
}

/* Stands in for the keymap and the other ZMK listeners after the lighting's. See keymap.c. */
int keymap_position(const zmk_event_t *event) {
    keymap_event = *as_zmk_position_state_changed(event);
    keymap_events++;
    return ZMK_EV_EVENT_BUBBLE;
}

/*
 * Positions of the number keys 1 to 5 and 6 to 0, and of the letters below them, Q to T
 * and Y to P, with the profiles that the right half's keys pick.
 */
static const uint32_t left_numbers[] = {1, 2, 3, 4, 5};
static const uint32_t right_numbers[] = {8, 9, 10, 11, 12};
static const uint32_t left_letters[] = {15, 16, 17, 18, 19};
static const uint32_t right_letters[] = {22, 23, 24, 25, 26};
static const uint8_t right_profiles[] = {5, 6, 7, 8, 9};

/* Raises a key event and returns whether the chooser took it, so the keymap never saw it. */
static bool key(uint8_t source, uint32_t position, bool pressed) {
    unsigned int seen = keymap_events;
    zassert_ok(raise_zmk_position_state_changed((struct zmk_position_state_changed){
        .source = source, .position = position, .state = pressed, .timestamp = now,
    }));
    if (keymap_events == seen) {
        return true;
    }
    zassert_equal(keymap_events, seen + 1);
    zassert_equal(keymap_event.source, source);
    zassert_equal(keymap_event.position, position);
    zassert_equal(keymap_event.state, pressed);
    return false;
}

static void tick(void) { chooser_tick(&chooser_work.work); }

/* A 'DL' frame's header, then the rest as the host display. */
static int decode_wired(const uint8_t *frame, size_t length,
                        struct defy_wired_host_feedback *command) {
    struct defy_wired_control control;
    int err = defy_wired_control_check(frame, length, &control);
    return err ? err : defy_wired_host_feedback_decode(frame, &control, command);
}

static void before(void *unused) {
    ARG_UNUSED(unused);
    now = 100;
    active_profile = 0;
    paired = BIT(0) | BIT(2);
    connected = BIT(0);
    clear_calls = selected_calls = feedback_calls = ble_feedback_calls = 0;
    cleared_profile = -1;
    select_error = 0;
    preferred = selected = ZMK_TRANSPORT_USB;
    usb_ready = true;
    usb_status = USB_DC_CONFIGURED;
    place = DEFY_NEURON_COMPUTER;
    rescheduled_ms = -1;
    chooser_active = false;
    chooser_deadline = preview_deadline = 0;
    chooser_generation = 0;
    boot = sequence = 0;
    memset(forwarded, 0, sizeof(forwarded));
    memset(captured, 0, sizeof(captured));
    memset(clear_at, 0, sizeof(clear_at));
    clears = 0;
    keymap_events = 0;
    settings_ready = true;
    desired = (struct defy_lighting_state){.boot = 123, .revision = 1, .enabled = true};
    preferred_enabled = true;
    idle = false;
    last_activity_ms = 0;
}

ZTEST(chooser, test_next_profile_switches_without_opening_the_chooser) {
    zassert_ok(__wrap_zmk_ble_prof_next());
    zassert_false(chooser_active);
    zassert_equal(active_profile, 1);
    zassert_equal(selected_calls, 1);
    zassert_equal(feedback_calls, 1);
    zassert_equal(displayed.profile, 1);
    zassert_false(displayed.flags & DEFY_HOST_CHOOSER);
    zassert_true(defy_host_feedback_valid(&displayed));
}

ZTEST(chooser, test_open_chooser_shows_every_profile) {
    defy_profile_chooser_toggle();
    tick();
    zassert_true(chooser_active);
    zassert_equal(active_profile, 0);
    zassert_equal(selected_calls, 0);
    zassert_equal(clear_calls, 0);
    zassert_equal(displayed.profile_count, ZMK_BLE_PROFILE_COUNT);
    zassert_equal(displayed.paired_profiles, paired);
    zassert_equal(displayed.connected_profiles, connected);
    zassert_true(displayed.flags & DEFY_HOST_CHOOSER);
    zassert_true(defy_host_feedback_valid(&displayed));
}

ZTEST(chooser, test_keys_the_chooser_takes_still_wake_the_lights) {
    defy_profile_chooser_toggle();
    idle = true;
    desired.enabled = false;
    now = 5000;
    zassert_true(key(0, 3, true));
    zassert_true(desired.enabled);
    zassert_equal(last_activity_ms, 5000);
    now = 6000;
    zassert_true(key(0, 3, false));
    zassert_equal(last_activity_ms, 5000);
    zassert_false(chooser_active);
    zassert_false(key(0, 29, true));
    zassert_equal(last_activity_ms, 6000);
    zassert_false(key(0, 29, false));
    zassert_equal(keymap_events, 2);
}

ZTEST(chooser, test_selection_is_on_release_and_never_types_the_number) {
    zassert_false(key(0, 63, true));
    zassert_false(key(0, 46, true));
    defy_profile_chooser_toggle();
    zassert_false(key(0, 46, false));
    zassert_false(key(0, 63, false));
    zassert_true(key(0, 3, true));
    zassert_equal(active_profile, 0);
    zassert_true(key(0, 3, false));
    zassert_equal(active_profile, 2);
    zassert_equal(preferred, ZMK_TRANSPORT_BLE);
    zassert_false(chooser_active);
    zassert_false(key(0, 29, true));
    zassert_false(key(0, 29, false));
}

ZTEST(chooser, test_number_keys_select_every_profile_as_the_keycaps_read) {
    for (uint8_t i = 0; i < 5; i++) {
        defy_profile_chooser_toggle();
        zassert_true(key(0, left_numbers[i], true));
        zassert_true(key(0, left_numbers[i], false));
        zassert_equal(active_profile, i);
        zassert_equal(preferred, ZMK_TRANSPORT_BLE);
        zassert_false(chooser_active);
        preferred = ZMK_TRANSPORT_USB;
        defy_profile_chooser_toggle();
        zassert_true(key(1, right_numbers[i], true));
        zassert_true(key(1, right_numbers[i], false));
        zassert_equal(active_profile, right_profiles[i], "key %u", right_numbers[i]);
        zassert_equal(preferred, ZMK_TRANSPORT_BLE);
        zassert_false(chooser_active);
    }
    zassert_equal(selected_calls, ZMK_BLE_PROFILE_COUNT);
}

ZTEST(chooser, test_letter_keys_clear_every_paired_profile_below_its_number) {
    for (int side = 0; side < 2; side++) {
        for (uint8_t i = 0; i < 5; i++) {
            uint8_t profile = side ? right_profiles[i] : i;
            uint32_t letter = side ? right_letters[i] : left_letters[i];
            paired = BIT_MASK(ZMK_BLE_PROFILE_COUNT);
            connected = BIT(profile);
            defy_profile_chooser_toggle();
            zassert_true(key(side, letter, true));
            now += CHOOSER_CLEAR_MS;
            tick();
            zassert_equal(cleared_profile, profile, "key %u", letter);
            zassert_equal(active_profile, profile);
            zassert_equal(paired, BIT_MASK(ZMK_BLE_PROFILE_COUNT) & ~BIT(profile));
            zassert_true(chooser_active);
            zassert_equal(displayed.paired_profiles, paired);
            zassert_equal(displayed.connected_profiles, 0);
            zassert_true(key(side, letter, false));
            defy_profile_chooser_toggle();
        }
    }
    zassert_equal(clear_calls, ZMK_BLE_PROFILE_COUNT);
}

ZTEST(chooser, test_every_frame_counts_clears_for_the_halves_flash) {
    defy_profile_chooser_toggle();
    tick();
    zassert_equal(displayed.clears, 0);
    key(0, 17, true);
    now += CHOOSER_CLEAR_MS;
    tick();
    zassert_equal(clear_calls, 1);
    zassert_equal(displayed.clears, 1);
    zassert_true(displayed.flags & DEFY_HOST_CHOOSER);
    now += CHOOSER_REFRESH_MS;
    tick();
    zassert_equal(displayed.clears, 1);
    key(0, 17, false);
    /* Profile 3 is empty now: holding E again clears nothing. */
    key(0, 17, true);
    now += CHOOSER_CLEAR_MS;
    tick();
    zassert_equal(clear_calls, 1);
    zassert_equal(displayed.clears, 1);
    key(0, 17, false);
    key(0, 28, true);
    key(0, 28, false);
    now += DEFY_HOST_FEEDBACK_DURATION_MS;
    tick();
    zassert_ok(__wrap_zmk_endpoint_set_preferred_transport(ZMK_TRANSPORT_USB));
    zassert_false(displayed.flags & DEFY_HOST_CHOOSER);
    zassert_equal(displayed.clears, 1);
    zassert_true(defy_host_feedback_valid(&displayed));
}

ZTEST(chooser, test_clear_requires_full_hold_and_targets_its_own_profile) {
    defy_profile_chooser_toggle();
    zassert_true(key(0, 17, true));
    now += 2999;
    tick();
    zassert_equal(clear_calls, 0);
    now++;
    tick();
    zassert_equal(clear_calls, 1);
    zassert_equal(cleared_profile, 2);
    zassert_equal(paired, BIT(0));
    zassert_true(chooser_active);
    now += 4000;
    tick();
    zassert_equal(clear_calls, 1);
    zassert_true(key(0, 17, false));
}

ZTEST(chooser, test_right_clear_and_release_before_deadline) {
    paired |= BIT(right_profiles[2]);
    defy_profile_chooser_toggle();
    key(1, 24, true);
    now += 2999;
    key(1, 24, false);
    now += 1000;
    tick();
    zassert_equal(clear_calls, 0);
    key(1, 24, true);
    now += 3000;
    tick();
    zassert_equal(cleared_profile, right_profiles[2]);
}

ZTEST(chooser, test_cancel_prevents_clear_and_swallows_late_releases) {
    defy_profile_chooser_toggle();
    key(0, 15, true);
    key(0, 28, true);
    key(0, 28, false);
    zassert_false(chooser_active);
    now += 4000;
    tick();
    zassert_equal(clear_calls, 0);
    zassert_true(key(0, 15, false));
    zassert_false(key(0, 15, true));
}

ZTEST(chooser, test_reopening_does_not_reuse_old_captured_presses) {
    defy_profile_chooser_toggle();
    key(0, 4, true);
    defy_profile_chooser_toggle();
    defy_profile_chooser_toggle();
    key(0, 4, false);
    zassert_equal(selected_calls, 0);
    zassert_true(chooser_active);
}

ZTEST(chooser, test_preexisting_modifier_release_reaches_keymap) {
    zassert_false(key(0, 42, true));
    defy_profile_chooser_toggle();
    zassert_true(key(1, 37, true));
    zassert_false(key(0, 42, false));
    defy_profile_chooser_toggle();
    zassert_true(key(1, 37, false));
}

ZTEST(chooser, test_usb_exit_and_idle_timeout) {
    preferred = ZMK_TRANSPORT_BLE;
    defy_profile_chooser_toggle();
    key(1, 13, true);
    key(1, 13, false);
    zassert_equal(preferred, ZMK_TRANSPORT_USB);
    zassert_false(chooser_active);
    defy_profile_chooser_toggle();
    now += 30000;
    tick();
    zassert_false(chooser_active);
    zassert_false(key(0, 29, true));
}

ZTEST(chooser, test_failed_selection_never_clears_another_profile) {
    defy_profile_chooser_toggle();
    select_error = -EIO;
    key(0, 17, true);
    now += 3000;
    tick();
    zassert_equal(clear_calls, 0);
    zassert_equal(active_profile, 0);
    key(0, 17, false);
    key(0, 5, true);
    key(0, 5, false);
    zassert_true(chooser_active);
    zassert_equal(preferred, ZMK_TRANSPORT_USB);
}

ZTEST(chooser, test_empty_profiles_and_invalid_events_cannot_erase) {
    defy_profile_chooser_toggle();
    zassert_false(key(2, 15, true));
    zassert_false(key(0, 70, true));
    key(0, 16, true);
    now += 4000;
    tick();
    zassert_equal(clear_calls, 0);
}

ZTEST(chooser, test_dock_marker_follows_the_place_on_each_refresh) {
    defy_profile_chooser_toggle();
    tick();
    zassert_equal(rescheduled_ms, CHOOSER_REFRESH_MS);
    zassert_false(displayed.flags & DEFY_HOST_DOCKED);
    unsigned int calls = feedback_calls;
    place = DEFY_NEURON_DOCKED;
    now += CHOOSER_REFRESH_MS;
    tick();
    zassert_equal(feedback_calls, calls + 1);
    zassert_equal(displayed.flags & (DEFY_HOST_CHOOSER | DEFY_HOST_DOCKED),
                  DEFY_HOST_CHOOSER | DEFY_HOST_DOCKED);
    zassert_true(defy_host_feedback_valid(&displayed));
    place = DEFY_NEURON_CHARGER;
    now += CHOOSER_REFRESH_MS;
    tick();
    zassert_false(displayed.flags & DEFY_HOST_DOCKED);
    zassert_equal(rescheduled_ms, CHOOSER_REFRESH_MS);
}

ZTEST_SUITE(chooser, NULL, NULL, before, NULL, NULL);

/* The host display without the chooser, from a selection with no profile connected. */
static void feedback_before(void *unused) {
    before(unused);
    connected = 0;
}

ZTEST(feedback, test_selection_reports_preference_and_actual_fallback) {
    zassert_ok(__wrap_zmk_endpoint_set_preferred_transport(ZMK_TRANSPORT_BLE));
    zassert_equal(feedback_calls, 1);
    zassert_equal(ble_feedback_calls, 1);
    zassert_equal(displayed.flags, DEFY_HOST_PREFERRED_BLE | DEFY_HOST_SELECTED_USB |
                                   DEFY_HOST_USB_READY);
    zassert_equal(displayed.remaining_ms, 2000);
    zassert_equal(displayed.boot, 123);
    zassert_equal(displayed.sequence, 1);
}

ZTEST(feedback, test_profile_cycle_and_failed_selection) {
    zassert_ok(__wrap_zmk_ble_prof_next());
    zassert_equal(displayed.profile, 1);
    zassert_ok(__wrap_zmk_ble_prof_prev());
    zassert_equal(displayed.profile, 0);
    zassert_ok(__wrap_zmk_ble_prof_select(4));
    zassert_equal(displayed.profile, 4);
    select_error = -EINVAL;
    zassert_equal(__wrap_zmk_ble_prof_select(6), -EINVAL);
    zassert_equal(feedback_calls, 3);
    zassert_equal(displayed.sequence, 3);
}

ZTEST(feedback, test_bluetooth_only_and_no_output_are_distinct) {
    preferred = selected = ZMK_TRANSPORT_BLE;
    connected = BIT_MASK(ZMK_BLE_PROFILE_COUNT);
    usb_ready = false;
    zassert_ok(__wrap_zmk_ble_prof_next());
    zassert_equal(displayed.flags, DEFY_HOST_PREFERRED_BLE | DEFY_HOST_SELECTED_BLE |
                                   DEFY_HOST_PROFILE_CONNECTED);
    selected = ZMK_TRANSPORT_NONE;
    zassert_ok(__wrap_zmk_endpoint_set_preferred_transport(ZMK_TRANSPORT_NONE));
    zassert_equal(displayed.flags, DEFY_HOST_PREFERRED_NONE | DEFY_HOST_PROFILE_CONNECTED);
    sequence = UINT32_MAX;
    zassert_ok(__wrap_zmk_endpoint_toggle_preferred_transport());
    zassert_equal(displayed.sequence, 1);
}

ZTEST(feedback, test_suspended_usb_is_not_reported_as_usable) {
    usb_status = USB_DC_SUSPEND;
    zassert_ok(__wrap_zmk_endpoint_set_preferred_transport(ZMK_TRANSPORT_USB));
    zassert_equal(displayed.flags, 0);
}

ZTEST(feedback, test_settings_not_ready_does_not_invent_boot_identity) {
    settings_ready = false;
    zassert_ok(__wrap_zmk_ble_prof_next());
    zassert_equal(active_profile, 1);
    zassert_equal(feedback_calls, 0);
    zassert_equal(ble_feedback_calls, 0);
}

ZTEST(feedback, test_control_codec_rejects_invalid_state) {
    zassert_ok(__wrap_zmk_ble_prof_next());
    uint8_t data[DEFY_HOST_FEEDBACK_SIZE];
    struct defy_host_feedback decoded = {0};
    zassert_ok(defy_host_feedback_encode(&displayed, data));
    zassert_ok(defy_host_feedback_decode(data, sizeof(data), &decoded));
    zassert_mem_equal(&decoded, &displayed, sizeof(displayed));
    zassert_equal(defy_host_feedback_decode(data, sizeof(data) - 1, &decoded), -EINVAL);
    for (size_t i = 0; i < 2; i++) {
        data[i] ^= 0x80;
        zassert_equal(defy_host_feedback_decode(data, sizeof(data), &decoded), -EPROTO);
        data[i] ^= 0x80;
    }
    data[13] = DEFY_HOST_SELECTED_BLE;
    zassert_equal(defy_host_feedback_decode(data, sizeof(data), &decoded), -EPROTO);
    data[13] = DEFY_HOST_SELECTED_USB;
    zassert_equal(defy_host_feedback_decode(data, sizeof(data), &decoded), -EPROTO);
    data[13] = DEFY_HOST_DOCKED;
    zassert_ok(defy_host_feedback_decode(data, sizeof(data), &decoded));
    zassert_equal(decoded.flags, DEFY_HOST_DOCKED);
    displayed.remaining_ms = 0;
    zassert_equal(defy_host_feedback_encode(&displayed, data), -EINVAL);
    displayed.remaining_ms = 2001;
    zassert_equal(defy_host_feedback_encode(&displayed, data), -EINVAL);
}

ZTEST(feedback, test_wired_codec_preserves_scope_and_checks_reserved_bytes) {
    zassert_ok(__wrap_zmk_ble_prof_next());
    struct defy_wired_host_feedback command = {.side = 1, .session = 456, .state = displayed};
    struct defy_wired_host_feedback decoded;
    uint8_t frame[DEFY_WIRED_FRAME_SIZE];
    zassert_ok(defy_wired_host_feedback_encode(&command, frame));
    zassert_ok(decode_wired(frame, sizeof(frame), &decoded));
    zassert_equal(decoded.side, 1);
    zassert_equal(decoded.session, 456);
    zassert_mem_equal(&decoded.state, &displayed, sizeof(displayed));
    frame[12] ^= 1;
    zassert_equal(decode_wired(frame, sizeof(frame), &decoded), -EBADMSG);
    frame[12] ^= 1;
    for (size_t i = 5; i < 8; i++) {
        frame[i] = 1;
        defy_wired_frame_seal(frame);
        zassert_equal(decode_wired(frame, sizeof(frame), &decoded), -EPROTO);
        frame[i] = 0;
    }
    const uint8_t versions[] = {0, DEFY_HOST_FEEDBACK_VERSION + 1};
    for (size_t i = 0; i < ARRAY_SIZE(versions); i++) {
        frame[2] = versions[i];
        defy_wired_frame_seal(frame);
        zassert_equal(decode_wired(frame, sizeof(frame), &decoded), -EPROTO);
    }
    command.session = 0;
    zassert_equal(defy_wired_host_feedback_encode(&command, frame), -EINVAL);
}

ZTEST(feedback, test_version_3_carries_ten_profiles_and_the_clear_count) {
    zassert_ok(__wrap_zmk_ble_prof_select(9));
    displayed.flags |= DEFY_HOST_CHOOSER;
    displayed.profile_count = 10;
    displayed.clears = 3;
    displayed.paired_profiles = BIT(0) | BIT(2) | BIT(5) | BIT(9);
    displayed.connected_profiles = BIT(5) | BIT(9);
    uint8_t bytes[DEFY_HOST_FEEDBACK_SIZE];
    struct defy_host_feedback decoded;
    zassert_ok(defy_host_feedback_encode(&displayed, bytes));
    const uint8_t tail[] = {9, displayed.flags, 10, 3, 0x25, 0x02, 0x20, 0x02};
    zassert_equal(bytes[0], 3);
    zassert_equal(bytes[1], DEFY_CONTROL_HOST_FEEDBACK);
    zassert_mem_equal(bytes + 12, tail, sizeof(tail));
    zassert_ok(defy_host_feedback_decode(bytes, sizeof(bytes), &decoded));
    zassert_mem_equal(&displayed, &decoded, sizeof(displayed));

    /* The cable frame's header holds the version and opcode, and bytes 12 to 29 the rest. */
    struct defy_wired_host_feedback command = {.side = 0, .session = 456, .state = displayed};
    struct defy_wired_host_feedback result;
    uint8_t frame[DEFY_WIRED_FRAME_SIZE];
    zassert_ok(defy_wired_host_feedback_encode(&command, frame));
    zassert_equal(frame[2], 3);
    zassert_equal(frame[3], DEFY_CONTROL_HOST_FEEDBACK);
    zassert_mem_equal(frame + 12, bytes + 2, sizeof(bytes) - 2);
    zassert_ok(decode_wired(frame, sizeof(frame), &result));
    zassert_mem_equal(&displayed, &result.state, sizeof(displayed));

    struct {
        size_t offset;
        uint8_t value;
    } invalid[] = {
        {14, 9},     /* the selected profile beyond the count */
        {14, 11},    /* more than ten profiles */
        {17, 0x06},  /* profile 11 paired */
        {19, 0x03},  /* profile 9 connected but not paired */
    };
    for (size_t i = 0; i < ARRAY_SIZE(invalid); i++) {
        uint8_t mutated[DEFY_HOST_FEEDBACK_SIZE];
        memcpy(mutated, bytes, sizeof(mutated));
        mutated[invalid[i].offset] = invalid[i].value;
        zassert_equal(defy_host_feedback_decode(mutated, sizeof(mutated), &decoded), -EPROTO,
                      "case %u", (unsigned int)i);
    }
    displayed.profile = 10;
    zassert_equal(defy_host_feedback_encode(&displayed, bytes), -EINVAL);
}

ZTEST(feedback, test_clear_count_needs_no_chooser) {
    zassert_ok(__wrap_zmk_ble_prof_next());
    displayed.clears = 255;
    uint8_t bytes[DEFY_HOST_FEEDBACK_SIZE];
    struct defy_host_feedback decoded;
    zassert_ok(defy_host_feedback_encode(&displayed, bytes));
    zassert_ok(defy_host_feedback_decode(bytes, sizeof(bytes), &decoded));
    zassert_equal(decoded.clears, 255);
    bytes[16] = 1;
    zassert_equal(defy_host_feedback_decode(bytes, sizeof(bytes), &decoded), -EPROTO);
}

ZTEST(feedback, test_only_the_dock_sets_the_docked_flag) {
    static const enum defy_neuron_place places[] = {
        DEFY_NEURON_ALONE, DEFY_NEURON_CHARGER, DEFY_NEURON_DOCKED, DEFY_NEURON_COMPUTER,
    };
    usb_ready = false;
    for (size_t i = 0; i < ARRAY_SIZE(places); i++) {
        place = places[i];
        zassert_ok(__wrap_zmk_ble_prof_next());
        zassert_equal(displayed.flags & DEFY_HOST_DOCKED,
                      place == DEFY_NEURON_DOCKED ? DEFY_HOST_DOCKED : 0);
        zassert_true(defy_host_feedback_valid(&displayed));
    }
    zassert_equal(ble_feedback_calls, ARRAY_SIZE(places));
}

ZTEST_SUITE(feedback, NULL, NULL, feedback_before, NULL, NULL);
