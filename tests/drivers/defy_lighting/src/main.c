/* SPDX-License-Identifier: MIT */

#include <dt-bindings/dygma/defy_lighting.h>
#include <dygma/defy_lighting.h>
#include <errno.h>
#include <string.h>
#include <zephyr/kernel.h>
#include <zephyr/random/random.h>
#include <zephyr/settings/settings.h>
#include <zephyr/ztest.h>
#include <zmk/events/layer_state_changed.h>
#include <zmk/events/position_state_changed.h>
#include <zmk/keymap.h>

#include "fake_uptime.h"

static unsigned int publishes;
static unsigned int schedules;
static unsigned int saves;
static uint8_t saved_value;
/* Saves of "defy/look", and the last one's bytes. */
static unsigned int look_saves;
static uint8_t saved_look_bytes[6];
static int save_error;
static bool toggle_during_save;
static unsigned int renders;
static int render_error;
static bool rendered_enabled;
/* Whether the last render had a state, and its colours. */
static bool rendered_known;
static struct defy_lighting_view rendered_view;
static bool disconnect_during_render;
static unsigned int idle_schedules;
static int64_t idle_delay;
static unsigned int consumed_events;
static int consumer_result;
static bool consumer_saw_enabled;
static bool toggle_in_consumer;
static struct zmk_position_state_changed_event captured_event;

void defy_wired_lighting_changed(void) { publishes++; }
void defy_control_ble_lighting_changed(void) { publishes++; }
int settings_save_one(const char *name, const void *value, size_t length) {
    if (strcmp(name, "defy/look") == 0) {
        zassert_equal(length, sizeof(saved_look_bytes));
        memcpy(saved_look_bytes, value, length);
        look_saves++;
        return save_error;
    }
    zassert_equal(strcmp(name, "defy/lighting"), 0);
    zassert_equal(length, 1);
    saved_value = *(const uint8_t *)value;
    saves++;
    if (toggle_during_save) {
        toggle_during_save = false;
        zassert_ok(defy_lighting_command(DEFY_LIGHTING_TOGGLE));
    }
    return save_error;
}
/* Zephyr's, as the settings subsystem is not built here. */
int settings_name_steq(const char *name, const char *key, const char **next) {
    if (next) {
        *next = NULL;
    }
    if (!name || !key) {
        return 0;
    }
    while (*key && *key == *name && *name && *name != '=') {
        key++;
        name++;
    }
    if (*key) {
        return 0;
    }
    if (*name == '/') {
        if (next) {
            *next = name + 1;
        }
        return 1;
    }
    return *name == '=' || !*name;
}
int defy_half_leds_lighting_set(const struct defy_lighting_view *view) {
    renders++;
    rendered_known = view != NULL;
    rendered_enabled = view && view->enabled;
    rendered_view = view ? *view : (struct defy_lighting_view){0};
    if (disconnect_during_render) {
        defy_lighting_peripheral_ble_epoch(0);
    }
    return render_error;
}
static int test_schedule(struct k_work_delayable *work, k_timeout_t timeout);
static uint32_t test_random(void) { return 0x12345678; }
/* apply() asserts that it runs on the system work queue. */
static k_tid_t test_current(void) { return &k_sys_work_q.thread; }

#define k_work_reschedule test_schedule
#define sys_rand32_get test_random
#define k_current_get test_current
#include "../../../../lighting/defy_lighting_central.c"
#include "../../../../lighting/defy_lighting_peripheral.c"

/* The chooser has its own suite; here it never takes a key. */
bool defy_profile_chooser_process(const struct zmk_position_state_changed *event) {
    ARG_UNUSED(event);
    return false;
}
#undef k_work_reschedule
#undef k_uptime_get
#undef sys_rand32_get
#undef k_current_get

/*
 * ZMK's layers, as app.overlay's keymap has them: layer 0 and those in
 * active_layers are active, and layer_order lists them lowest first.
 */
static uint32_t active_layers;
static zmk_keymap_layer_id_t layer_order[5];

zmk_keymap_layer_id_t zmk_keymap_layer_index_to_id(zmk_keymap_layer_index_t index) {
    return index < ARRAY_SIZE(layer_order) ? layer_order[index] : ZMK_KEYMAP_LAYER_ID_INVAL;
}

bool zmk_keymap_layer_active(zmk_keymap_layer_id_t layer) {
    return layer == 0 || (active_layers & BIT(layer));
}

static void set_layer(uint8_t layer, bool active) {
    WRITE_BIT(active_layers, layer, active);
    zassert_ok(raise_layer_state_changed(layer, active, false));
}

static void zassert_colours(struct defy_lighting_colour keys,
                            struct defy_lighting_colour underglow) {
    zassert_true(defy_lighting_colour_equal(desired.keys, keys), "keys %u %u %u", desired.keys.r,
                 desired.keys.g, desired.keys.b);
    zassert_true(defy_lighting_colour_equal(desired.underglow, underglow), "underglow %u %u %u",
                 desired.underglow.r, desired.underglow.g, desired.underglow.b);
}

static const struct defy_lighting_colour first_keys = {0x10, 0x20, 0x30};
static const struct defy_lighting_colour first_underglow = {4, 5, 6};
static const struct defy_lighting_colour white = {255, 255, 255};
static const struct defy_lighting_colour red = {255, 0, 0};
static const struct defy_lighting_colour blue = {0, 0, 255};
static const struct defy_lighting_colour dark = {0, 0, 0};

static int test_schedule(struct k_work_delayable *work, k_timeout_t timeout) {
    int64_t delay = k_ticks_to_ms_ceil64(timeout.ticks);
    if (work == &save_work) {
        zassert_equal(delay, 1000);
        schedules++;
    } else {
        zassert_equal(work, &idle_work);
        idle_schedules++;
        idle_delay = delay;
    }
    return 1;
}

