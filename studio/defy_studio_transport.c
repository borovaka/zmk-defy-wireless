/*
 * Keeps Studio on USB while a USB host is ready, whichever output carries the
 * keys. studio/CMakeLists.txt renames the two ZMK calls these replace.
 *
 * SPDX-License-Identifier: MIT
 */

#include <zmk/endpoints.h>
#include <zmk/event_manager.h>
#include <zmk/events/endpoint_changed.h>
#include <zmk/events/usb_conn_state_changed.h>
#include <zmk/studio/core.h>
#include <zmk/usb.h>

/*
 * Studio's RPC picks its transport only on zmk_endpoint_changed. A USB change
 * alters defy_studio_get_selected_endpoint() without one, so its listener
 * gets a synthetic event.
 */
extern const struct zmk_listener zmk_listener_studio_rpc;

struct zmk_endpoint_instance defy_studio_get_selected_endpoint(void) {
    if (zmk_usb_is_hid_ready()) {
        return (struct zmk_endpoint_instance){.transport = ZMK_TRANSPORT_USB};
    }

    return zmk_endpoint_get_selected();
}

void defy_studio_ble_disconnected(void) {
    if (!zmk_usb_is_hid_ready()) {
        zmk_studio_core_lock();
    }
}

static int usb_changed(const zmk_event_t *event) {
    if (!as_zmk_usb_conn_state_changed(event)) {
        return ZMK_EV_EVENT_BUBBLE;
    }

    struct zmk_endpoint_changed_event refresh = {
        .header = {.event = &zmk_event_zmk_endpoint_changed},
        .data = {.endpoint = defy_studio_get_selected_endpoint()},
    };
    zmk_listener_studio_rpc.callback(&refresh.header);
    return ZMK_EV_EVENT_BUBBLE;
}

ZMK_LISTENER(defy_studio_usb, usb_changed);
ZMK_SUBSCRIPTION(defy_studio_usb, zmk_usb_conn_state_changed);
