/* SPDX-License-Identifier: MIT */

#define DT_DRV_COMPAT dygma_defy_profile_chooser

#include <dygma/defy_profile_chooser.h>
#include <dygma/defy_wired_protocol.h>
#include <errno.h>
#include <string.h>
#include <zephyr/devicetree.h>
#include <zmk/ble.h>
#include <zmk/endpoints.h>

#define CHOOSER_IDLE_MS 30000
#define CHOOSER_CLEAR_MS 3000
#define CHOOSER_REFRESH_MS 500

/* The chooser's keys, from its behaviour's devicetree node. */
static const uint8_t select_keys[] = DT_INST_PROP(0, select_positions);
static const uint8_t clear_keys[] = DT_INST_PROP(0, clear_positions);
static const uint8_t usb_keys[] = DT_INST_PROP(0, usb_positions);
static const uint8_t close_keys[] = DT_INST_PROP(0, close_positions);

BUILD_ASSERT(ARRAY_SIZE(select_keys) == DEFY_HOST_FEEDBACK_PROFILE_COUNT &&
                 ARRAY_SIZE(clear_keys) == DEFY_HOST_FEEDBACK_PROFILE_COUNT,
             "a key to select and a key to clear each profile");

static K_MUTEX_DEFINE(chooser_lock);
static bool chooser_active;
static int64_t chooser_deadline;
static int64_t preview_deadline;
static uint32_t chooser_generation;
/* Keys pressed while the chooser was closed, as their source + 1: their release passes too. */
static uint8_t forwarded[DEFY_WIRED_KEY_COUNT];
/*
 * Keys the chooser took, by the chooser_generation that took them, 0 for none: their release
 * acts only in that generation.
 */
static uint32_t captured[DEFY_WIRED_KEY_COUNT];
/* When a held clear key clears its profile, 0 while none is held. */
static int64_t clear_at[DEFY_HOST_FEEDBACK_PROFILE_COUNT];
static uint8_t clears;

int __real_zmk_ble_prof_select(uint8_t index);
int __real_zmk_endpoint_set_preferred_transport(enum zmk_transport transport);

static void chooser_tick(struct k_work *work);
static K_WORK_DELAYABLE_DEFINE(chooser_work, chooser_tick);

static int key_index(const uint8_t *keys, size_t count, uint32_t position) {
    for (size_t i = 0; i < count; i++) {
        if (keys[i] == position) {
            return i;
        }
    }
    return -1;
}

static void close_chooser(void) {
    chooser_active = false;
    memset(clear_at, 0, sizeof(clear_at));
    preview_deadline = k_uptime_get() + DEFY_HOST_FEEDBACK_DURATION_MS;
}

void defy_profile_chooser_toggle(void) {
    k_mutex_lock(&chooser_lock, K_FOREVER);
    if (chooser_active) {
        close_chooser();
    } else {
        if (++chooser_generation == 0) {
            chooser_generation = 1;
        }
        chooser_active = true;
        chooser_deadline = k_uptime_get() + CHOOSER_IDLE_MS;
        preview_deadline = 0;
        memset(clear_at, 0, sizeof(clear_at));
    }
    k_mutex_unlock(&chooser_lock);
    k_work_reschedule(&chooser_work, K_NO_WAIT);
}

bool defy_profile_chooser_process(const struct zmk_position_state_changed *event) {
    if (event->source > DEFY_WIRED_RIGHT || event->position >= DEFY_WIRED_KEY_COUNT) {
        return false;
    }
    uint32_t position = event->position;
    k_mutex_lock(&chooser_lock, K_FOREVER);
    if (forwarded[position]) {
        if (!event->state && forwarded[position] == event->source + 1) {
            forwarded[position] = 0;
        }
        k_mutex_unlock(&chooser_lock);
        return false;
    }
    if (!chooser_active && !captured[position]) {
        if (event->state) {
            forwarded[position] = event->source + 1;
        }
        k_mutex_unlock(&chooser_lock);
        return false;
    }
    bool current = captured[position] == chooser_generation;
    int clear = key_index(clear_keys, ARRAY_SIZE(clear_keys), position);
    if (event->state) {
        if (chooser_active && !captured[position]) {
            captured[position] = chooser_generation;
            chooser_deadline = k_uptime_get() + CHOOSER_IDLE_MS;
            if (clear >= 0 && clear < ZMK_BLE_PROFILE_COUNT && !zmk_ble_profile_is_open(clear)) {
                clear_at[clear] = k_uptime_get() + CHOOSER_CLEAR_MS;
            }
        }
    } else {
        captured[position] = 0;
        if (clear >= 0) {
            clear_at[clear] = 0;
        }
        if (chooser_active && current) {
            int index = key_index(select_keys, ARRAY_SIZE(select_keys), position);
            if (index >= 0 && index < ZMK_BLE_PROFILE_COUNT) {
                if (!__real_zmk_ble_prof_select(index) &&
                    !__real_zmk_endpoint_set_preferred_transport(ZMK_TRANSPORT_BLE)) {
                    close_chooser();
                }
            } else if (key_index(usb_keys, ARRAY_SIZE(usb_keys), position) >= 0) {
                if (!__real_zmk_endpoint_set_preferred_transport(ZMK_TRANSPORT_USB)) {
                    close_chooser();
                }
            } else if (key_index(close_keys, ARRAY_SIZE(close_keys), position) >= 0) {
                close_chooser();
            }
        }
    }
    k_mutex_unlock(&chooser_lock);
    k_work_reschedule(&chooser_work, K_NO_WAIT);
    return true;
}

void defy_profile_chooser_feedback(struct defy_host_feedback *state) {
    k_mutex_lock(&chooser_lock, K_FOREVER);
    bool visible = chooser_active || preview_deadline > k_uptime_get();
    state->clears = clears;
    k_mutex_unlock(&chooser_lock);
    if (!visible) {
        return;
    }
    state->flags |= DEFY_HOST_CHOOSER;
    state->profile_count = ZMK_BLE_PROFILE_COUNT;
    for (uint8_t i = 0; i < ZMK_BLE_PROFILE_COUNT; i++) {
        if (!zmk_ble_profile_is_open(i)) {
            state->paired_profiles |= BIT(i);
        }
        if (zmk_ble_profile_is_connected(i)) {
            state->connected_profiles |= BIT(i);
        }
    }
}

static void chooser_tick(struct k_work *work) {
    ARG_UNUSED(work);
    int64_t now = k_uptime_get();
    k_mutex_lock(&chooser_lock, K_FOREVER);
    if (chooser_active && now >= chooser_deadline) {
        close_chooser();
    }
    if (chooser_active) {
        for (size_t i = 0; i < ARRAY_SIZE(clear_at); i++) {
            if (clear_at[i] && now >= clear_at[i]) {
                clear_at[i] = 0;
                if (i < ZMK_BLE_PROFILE_COUNT && !zmk_ble_profile_is_open(i) &&
                    !__real_zmk_ble_prof_select(i)) {
                    zmk_ble_clear_bonds();
                    clears++;
                }
            }
        }
    }
    bool again = chooser_active || preview_deadline > now;
    k_mutex_unlock(&chooser_lock);
    defy_host_feedback_show_selection();
    if (again) {
        k_work_reschedule(&chooser_work, K_MSEC(CHOOSER_REFRESH_MS));
    }
}