/* Stands in for ZMK's own listeners, which run after the lighting's. See consumer.c. */
int consume_position(const zmk_event_t *event) {
    const struct zmk_position_state_changed *position = as_zmk_position_state_changed(event);
    zassert_not_null(position);
    consumed_events++;
    consumer_saw_enabled = desired.enabled;
    captured_event = copy_raised_zmk_position_state_changed(position);
    if (toggle_in_consumer) {
        zassert_ok(defy_lighting_command(DEFY_LIGHTING_TOGGLE));
    }
    return consumer_result;
}

static int key_event(uint8_t source, uint32_t position, bool pressed) {
    return raise_zmk_position_state_changed((struct zmk_position_state_changed){
        .source = source, .position = position, .state = pressed, .timestamp = now,
    });
}

static ssize_t read_setting(void *value, void *output, size_t length) {
    memcpy(output, value, length);
    return length;
}

static struct defy_lighting_state setting(uint32_t boot, uint32_t revision, bool enabled) {
    return (struct defy_lighting_state){boot, revision, enabled};
}

static int apply(uint32_t boot, uint32_t revision, bool enabled, uint8_t route,
                  uint32_t epoch) {
    struct defy_lighting_state state = setting(boot, revision, enabled);
    return defy_lighting_peripheral_apply(&state, route, epoch);
}

static void before(void *fixture) {
    ARG_UNUSED(fixture);
    desired = (struct defy_lighting_state){0};
    settings_ready = false;
    preferred_enabled = true;
    idle = false;
    last_activity_ms = 0;
    memset(wake_boot, 0, sizeof(wake_boot));
    now = 0;
    idle_schedules = consumed_events = 0;
    idle_delay = 0;
    consumer_result = ZMK_EV_EVENT_CAPTURED;
    consumer_saw_enabled = toggle_in_consumer = false;
    saved_enabled = true;
    look_index = saved_look = 0;
    level = saved_level = FIRST_LEVEL;
    active_layers = 0;
    for (size_t i = 0; i < ARRAY_SIZE(layer_order); i++) {
        layer_order[i] = i;
    }
    look_saves = 0;
    memset(saved_look_bytes, 0, sizeof(saved_look_bytes));
    save_attempts = 0;
    publishes = schedules = saves = renders = 0;
    save_error = render_error = 0;
    toggle_during_save = disconnect_during_render = false;
    rendered_known = rendered_enabled = false;
    rendered_view = (struct defy_lighting_view){0};
    memset(groups, 0, sizeof(groups));
    group_boot = 0;
    held = 0;
    memset(&applied_view, 0, sizeof(applied_view));
    applied = (struct defy_lighting_state){0};
    retired_boot = active_ble_epoch = 0;
    memset(lighting_routes, 0, sizeof(lighting_routes));
}

ZTEST(lighting, test_codec_roundtrip_and_strict_validation) {
    struct defy_lighting_state state = setting(123, 456, true), decoded;
    uint8_t bytes[DEFY_LIGHTING_CONTROL_SIZE];
    for (uint8_t opcode = DEFY_CONTROL_LIGHTING_SET; opcode <= DEFY_CONTROL_LIGHTING_ACK;
         opcode++) {
        zassert_ok(defy_lighting_control_encode(&state, opcode, bytes));
        zassert_ok(defy_lighting_control_decode(bytes, sizeof(bytes), opcode, &decoded));
        zassert_true(defy_lighting_equal(&state, &decoded));
    }
    zassert_equal(defy_lighting_control_decode(bytes, sizeof(bytes) - 1, DEFY_CONTROL_LIGHTING_ACK,
                                              &decoded), -EINVAL);
    zassert_equal(defy_lighting_control_decode(bytes, sizeof(bytes), DEFY_CONTROL_LIGHTING_SET,
                                              &decoded), -EPROTO);
    bytes[2] = 2;
    zassert_equal(defy_lighting_control_decode(bytes, sizeof(bytes), DEFY_CONTROL_LIGHTING_ACK,
                                              &decoded), -EPROTO);
    state.boot = 0;
    zassert_equal(defy_lighting_control_encode(&state, DEFY_CONTROL_LIGHTING_SET, bytes), -EINVAL);
    zassert_equal(defy_lighting_control_encode(&decoded, 1, bytes), -EINVAL);
}

ZTEST(lighting, test_codec_carries_the_colours_after_boot_and_revision) {
    struct defy_lighting_state state = {
        .boot = 0x01020304, .revision = 0x05060708, .enabled = true,
        .keys = {0x11, 0x12, 0x13}, .underglow = {0x21, 0x22, 0x23},
        .groups = 0x8001, .level = 0x30,
    };
    struct defy_lighting_state decoded;
    uint8_t bytes[DEFY_LIGHTING_CONTROL_SIZE];
    static const uint8_t expected[] = {3, DEFY_CONTROL_LIGHTING_SET, 1, 4, 3, 2, 1, 8, 7, 6, 5,
                                       0x11, 0x12, 0x13, 0x21, 0x22, 0x23, 0x01, 0x80, 0x30};
    BUILD_ASSERT(sizeof(expected) == DEFY_LIGHTING_CONTROL_SIZE);
    zassert_ok(defy_lighting_control_encode(&state, DEFY_CONTROL_LIGHTING_SET, bytes));
    zassert_mem_equal(bytes, expected, sizeof(expected));
    zassert_ok(defy_lighting_control_decode(bytes, sizeof(bytes), DEFY_CONTROL_LIGHTING_SET,
                                            &decoded));
    zassert_true(defy_lighting_equal(&state, &decoded));
    decoded.underglow.b++;
    zassert_false(defy_lighting_equal(&state, &decoded));
    decoded = state;
    decoded.level++;
    zassert_false(defy_lighting_equal(&state, &decoded));
    /* Groups need a level to draw them with. */
    bytes[19] = 0;
    zassert_equal(defy_lighting_control_decode(bytes, sizeof(bytes), DEFY_CONTROL_LIGHTING_SET,
                                              &decoded), -EPROTO);
    bytes[0] = 2;
    zassert_equal(defy_lighting_control_decode(bytes, sizeof(bytes), DEFY_CONTROL_LIGHTING_SET,
                                              &decoded), -EPROTO);
}

