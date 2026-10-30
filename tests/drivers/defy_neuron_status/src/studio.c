/* SPDX-License-Identifier: MIT */

#include <zephyr/ztest.h>
#include <zmk/endpoints.h>
#include <zmk/event_manager.h>
#include <zmk/events/endpoint_changed.h>
#include <zmk/events/usb_conn_state_changed.h>
#include <zmk/studio/core.h>
#include <zmk/usb.h>

#include "../../../../studio/defy_studio_transport.c"

/* ZMK's usb.c, which src/main.c builds: its status callback and the work that raises its event. */
void usb_status_cb(enum usb_dc_status_code status, const uint8_t *params);
extern struct k_work usb_status_notifier_work;

static bool locked;
static unsigned int endpoint_events;
static enum zmk_transport studio_transport;
static struct zmk_endpoint_instance typing_endpoint;

struct zmk_endpoint_instance zmk_endpoint_get_selected(void) { return typing_endpoint; }

void zmk_studio_core_lock(void) { locked = true; }

static int studio_refresh(const zmk_event_t *event) {
    zassert_not_null(as_zmk_endpoint_changed(event));
    enum zmk_transport selected = defy_studio_get_selected_endpoint().transport;
    if (studio_transport != selected) {
        studio_transport = selected;
        locked = true;
    }
    return ZMK_EV_EVENT_BUBBLE;
}

ZMK_LISTENER(studio_rpc, studio_refresh);

static int typing_endpoint_changed(const zmk_event_t *event) {
    zassert_not_null(as_zmk_endpoint_changed(event));
    ++endpoint_events;
    return ZMK_EV_EVENT_BUBBLE;
}

ZMK_LISTENER(typing_endpoint_observer, typing_endpoint_changed);
ZMK_SUBSCRIPTION(typing_endpoint_observer, zmk_endpoint_changed);

/* Moves ZMK's USB state and delivers its event, which typing never sees. */
static void usb_event(enum usb_dc_status_code status) {
    struct k_work_sync sync;
    usb_status_cb(status, NULL);
    k_work_flush(&usb_status_notifier_work, &sync);
    zassert_equal(endpoint_events, 0);
}

/* An event whose payload ZMK's USB state has since overtaken. */
static void stale_event(enum zmk_usb_conn_state state) {
    zassert_ok(raise_zmk_usb_conn_state_changed(
        (struct zmk_usb_conn_state_changed){.conn_state = state}));
    zassert_equal(endpoint_events, 0);
}

static void before(void *fixture) {
    ARG_UNUSED(fixture);
    typing_endpoint =
        (struct zmk_endpoint_instance){.transport = ZMK_TRANSPORT_BLE, .ble.profile_index = 3};
    studio_transport = ZMK_TRANSPORT_BLE;
    usb_event(USB_DC_DISCONNECTED);
    locked = false;
    endpoint_events = 0;
}

ZTEST(studio_transport, test_usb_studio_does_not_change_bluetooth_typing) {
    usb_event(USB_DC_CONFIGURED);
    zassert_equal(studio_transport, ZMK_TRANSPORT_USB);
    zassert_equal(zmk_endpoint_get_selected().transport, ZMK_TRANSPORT_BLE);
    zassert_equal(zmk_endpoint_get_selected().ble.profile_index, 3);
}

ZTEST(studio_transport, test_power_only_usb_keeps_bluetooth_studio) {
    usb_event(USB_DC_CONNECTED);
    zassert_equal(zmk_usb_get_conn_state(), ZMK_USB_CONN_POWERED);
    zassert_equal(studio_transport, ZMK_TRANSPORT_BLE);
    zassert_equal(defy_studio_get_selected_endpoint().ble.profile_index, 3);
    zassert_false(locked);
}

ZTEST(studio_transport, test_usb_disconnect_returns_to_bluetooth_and_locks) {
    usb_event(USB_DC_CONFIGURED);
    locked = false;
    usb_event(USB_DC_DISCONNECTED);
    zassert_equal(studio_transport, ZMK_TRANSPORT_BLE);
    zassert_true(locked);
    zassert_equal(typing_endpoint.transport, ZMK_TRANSPORT_BLE);
}

ZTEST(studio_transport, test_output_change_does_not_disconnect_or_lock_usb_studio) {
    usb_event(USB_DC_CONFIGURED);
    locked = false;
    typing_endpoint.transport = ZMK_TRANSPORT_USB;
    usb_event(USB_DC_SUSPEND);
    typing_endpoint.transport = ZMK_TRANSPORT_BLE;
    usb_event(USB_DC_RESUME);
    zassert_equal(studio_transport, ZMK_TRANSPORT_USB);
    zassert_false(locked);
}

ZTEST(studio_transport, test_host_bluetooth_disconnect_does_not_lock_usb_studio) {
    usb_event(USB_DC_CONFIGURED);
    locked = false;
    defy_studio_ble_disconnected();
    zassert_false(locked);
}

ZTEST(studio_transport, test_host_bluetooth_disconnect_locks_bluetooth_studio) {
    defy_studio_ble_disconnected();
    zassert_true(locked);
}

ZTEST(studio_transport, test_current_usb_state_wins_over_queued_event) {
    usb_event(USB_DC_CONFIGURED);
    stale_event(ZMK_USB_CONN_NONE);
    zassert_equal(studio_transport, ZMK_TRANSPORT_USB);
    usb_event(USB_DC_DISCONNECTED);
    stale_event(ZMK_USB_CONN_HID);
    zassert_equal(studio_transport, ZMK_TRANSPORT_BLE);
}

ZTEST_SUITE(studio_transport, NULL, NULL, before, NULL, NULL);
