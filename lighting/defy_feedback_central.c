/* SPDX-License-Identifier: MIT */

#include <dygma/defy_control.h>
#include <dygma/defy_feedback.h>
#include <dygma/defy_lighting.h>
#include <zephyr/kernel.h>
#include <zmk/ble.h>
#include <zmk/endpoints.h>
#include <zmk/usb.h>
#include <dygma/defy_neuron_dock.h>
#include <dygma/defy_profile_chooser.h>
#include <dygma/defy_wrap.h>

BUILD_ASSERT(ZMK_BLE_PROFILE_COUNT <= DEFY_HOST_FEEDBACK_PROFILE_COUNT);

static uint32_t boot;
static uint32_t sequence;
static struct k_spinlock request_lock;

void defy_host_feedback_show_selection(void) {
    struct defy_lighting_state lighting;
    if (!defy_lighting_central_get(&lighting)) {
        return;
    }
    k_spinlock_key_t key = k_spin_lock(&request_lock);
    if (boot != lighting.boot) {
        boot = lighting.boot;
        sequence = 0;
    }
    if (++sequence == 0) {
        sequence = 1;
    }
    uint32_t request_boot = boot;
    uint32_t request_sequence = sequence;
    k_spin_unlock(&request_lock, key);
    enum zmk_transport selected = zmk_endpoint_get_selected().transport;
    enum zmk_transport preferred = zmk_endpoint_get_preferred_transport();
    bool ble_ready = zmk_ble_active_profile_is_connected();
    bool usb_ready = zmk_usb_is_hid_ready() && zmk_usb_get_status() != USB_DC_SUSPEND;
    bool docked =
        IS_ENABLED(CONFIG_DEFY_NEURON_DOCK) && defy_neuron_place() == DEFY_NEURON_DOCKED;
    struct defy_host_feedback state = {
        .boot = request_boot,
        .sequence = request_sequence,
        .remaining_ms = DEFY_HOST_FEEDBACK_DURATION_MS,
        .profile = zmk_ble_active_profile_index(),
        .flags = (preferred == ZMK_TRANSPORT_BLE ? DEFY_HOST_PREFERRED_BLE : 0) |
                 (preferred == ZMK_TRANSPORT_NONE ? DEFY_HOST_PREFERRED_NONE : 0) |
                 (ble_ready ? DEFY_HOST_PROFILE_CONNECTED : 0) |
                 (usb_ready ? DEFY_HOST_USB_READY : 0) |
                 (ble_ready && selected == ZMK_TRANSPORT_BLE ? DEFY_HOST_SELECTED_BLE : 0) |
                 (usb_ready && selected == ZMK_TRANSPORT_USB ? DEFY_HOST_SELECTED_USB : 0) |
                 (docked ? DEFY_HOST_DOCKED : 0),
    };
    defy_profile_chooser_feedback(&state);
    defy_wired_host_feedback_show(&state);
    defy_control_ble_host_feedback_show(&state);
}

DEFY_WRAP_REAL(zmk_ble_prof_next);
DEFY_WRAP_REAL(zmk_ble_prof_prev);
DEFY_WRAP_REAL(zmk_ble_prof_select);
DEFY_WRAP_REAL(zmk_endpoint_set_preferred_transport);
DEFY_WRAP_REAL(zmk_endpoint_toggle_preferred_transport);

int __wrap_zmk_ble_prof_next(void) {
    int err = __real_zmk_ble_prof_next();
    if (!err) {
        defy_host_feedback_show_selection();
    }
    return err;
}

int __wrap_zmk_ble_prof_prev(void) {
    int err = __real_zmk_ble_prof_prev();
    if (!err) {
        defy_host_feedback_show_selection();
    }
    return err;
}

int __wrap_zmk_ble_prof_select(uint8_t index) {
    int err = __real_zmk_ble_prof_select(index);
    if (!err) {
        defy_host_feedback_show_selection();
    }
    return err;
}

int __wrap_zmk_endpoint_set_preferred_transport(enum zmk_transport transport) {
    int err = __real_zmk_endpoint_set_preferred_transport(transport);
    if (!err) {
        defy_host_feedback_show_selection();
    }
    return err;
}

int __wrap_zmk_endpoint_toggle_preferred_transport(void) {
    int err = __real_zmk_endpoint_toggle_preferred_transport();
    if (!err) {
        defy_host_feedback_show_selection();
    }
    return err;
}

DEFY_WRAP_CHECK(zmk_ble_prof_next);
DEFY_WRAP_CHECK(zmk_ble_prof_prev);
DEFY_WRAP_CHECK(zmk_ble_prof_select);
DEFY_WRAP_CHECK(zmk_endpoint_set_preferred_transport);
DEFY_WRAP_CHECK(zmk_endpoint_toggle_preferred_transport);