ZTEST(lighting, test_settings_gate_and_default_on) {
    struct defy_lighting_state state;
    zassert_false(defy_lighting_central_get(&state));
    zassert_equal(defy_lighting_command(DEFY_LIGHTING_TOGGLE), -EAGAIN);
    zassert_ok(lighting_settings_commit());
    zassert_true(defy_lighting_central_get(&state));
    zassert_true(state.enabled);
    zassert_equal(state.boot, 0x12345678);
    zassert_equal(state.revision, 1);
    zassert_true(defy_lighting_colour_equal(state.keys, first_keys));
    zassert_true(defy_lighting_colour_equal(state.underglow, first_underglow));
    zassert_equal(publishes, 2);
    zassert_ok(lighting_settings_commit());
    zassert_equal(publishes, 2);
}

ZTEST(lighting, test_load_off_and_reject_bad_settings) {
    uint8_t value = 0;
    zassert_ok(lighting_setting("lighting", 1, read_setting, &value));
    zassert_ok(lighting_settings_commit());
    zassert_false(desired.enabled);
    value = 2;
    zassert_equal(lighting_setting("lighting", 1, read_setting, &value), -EINVAL);
    zassert_equal(lighting_setting("lighting", 2, read_setting, &value), -EINVAL);
    zassert_equal(lighting_setting("unknown", 1, read_setting, &value), -ENOENT);
    zassert_equal(lighting_setting("lighting/more", 1, read_setting, &value), -ENOENT);
    zassert_false(desired.enabled);
}

ZTEST(lighting, test_rapid_toggle_back_to_saved_state_does_not_write_flash) {
    zassert_ok(lighting_settings_commit());
    zassert_ok(defy_lighting_command(DEFY_LIGHTING_TOGGLE));
    zassert_false(desired.enabled);
    zassert_equal(desired.revision, 2);
    zassert_ok(defy_lighting_command(DEFY_LIGHTING_TOGGLE));
    zassert_true(desired.enabled);
    zassert_equal(desired.revision, 3);
    save_setting(&save_work.work);
    zassert_equal(saves, 0);
    zassert_equal(publishes, 6);
}

ZTEST(lighting, test_save_coalesces_and_repairs_toggle_during_write) {
    zassert_ok(lighting_settings_commit());
    zassert_ok(defy_lighting_command(DEFY_LIGHTING_TOGGLE));
    toggle_during_save = true;
    save_setting(&save_work.work);
    zassert_false(saved_enabled);
    zassert_true(desired.enabled);
    zassert_equal(saves, 1);
    save_setting(&save_work.work);
    zassert_true(saved_enabled);
    zassert_equal(saved_value, 1);
    zassert_equal(saves, 2);
    save_setting(&save_work.work);
    zassert_equal(saves, 2);
}

ZTEST(lighting, test_failed_flash_save_has_bounded_retries) {
    zassert_ok(lighting_settings_commit());
    zassert_ok(defy_lighting_command(DEFY_LIGHTING_TOGGLE));
    save_error = -EIO;
    for (size_t i = 0; i < 3; i++) {
        save_setting(&save_work.work);
    }
    zassert_equal(saves, 3);
    zassert_equal(schedules, 3);
    zassert_true(saved_enabled);
    zassert_false(desired.enabled);
}

ZTEST(lighting, test_idle_expiry_changes_only_effective_state_once) {
    zassert_ok(lighting_settings_commit());
    zassert_equal(idle_schedules, 1);
    zassert_equal(idle_delay, 60000);
    now = 60000;
    idle_expired(&idle_work.work);
    zassert_false(desired.enabled);
    zassert_true(preferred_enabled);
    zassert_true(saved_enabled);
    zassert_equal(desired.revision, 2);
    zassert_equal(publishes, 4);
    zassert_equal(idle_schedules, 1);
    idle_expired(&idle_work.work);
    save_setting(&save_work.work);
    zassert_equal(publishes, 4);
    zassert_equal(saves, 0);
    zassert_equal(schedules, 0);
}

ZTEST(lighting, test_press_before_queued_expiry_rechecks_deadline) {
    zassert_ok(lighting_settings_commit());
    now = 59000;
    zassert_ok(key_event(0, 10, true));
    now = 60000;
    idle_expired(&idle_work.work);
    zassert_true(desired.enabled);
    zassert_equal(desired.revision, 1);
    zassert_equal(idle_delay, 59000);
    now = 118999;
    idle_expired(&idle_work.work);
    zassert_true(desired.enabled);
    zassert_equal(idle_delay, k_ticks_to_ms_ceil64(K_MSEC(1).ticks));
    now = 119000;
    idle_expired(&idle_work.work);
    zassert_false(desired.enabled);
}

ZTEST(lighting, test_press_after_expiry_restores_enabled_preference) {
    zassert_ok(lighting_settings_commit());
    now = 60000;
    idle_expired(&idle_work.work);
    zassert_ok(key_event(1, 69, true));
    zassert_true(desired.enabled);
    zassert_equal(desired.revision, 3);
    zassert_equal(last_activity_ms, now);
    idle_expired(&idle_work.work);
    zassert_true(desired.enabled);
    zassert_equal(idle_delay, 60000);
    zassert_equal(publishes, 6);
    zassert_equal(saves, 0);
}

ZTEST(lighting, test_saved_off_stays_dark_under_physical_activity) {
    uint8_t value = 0;
    zassert_ok(lighting_setting("lighting", 1, read_setting, &value));
    zassert_ok(lighting_settings_commit());
    now = 60000;
    idle_expired(&idle_work.work);
    zassert_ok(key_event(0, 5, true));
    zassert_false(desired.enabled);
    zassert_false(preferred_enabled);
    zassert_false(saved_enabled);
    zassert_equal(desired.revision, 1);
    zassert_equal(publishes, 2);
    zassert_equal(saves, 0);
}

ZTEST(lighting, test_toggle_while_idle_changes_preference_not_effective_darkness) {
    zassert_ok(lighting_settings_commit());
    now = 60000;
    idle_expired(&idle_work.work);
    zassert_ok(defy_lighting_command(DEFY_LIGHTING_TOGGLE));
    zassert_false(preferred_enabled);
    zassert_false(desired.enabled);
    zassert_equal(desired.revision, 2);
    save_setting(&save_work.work);
    zassert_equal(saved_value, 0);
    zassert_ok(defy_lighting_command(DEFY_LIGHTING_TOGGLE));
    zassert_true(preferred_enabled);
    zassert_false(desired.enabled);
    save_setting(&save_work.work);
    zassert_equal(saved_value, 1);
    zassert_equal(publishes, 4);
    zassert_ok(key_event(0, 5, true));
    zassert_true(desired.enabled);
}

ZTEST(lighting, test_consumed_toggle_press_uses_saved_preference_after_idle) {
    zassert_ok(lighting_settings_commit());
    now = 60000;
    idle_expired(&idle_work.work);
    toggle_in_consumer = true;
    consumer_result = ZMK_EV_EVENT_HANDLED;
    zassert_ok(key_event(0, 15, true));
    zassert_true(consumer_saw_enabled);
    zassert_false(preferred_enabled);
    zassert_false(desired.enabled);
    save_setting(&save_work.work);
    zassert_equal(saved_value, 0);
}

ZTEST(lighting, test_startup_settings_commit_starts_full_idle_interval) {
    now = 120000;
    zassert_ok(key_event(0, 1, true));
    zassert_equal(consumed_events, 1);
    zassert_equal(idle_schedules, 0);
    zassert_ok(lighting_settings_commit());
    zassert_equal(last_activity_ms, 120000);
    now = 179999;
    idle_expired(&idle_work.work);
    zassert_true(desired.enabled);
    zassert_equal(idle_delay, k_ticks_to_ms_ceil64(K_MSEC(1).ticks));
    now = 180000;
    idle_expired(&idle_work.work);
    zassert_false(desired.enabled);
}

ZTEST(lighting, test_transport_reads_do_not_extend_idle) {
    zassert_ok(lighting_settings_commit());
    struct defy_lighting_state state;
    for (now = 10000; now <= 60000; now += 10000) {
        zassert_true(defy_lighting_central_get(&state));
    }
    now = 60000;
    zassert_equal(last_activity_ms, 0);
    zassert_equal(idle_schedules, 1);
    idle_expired(&idle_work.work);
    zassert_false(desired.enabled);
}

ZTEST(lighting, test_captured_press_wakes_before_consumption_and_replay_does_not_refresh) {
    zassert_ok(lighting_settings_commit());
    now = 60000;
    idle_expired(&idle_work.work);
    zassert_ok(key_event(0, 3, true));
    zassert_true(consumer_saw_enabled);
    zassert_equal(consumed_events, 1);
    zassert_equal(last_activity_ms, 60000);
    now = 65000;
    zassert_ok(ZMK_EVENT_RELEASE(captured_event));
    zassert_equal(consumed_events, 1);
    zassert_equal(last_activity_ms, 60000);
}

ZTEST(lighting, test_release_and_invalid_position_forward_without_waking) {
    zassert_ok(lighting_settings_commit());
    now = 60000;
    idle_expired(&idle_work.work);
    unsigned int scheduled = idle_schedules;
    consumer_result = -EIO;
    zassert_equal(key_event(0, 1, false), -EIO);
    zassert_equal(key_event(0, DEFY_WIRED_KEY_COUNT, true), -EIO);
    zassert_equal(key_event(2, 1, true), -EIO);
    zassert_equal(key_event(ZMK_POSITION_STATE_CHANGE_SOURCE_LOCAL, 1, true), -EIO);
    zassert_equal(consumed_events, 4);
    zassert_false(desired.enabled);
    zassert_equal(last_activity_ms, 0);
    zassert_equal(idle_schedules, scheduled);
}

ZTEST(lighting, test_idle_transition_revision_wrap_skips_zero) {
    zassert_ok(lighting_settings_commit());
    desired.revision = UINT32_MAX;
    now = 60000;
    idle_expired(&idle_work.work);
    zassert_equal(desired.revision, 1);
    zassert_ok(key_event(0, 1, true));
    zassert_equal(desired.revision, 2);
}

ZTEST(lighting, test_sleep_wake_restores_lights_without_key_event_or_setting_write) {
    zassert_ok(lighting_settings_commit());
    now = 60000;
    idle_expired(&idle_work.work);
    now = 65000;
    struct defy_lighting_wake wake = {0, true, 123, 2000};
    defy_lighting_central_wake(&wake);
    zassert_true(desired.enabled);
    zassert_true(preferred_enabled);
    zassert_equal(desired.revision, 3);
    zassert_equal(last_activity_ms, 63000);
    zassert_equal(idle_delay, 58000);
    zassert_equal(consumed_events, 0);
    zassert_equal(schedules, 0);
    zassert_equal(saves, 0);
    now = 123000;
    idle_expired(&idle_work.work);
    zassert_false(desired.enabled);
}

ZTEST(lighting, test_sleep_wake_preserves_manual_off) {
    uint8_t value = 0;
    zassert_ok(lighting_setting("lighting", 1, read_setting, &value));
    zassert_ok(lighting_settings_commit());
    now = 60000;
    idle_expired(&idle_work.work);
    now = 65000;
    struct defy_lighting_wake wake = {1, true, 123, 2000};
    defy_lighting_central_wake(&wake);
    zassert_false(desired.enabled);
    zassert_false(preferred_enabled);
    zassert_false(saved_enabled);
    zassert_equal(last_activity_ms, 63000);
    zassert_equal(desired.revision, 1);
    zassert_equal(publishes, 2);
    zassert_equal(consumed_events, 0);
    zassert_equal(saves, 0);
}

ZTEST(lighting, test_sleep_wake_token_is_deduplicated_per_side) {
    zassert_ok(lighting_settings_commit());
    now = 65000;
    idle_expired(&idle_work.work);
    struct defy_lighting_wake wake = {0, true, 123, 2000};
    defy_lighting_central_wake(&wake);
    unsigned int scheduled = idle_schedules;
    now = 70000;
    defy_lighting_central_wake(&wake);
    zassert_equal(last_activity_ms, 63000);
    zassert_equal(idle_schedules, scheduled);
    wake.side = 1;
    defy_lighting_central_wake(&wake);
    zassert_equal(last_activity_ms, 68000);
    wake.side = 0;
    wake.boot++;
    now = 75000;
    defy_lighting_central_wake(&wake);
    zassert_equal(last_activity_ms, 73000);
    zassert_equal(consumed_events, 0);
}

ZTEST(lighting, test_sleep_wake_rejects_old_normal_and_invalid_reports) {
    zassert_ok(lighting_settings_commit());
    now = 65000;
    idle_expired(&idle_work.work);
    unsigned int scheduled = idle_schedules;
    struct defy_lighting_wake wake = {0, false, 123, 2000};
    defy_lighting_central_wake(&wake);
    wake.from_sleep = true;
    wake.age_ms = 60000;
    defy_lighting_central_wake(&wake);
    wake.age_ms = UINT32_MAX;
    defy_lighting_central_wake(&wake);
    wake.age_ms = 2000;
    wake.side = 2;
    defy_lighting_central_wake(&wake);
    wake.side = 0;
    wake.boot = 0;
    defy_lighting_central_wake(&wake);
    defy_lighting_central_wake(NULL);
    zassert_false(desired.enabled);
    zassert_equal(last_activity_ms, 0);
    zassert_equal(idle_schedules, scheduled);
    zassert_equal(consumed_events, 0);
}

ZTEST(lighting, test_sleep_wake_cannot_move_newer_key_activity_backwards) {
    zassert_ok(lighting_settings_commit());
    now = 65000;
    zassert_ok(key_event(0, 1, true));
    unsigned int scheduled = idle_schedules;
    now = 66000;
    struct defy_lighting_wake wake = {1, true, 123, 2000};
    defy_lighting_central_wake(&wake);
    zassert_equal(last_activity_ms, 65000);
    zassert_equal(idle_schedules, scheduled);
    now = 68000;
    wake.age_ms = 0;
    defy_lighting_central_wake(&wake);
    zassert_equal(last_activity_ms, 65000);
    zassert_equal(idle_schedules, scheduled);
}

ZTEST(lighting, test_sleep_wake_waits_for_settings_and_ignores_preboot_activity) {
    struct defy_lighting_wake wake = {0, true, 123, 100};
    now = 500;
    defy_lighting_central_wake(&wake);
    zassert_equal(wake_boot[0], 0);
    zassert_equal(idle_schedules, 0);
    zassert_ok(lighting_settings_commit());
    now = 600;
    wake.age_ms = 700;
    defy_lighting_central_wake(&wake);
    zassert_equal(last_activity_ms, 500);
    zassert_equal(idle_schedules, 1);
    now = 2000;
    wake.boot++;
    wake.age_ms = 500;
    defy_lighting_central_wake(&wake);
    zassert_equal(last_activity_ms, 1500);
    zassert_equal(idle_delay, 59500);
}

ZTEST(lighting, test_renderer_failure_does_not_acknowledge) {
    struct defy_lighting_state state;
    render_error = -ENODEV;
    zassert_equal(apply(1, 1, false, DEFY_LINK_WIRED, 1), -ENODEV);
    zassert_false(defy_lighting_peripheral_get(&state));
    render_error = 0;
    zassert_ok(apply(1, 1, false, DEFY_LINK_WIRED, 1));
    zassert_true(defy_lighting_peripheral_get(&state));
    zassert_false(state.enabled);
}

ZTEST(lighting, test_duplicate_cross_route_set_does_not_redraw) {
    defy_lighting_peripheral_ble_epoch(10);
    zassert_ok(apply(1, 1, false, DEFY_LINK_WIRED, 1));
    zassert_ok(apply(1, 1, false, DEFY_LINK_BLE, 10));
    zassert_ok(apply(1, 1, false, DEFY_LINK_WIRED, 1));
    zassert_equal(renders, 1);
    zassert_equal(lighting_routes[DEFY_LINK_BLE].boot, 1);
    zassert_equal(apply(1, 1, true, DEFY_LINK_BLE, 10), -EPROTO);
}

ZTEST(lighting, test_colours_reach_the_leds_and_a_changed_duplicate_is_refused) {
    struct defy_lighting_state state = setting(1, 1, true);
    state.keys = (struct defy_lighting_colour){1, 2, 3};
    zassert_ok(defy_lighting_peripheral_apply(&state, DEFY_LINK_WIRED, 1));
    zassert_true(defy_lighting_colour_equal(rendered_view.keys[34], state.keys));
    zassert_true(defy_lighting_colour_equal(rendered_view.underglow, state.underglow));
    state.underglow.g = 9;
    zassert_equal(defy_lighting_peripheral_apply(&state, DEFY_LINK_WIRED, 1), -EPROTO);
    zassert_equal(renders, 1);
    state.revision = 2;
    zassert_ok(defy_lighting_peripheral_apply(&state, DEFY_LINK_WIRED, 1));
    zassert_true(defy_lighting_colour_equal(rendered_view.underglow, state.underglow));
    zassert_true(defy_lighting_equal(&applied, &state));
}

ZTEST(lighting, test_older_opposite_setting_cannot_overtake_newer_route) {
    defy_lighting_peripheral_ble_epoch(10);
    zassert_ok(apply(1, 3, false, DEFY_LINK_BLE, 10));
    zassert_equal(apply(1, 2, true, DEFY_LINK_WIRED, 1), -ESTALE);
    zassert_false(applied.enabled);
    zassert_equal(renders, 1);
    zassert_ok(apply(1, 4, true, DEFY_LINK_WIRED, 1));
    zassert_true(applied.enabled);
}

ZTEST(lighting, test_unobserved_old_ble_token_rejected_after_two_wired_reboots) {
    defy_lighting_peripheral_ble_epoch(10);
    zassert_ok(apply(1, 1, true, DEFY_LINK_WIRED, 1));
    zassert_ok(apply(2, 1, false, DEFY_LINK_WIRED, 2));
    zassert_ok(apply(3, 1, true, DEFY_LINK_WIRED, 3));
    zassert_equal(apply(1, 2, false, DEFY_LINK_BLE, 10), -ESTALE);
    zassert_equal(applied.boot, 3);
    zassert_equal(renders, 3);
    defy_lighting_peripheral_ble_epoch(11);
    zassert_ok(apply(4, 1, false, DEFY_LINK_BLE, 11));
    zassert_equal(applied.boot, 4);
}

ZTEST(lighting, test_ble_disconnect_and_inflight_disconnect_cannot_acknowledge) {
    defy_lighting_peripheral_ble_epoch(10);
    disconnect_during_render = true;
    zassert_equal(apply(1, 1, true, DEFY_LINK_BLE, 10), -ESTALE);
    zassert_equal(applied.boot, 0);
    /* With no state before it, the half goes back to having none. */
    zassert_false(rendered_known);
    disconnect_during_render = false;
    zassert_equal(apply(1, 1, true, DEFY_LINK_BLE, 10), -ESTALE);
    defy_lighting_peripheral_ble_epoch(11);
    zassert_ok(apply(1, 1, true, DEFY_LINK_BLE, 11));
}

ZTEST(lighting, test_disconnect_during_on_restores_previously_acknowledged_off) {
    zassert_ok(apply(1, 1, false, DEFY_LINK_WIRED, 1));
    defy_lighting_peripheral_ble_epoch(10);
    disconnect_during_render = true;
    zassert_equal(apply(1, 2, true, DEFY_LINK_BLE, 10), -ESTALE);
    zassert_true(rendered_known);
    zassert_false(rendered_enabled);
    zassert_equal(applied.revision, 1);
    zassert_false(applied.enabled);
    disconnect_during_render = false;
    unsigned int before_duplicate = renders;
    zassert_ok(apply(1, 1, false, DEFY_LINK_WIRED, 1));
    zassert_equal(renders, before_duplicate);
    zassert_false(rendered_enabled);
}

ZTEST(lighting, test_serial_revision_wrap_and_invalid_state) {
    zassert_ok(apply(1, UINT32_MAX, false, DEFY_LINK_WIRED, 1));
    zassert_ok(apply(1, 1, true, DEFY_LINK_WIRED, 1));
    zassert_equal(apply(1, UINT32_MAX, false, DEFY_LINK_WIRED, 1), -ESTALE);
    zassert_equal(apply(0, 1, false, DEFY_LINK_WIRED, 1), -EINVAL);
}

ZTEST(lighting, test_commands_wait_for_settings_and_refuse_unknown_ones) {
    zassert_equal(defy_lighting_command(DEFY_LIGHTING_TOGGLE), -EAGAIN);
    zassert_equal(defy_lighting_command(DEFY_LIGHTING_NEXT), -EAGAIN);
    zassert_ok(lighting_settings_commit());
    zassert_equal(defy_lighting_command(DEFY_LIGHTING_NEXT + 1), -EINVAL);
    zassert_equal(desired.revision, 1);
    zassert_ok(defy_lighting_command(DEFY_LIGHTING_TOGGLE));
    zassert_false(desired.enabled);
}

ZTEST(lighting, test_brightness_steps_scale_the_colours_and_are_saved) {
    zassert_ok(lighting_settings_commit());
    zassert_ok(defy_lighting_command(DEFY_LIGHTING_DIMMER));
    zassert_equal(level, 192);
    zassert_equal(desired.revision, 2);
    zassert_colours((struct defy_lighting_colour){12, 24, 36},
                    (struct defy_lighting_colour){3, 4, 5});
    zassert_equal(schedules, 1);
    save_setting(&save_work.work);
    zassert_equal(saves, 0);
    zassert_equal(look_saves, 1);
    struct look_setting saved;
    memcpy(&saved, saved_look_bytes, sizeof(saved));
    zassert_equal(saved.config, looks_checksum());
    zassert_equal(saved.look, 0);
    zassert_equal(saved.level, 192);

    zassert_ok(defy_lighting_command(DEFY_LIGHTING_BRIGHTER));
    zassert_ok(defy_lighting_command(DEFY_LIGHTING_BRIGHTER));
    zassert_equal(level, 255);
    zassert_equal(desired.revision, 3);
    zassert_colours(first_keys, first_underglow);
    for (int i = 0; i < 20; i++) {
        zassert_ok(defy_lighting_command(DEFY_LIGHTING_DIMMER));
    }
    zassert_equal(level, 4);
    zassert_colours((struct defy_lighting_colour){0, 1, 1}, dark);
}

ZTEST(lighting, test_next_look_wraps_and_turns_the_lights_on) {
    zassert_ok(lighting_settings_commit());
    zassert_ok(defy_lighting_command(DEFY_LIGHTING_TOGGLE));
    zassert_false(desired.enabled);
    zassert_ok(defy_lighting_command(DEFY_LIGHTING_NEXT));
    zassert_true(desired.enabled);
    zassert_equal(look_index, 1);
    zassert_colours(white, dark);
    zassert_ok(defy_lighting_command(DEFY_LIGHTING_NEXT));
    zassert_equal(look_index, 0);
    zassert_colours(first_keys, first_underglow);
    /* Back to the saved preference and look: nothing to write. */
    save_setting(&save_work.work);
    zassert_equal(saves, 0);
    zassert_equal(look_saves, 0);
}

ZTEST(lighting, test_the_highest_active_layer_with_colours_decides) {
    zassert_ok(lighting_settings_commit());
    set_layer(1, true);
    zassert_colours(red, dark);
    set_layer(2, true);
    zassert_colours(first_keys, blue);
    uint32_t revision = desired.revision;
    set_layer(3, true);
    zassert_equal(desired.revision, revision);
    zassert_colours(first_keys, blue);
    set_layer(2, false);
    zassert_colours(red, dark);
    set_layer(1, false);
    set_layer(3, false);
    zassert_colours(first_keys, first_underglow);

    /* With the gaming layer moved above Fn, as Studio can, it decides. */
    layer_order[1] = 2;
    layer_order[2] = 1;
    set_layer(2, true);
    zassert_colours(first_keys, blue);
    set_layer(1, true);
    zassert_colours(red, dark);
}

ZTEST(lighting, test_layer_changes_while_dark_send_nothing) {
    zassert_ok(lighting_settings_commit());
    zassert_ok(defy_lighting_command(DEFY_LIGHTING_TOGGLE));
    zassert_equal(publishes, 4);
    set_layer(1, true);
    zassert_equal(desired.revision, 2);
    zassert_equal(publishes, 4);
    zassert_ok(defy_lighting_command(DEFY_LIGHTING_TOGGLE));
    zassert_true(desired.enabled);
    zassert_colours(red, dark);
}

ZTEST(lighting, test_a_saved_look_needs_the_same_looks) {
    struct look_setting value = {.config = looks_checksum(), .look = 1, .level = 64};
    zassert_ok(lighting_setting("look", sizeof(value), read_setting, &value));
    zassert_equal(look_index, 1);
    zassert_equal(level, 64);
    zassert_ok(lighting_settings_commit());
    zassert_colours((struct defy_lighting_colour){64, 64, 64}, dark);

    look_index = 0;
    level = FIRST_LEVEL;
    value.config ^= 1;
    zassert_ok(lighting_setting("look", sizeof(value), read_setting, &value));
    value.config ^= 1;
    value.look = ARRAY_SIZE(looks);
    zassert_ok(lighting_setting("look", sizeof(value), read_setting, &value));
    value.look = 0;
    value.level = 0;
    zassert_ok(lighting_setting("look", sizeof(value), read_setting, &value));
    zassert_equal(look_index, 0);
    zassert_equal(level, FIRST_LEVEL);
    zassert_equal(lighting_setting("look", sizeof(value) - 1, read_setting, &value), -EINVAL);
    zassert_equal(lighting_setting("look/more", sizeof(value), read_setting, &value), -ENOENT);
}

ZTEST(lighting, test_a_failed_save_writes_both_records_on_the_retry) {
    zassert_ok(lighting_settings_commit());
    zassert_ok(defy_lighting_command(DEFY_LIGHTING_TOGGLE));
    zassert_ok(defy_lighting_command(DEFY_LIGHTING_NEXT));
    zassert_ok(defy_lighting_command(DEFY_LIGHTING_TOGGLE));
    save_error = -EIO;
    save_setting(&save_work.work);
    zassert_equal(saves, 1);
    zassert_equal(look_saves, 0);
    zassert_true(saved_enabled);
    save_error = 0;
    save_setting(&save_work.work);
    zassert_equal(saves, 2);
    zassert_equal(look_saves, 1);
    zassert_false(saved_enabled);
    zassert_equal(saved_look, 1);
}

ZTEST(lighting, test_group_codec_and_validation) {
    struct defy_lighting_group group = {
        .boot = 0x01020304, .index = 5, .colour = {0xa1, 0xa2, 0xa3}, .leds = 0x412345678,
    };
    struct defy_lighting_group decoded;
    uint8_t bytes[DEFY_LIGHTING_GROUP_SIZE];
    static const uint8_t expected[] = {3, DEFY_CONTROL_LIGHTING_GROUP, 4, 3, 2, 1, 5,
                                       0xa1, 0xa2, 0xa3, 0x78, 0x56, 0x34, 0x12, 0x04};
    BUILD_ASSERT(sizeof(expected) == DEFY_LIGHTING_GROUP_SIZE);
    zassert_ok(defy_lighting_group_encode(&group, bytes));
    zassert_mem_equal(bytes, expected, sizeof(expected));
    zassert_ok(defy_lighting_group_decode(bytes, sizeof(bytes), &decoded));
    zassert_mem_equal(&decoded, &group, sizeof(group));
    zassert_equal(defy_lighting_group_decode(bytes, sizeof(bytes) - 1, &decoded), -EINVAL);

    bytes[14] = 0x08;
    zassert_equal(defy_lighting_group_decode(bytes, sizeof(bytes), &decoded), -EPROTO);
    bytes[14] = 0x04;
    bytes[6] = DEFY_LIGHTING_GROUPS;
    zassert_equal(defy_lighting_group_decode(bytes, sizeof(bytes), &decoded), -EPROTO);
    bytes[6] = 5;
    bytes[1] = DEFY_CONTROL_LIGHTING_SET;
    zassert_equal(defy_lighting_group_decode(bytes, sizeof(bytes), &decoded), -EPROTO);
    group.boot = 0;
    zassert_equal(defy_lighting_group_encode(&group, bytes), -EINVAL);
}

ZTEST(lighting, test_a_view_draws_the_named_groups_over_the_key_colour) {
    struct defy_lighting_group table[DEFY_LIGHTING_GROUPS] = {
        [0] = {.boot = 1, .index = 0, .colour = {255, 0, 0}, .leds = BIT64(0) | BIT64(1)},
        [2] = {.boot = 1, .index = 2, .colour = {0, 0, 255}, .leds = BIT64(1) | BIT64(34)},
        [3] = {.boot = 1, .index = 3, .colour = {0, 255, 0}, .leds = BIT64(2)},
    };
    struct defy_lighting_state state = setting(1, 1, true);
    state.keys = (struct defy_lighting_colour){1, 2, 3};
    state.underglow = (struct defy_lighting_colour){4, 5, 6};
    state.groups = BIT(0) | BIT(2) | BIT(3);
    state.level = 128;
    const struct defy_lighting_colour half_red = {128, 0, 0};
    const struct defy_lighting_colour half_blue = {0, 0, 128};
    struct defy_lighting_view view;
    /* Group 3 is named but not held, so its key keeps the key colour. */
    defy_lighting_view(&state, table, BIT(0) | BIT(2), &view);
    zassert_true(view.enabled);
    zassert_true(defy_lighting_colour_equal(view.keys[0], half_red));
    zassert_true(defy_lighting_colour_equal(view.keys[1], half_blue));
    zassert_true(defy_lighting_colour_equal(view.keys[2], state.keys));
    zassert_true(defy_lighting_colour_equal(view.keys[34], half_blue));
    zassert_true(defy_lighting_colour_equal(view.keys[33], state.keys));
    zassert_true(defy_lighting_colour_equal(view.underglow, state.underglow));
}

ZTEST(lighting, test_a_half_waits_for_the_groups_a_state_names) {
    struct defy_lighting_state state = setting(7, 1, true);
    state.groups = BIT(1);
    state.level = 255;
    zassert_equal(defy_lighting_peripheral_apply(&state, DEFY_LINK_WIRED, 1), -EAGAIN);
    zassert_equal(renders, 0);
    zassert_false(defy_lighting_peripheral_get(&state));

    struct defy_lighting_group group = {.boot = 7, .index = 1, .colour = {0, 9, 0},
                                        .leds = BIT64(3)};
    zassert_ok(defy_lighting_peripheral_group(&group));
    zassert_ok(defy_lighting_peripheral_apply(&state, DEFY_LINK_WIRED, 1));
    zassert_equal(renders, 1);
    zassert_true(defy_lighting_colour_equal(rendered_view.keys[3],
                                            (struct defy_lighting_colour){0, 9, 0}));

    /* A group from another Neuron boot drops the ones before it. */
    group.boot = 8;
    group.index = 2;
    zassert_ok(defy_lighting_peripheral_group(&group));
    zassert_equal(held, BIT(2));
    zassert_equal(group_boot, 8);
    state.revision = 2;
    zassert_equal(defy_lighting_peripheral_apply(&state, DEFY_LINK_WIRED, 1), -EAGAIN);

    group.index = DEFY_LIGHTING_GROUPS;
    zassert_equal(defy_lighting_peripheral_group(&group), -EINVAL);
    retired_boot = 9;
    group.boot = 9;
    group.index = 0;
    zassert_equal(defy_lighting_peripheral_group(&group), -ESTALE);
}

ZTEST(lighting, test_every_key_position_has_one_led_on_its_half) {
    uint8_t seen[2][DEFY_LIGHTING_KEY_LEDS] = {0};
    for (uint8_t position = 0; position < DEFY_WIRED_KEY_COUNT; position++) {
        int left = key_led(position, DEFY_WIRED_LEFT);
        int right = key_led(position, DEFY_WIRED_RIGHT);
        zassert_true((left >= 0) != (right >= 0), "position %u", position);
        if (left >= 0) {
            seen[DEFY_WIRED_LEFT][left]++;
        } else {
            seen[DEFY_WIRED_RIGHT][right]++;
        }
    }
    for (int side = 0; side < 2; side++) {
        for (int led = 0; led < DEFY_LIGHTING_KEY_LEDS; led++) {
            zassert_equal(seen[side][led], 1, "side %d LED %d", side, led);
        }
    }
    /* Each row runs from the outer end: =, -, the inner keys and the thumbs. */
    zassert_equal(key_led(0, DEFY_WIRED_LEFT), 0);
    zassert_equal(key_led(13, DEFY_WIRED_RIGHT), 0);
    zassert_equal(key_led(6, DEFY_WIRED_LEFT), 6);
    zassert_equal(key_led(7, DEFY_WIRED_RIGHT), 6);
    zassert_equal(key_led(14, DEFY_WIRED_RIGHT), -1);
    zassert_equal(key_led(42, DEFY_WIRED_LEFT), 21);
    zassert_equal(key_led(53, DEFY_WIRED_RIGHT), 21);
    zassert_equal(key_led(62, DEFY_WIRED_LEFT), 34);
    zassert_equal(key_led(69, DEFY_WIRED_RIGHT), 34);
}

ZTEST(lighting, test_key_groups_follow_the_deciding_layer) {
    struct defy_lighting_group group;
    zassert_equal(defy_lighting_central_group(DEFY_WIRED_LEFT, 0, &group), -EAGAIN);
    zassert_ok(lighting_settings_commit());
    zassert_equal(desired.groups, 0);
    set_layer(1, true);
    zassert_equal(desired.groups, BIT(0) | BIT(1));
    zassert_equal(desired.level, 255);
    zassert_colours(red, dark);
    /* The fifth layer has key colours only: they decide, with the look's colours. */
    set_layer(4, true);
    zassert_equal(desired.groups, BIT(2));
    zassert_colours(first_keys, first_underglow);
    /* Fn, between them, has layer colours only. */
    set_layer(4, false);
    set_layer(2, true);
    zassert_equal(desired.groups, 0);
    zassert_colours(first_keys, blue);

    zassert_ok(defy_lighting_central_group(DEFY_WIRED_LEFT, 0, &group));
    zassert_equal(group.boot, desired.boot);
    zassert_equal(group.index, 0);
    zassert_true(defy_lighting_colour_equal(group.colour, (struct defy_lighting_colour){0, 255}));
    zassert_equal(group.leds, BIT64(9) | BIT64(15) | BIT64(16) | BIT64(17));
    zassert_ok(defy_lighting_central_group(DEFY_WIRED_RIGHT, 0, &group));
    zassert_equal(group.leds, 0);
    zassert_ok(defy_lighting_central_group(DEFY_WIRED_LEFT, 1, &group));
    zassert_equal(group.leds, BIT64(1) | BIT64(2) | BIT64(3) | BIT64(4) | BIT64(5) | BIT64(16));
    zassert_ok(defy_lighting_central_group(DEFY_WIRED_RIGHT, 2, &group));
    zassert_equal(group.leds, BIT64(5));
    zassert_equal(defy_lighting_central_group(DEFY_WIRED_RIGHT, 3, &group), -ENOENT);
    zassert_equal(defy_lighting_central_group(2, 0, &group), -EINVAL);
}

ZTEST_SUITE(lighting, NULL, NULL, before, NULL, NULL);
