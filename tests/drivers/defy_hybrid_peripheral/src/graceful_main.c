/* SPDX-License-Identifier: MIT */

#include <errno.h>

#include <zephyr/kernel.h>
#include <zephyr/ztest.h>
#include <zmk/split/transport/peripheral.h>
#include <dygma/defy_hybrid.h>
#include <dygma/defy_split_peripheral.h>
#include <dygma/defy_wired_protocol.h>

#include "defy_split_internal.h"
#include "fake_links.h"

static int key_event(bool pressed) {
    struct zmk_split_transport_peripheral_event event = {
        .type = ZMK_SPLIT_TRANSPORT_PERIPHERAL_EVENT_TYPE_KEY_POSITION_EVENT,
        .data.key_position_event = {.position = 28, .pressed = pressed},
    };
    return defy_hybrid_peripheral.api->report_event(&event);
}

static K_SEM_DEFINE(key_delivered, 0, 1);
static void deliver_key(struct k_work *work) {
    ARG_UNUSED(work);
    zassert_ok(key_event(true));
    k_sem_give(&key_delivered);
}
static K_WORK_DEFINE(key_work, deliver_key);

static K_SEM_DEFINE(service_blocked, 0, 1);
static K_SEM_DEFINE(service_release, 0, 1);
static void block_service(struct k_work *work) {
    ARG_UNUSED(work);
    k_sem_give(&service_blocked);
    k_sem_take(&service_release, K_FOREVER);
}
static K_WORK_DEFINE(service_blocker, block_service);

static bool cable_connected(void) {
    return defy_wired_peripheral.api->get_status().connections ==
           ZMK_SPLIT_TRANSPORT_CONNECTIONS_STATUS_ALL_CONNECTED;
}
static bool bluetooth_enabled(void) { return fake_links_snapshot().bluetooth_enabled; }
static bool bluetooth_disabled(void) { return !bluetooth_enabled(); }
static bool gate_open(void) { return fake_bluetooth_advertising_allowed(); }

static void await_condition(bool (*condition)(void)) {
    int64_t end = k_uptime_get() + 350;
    while (!condition() && k_uptime_get() < end) {
        k_sleep(K_MSEC(1));
    }
    zassert_true(condition());
}

static void connect_bluetooth(void) {
    fake_bluetooth_available(true);
    await_condition(bluetooth_enabled);
    fake_bluetooth_connected(true);
    k_sleep(K_MSEC(5));
}

static void before(void *fixture) {
    ARG_UNUSED(fixture);
    fake_bluetooth_resume_stop();
    fake_bluetooth_delay_disconnect(false);
    defy_hybrid_peripheral_cancel_sleep();
    k_sleep(K_MSEC(25));
    zassert_ok(defy_hybrid_peripheral.api->set_enabled(true));
    zassert_ok(key_event(false));
    zassert_ok(defy_hybrid_peripheral.api->set_enabled(false));
    await_condition(bluetooth_disabled);
    k_sleep(K_MSEC(10));
    fake_links_reset();
    zassert_ok(defy_hybrid_peripheral.api->set_enabled(true));
    k_sleep(K_MSEC(CONFIG_DEFY_WIRED_PERIPHERAL_TIMEOUT_MS + 5));
}

static int await_prepare(void) {
    int err;
    int64_t end = k_uptime_get() + 350;
    do {
        err = defy_hybrid_peripheral_prepare_sleep(0);
        if (err != -EINPROGRESS) {
            return err;
        }
        k_sleep(K_MSEC(5));
    } while (k_uptime_get() < end);
    return err;
}

ZTEST(graceful, test_disconnect_finishes_before_transport_quiesce) {
    connect_bluetooth();
    fake_bluetooth_delay_disconnect(true);
    zassert_equal(defy_hybrid_peripheral_prepare_sleep(0), -EINPROGRESS);
    await_condition(bluetooth_disabled);
    size_t exchanges = fake_links_snapshot().cable_exchanges;
    k_sleep(K_MSEC(30));
    zassert_true(fake_links_snapshot().cable_exchanges > exchanges);
    zassert_equal(defy_hybrid_peripheral_prepare_sleep(0), -EINPROGRESS);
    fake_bluetooth_connected(false);
    zassert_ok(await_prepare());
    exchanges = fake_links_snapshot().cable_exchanges;
    k_sleep(K_MSEC(30));
    zassert_equal(fake_links_snapshot().cable_exchanges, exchanges);
    zassert_false(gate_open());
    zassert_equal(fake_bluetooth_ref_count(), 0);
}

ZTEST(graceful, test_pending_connection_without_connected_callback_vetoes_pm) {
    connect_bluetooth();
    fake_bluetooth_connected(false);
    fake_bluetooth_delay_disconnect(true);
    fake_bluetooth_state(BT_CONN_STATE_CONNECTING);
    zassert_equal(defy_hybrid_peripheral_prepare_sleep(0), -EINPROGRESS);
    await_condition(bluetooth_disabled);
    zassert_equal(defy_hybrid_peripheral_prepare_sleep(0), -EINPROGRESS);
    fake_bluetooth_state(BT_CONN_STATE_CONNECTED);
    zassert_equal(defy_hybrid_peripheral_prepare_sleep(0), -EINPROGRESS);
    fake_bluetooth_state(BT_CONN_STATE_DISCONNECTING);
    zassert_equal(defy_hybrid_peripheral_prepare_sleep(0), -EINPROGRESS);
    fake_bluetooth_state(BT_CONN_STATE_DISCONNECTED);
    zassert_ok(await_prepare());
}

ZTEST(graceful, test_key_cancels_inflight_stop_without_late_disable) {
    connect_bluetooth();
    fake_bluetooth_pause_stop();
    zassert_equal(defy_hybrid_peripheral_prepare_sleep(0), -EINPROGRESS);
    zassert_ok(fake_bluetooth_await_stop());
    zassert_equal(fake_bluetooth_ref_count(), 1);
    unsigned int calls = fake_links_snapshot().bluetooth_enable_calls;
    k_sem_reset(&key_delivered);
    zassert_true(k_work_submit(&key_work) >= 0);
    zassert_ok(k_sem_take(&key_delivered, K_MSEC(100)));
    zassert_equal(defy_hybrid_peripheral_prepare_sleep(0), -EBUSY);
    k_sleep(K_MSEC(30));
    zassert_equal(fake_links_snapshot().bluetooth_enable_calls, calls);
    zassert_false(gate_open());
    zassert_equal(fake_links_snapshot().bluetooth_keys[3], BIT(4));
    fake_bluetooth_resume_stop();
    await_condition(gate_open);
    await_condition(bluetooth_enabled);
    zassert_equal(fake_links_snapshot().bluetooth_enable_calls, calls + 2);
    zassert_equal(fake_bluetooth_ref_count(), 0);
    zassert_ok(key_event(false));
}

ZTEST(graceful, test_stuck_hci_request_times_out_without_blocking_system_work) {
    connect_bluetooth();
    fake_bluetooth_pause_stop();
    zassert_equal(defy_hybrid_peripheral_prepare_sleep(0), -EINPROGRESS);
    zassert_ok(fake_bluetooth_await_stop());
    size_t exchanges = fake_links_snapshot().cable_exchanges;
    k_sleep(K_MSEC(1010));
    zassert_equal(defy_hybrid_peripheral_prepare_sleep(0), -ETIMEDOUT);
    zassert_true(fake_links_snapshot().cable_exchanges > exchanges);
    zassert_false(gate_open());
    zassert_equal(defy_hybrid_peripheral_prepare_sleep(0), -EBUSY);
    fake_bluetooth_resume_stop();
    await_condition(gate_open);
    await_condition(bluetooth_enabled);
    k_sleep(K_MSEC(30));
    zassert_true(bluetooth_enabled());
}

ZTEST(graceful, test_disconnect_timeout_restores_backend_once) {
    connect_bluetooth();
    fake_bluetooth_delay_disconnect(true);
    zassert_equal(defy_hybrid_peripheral_prepare_sleep(0), -EINPROGRESS);
    await_condition(bluetooth_disabled);
    unsigned int calls = fake_links_snapshot().bluetooth_enable_calls;
    k_sleep(K_MSEC(1010));
    zassert_equal(defy_hybrid_peripheral_prepare_sleep(0), -ETIMEDOUT);
    await_condition(bluetooth_enabled);
    zassert_true(gate_open());
    zassert_equal(fake_links_snapshot().bluetooth_enable_calls, calls + 1);
    k_sleep(K_MSEC(100));
    zassert_equal(fake_links_snapshot().bluetooth_enable_calls, calls + 1);
}

ZTEST(graceful, test_failed_advertising_stop_restarts_enabled_backend) {
    connect_bluetooth();
    fake_bluetooth_stop_error(-EIO);
    unsigned int calls = fake_links_snapshot().bluetooth_enable_calls;
    zassert_equal(defy_hybrid_peripheral_prepare_sleep(0), -EINPROGRESS);
    zassert_equal(await_prepare(), -EIO);
    await_condition(gate_open);
    zassert_true(bluetooth_enabled());
    zassert_equal(fake_links_snapshot().bluetooth_enable_calls, calls + 1);
    zassert_equal(fake_bluetooth_stop_calls(), 1);
    k_sleep(K_MSEC(100));
    zassert_equal(fake_links_snapshot().bluetooth_enable_calls, calls + 1);
    zassert_equal(fake_bluetooth_stop_calls(), 1);
}

ZTEST(graceful, test_live_cable_and_held_key_never_start_bluetooth_stop) {
    connect_bluetooth();
    zassert_ok(key_event(true));
    zassert_equal(defy_hybrid_peripheral_prepare_sleep(0), -EBUSY);
    zassert_ok(key_event(false));
    fake_cable_online(true);
    await_condition(cable_connected);
    zassert_equal(defy_hybrid_peripheral_prepare_sleep(0), -EBUSY);
    zassert_equal(fake_bluetooth_stop_calls(), 0);
    zassert_true(gate_open());
}

ZTEST(graceful, test_pending_advertiser_reference_is_retained_during_stop) {
    connect_bluetooth();
    fake_bluetooth_connected(false);
    fake_bluetooth_state(BT_CONN_STATE_CONNECTING);
    fake_bluetooth_pause_stop();
    zassert_equal(defy_hybrid_peripheral_prepare_sleep(0), -EINPROGRESS);
    zassert_ok(fake_bluetooth_await_stop());
    zassert_equal(fake_bluetooth_ref_count(), 1);
    fake_bluetooth_connected(true);
    fake_bluetooth_resume_stop();
    zassert_ok(await_prepare());
    zassert_equal(fake_bluetooth_ref_count(), 0);
    zassert_false(bluetooth_enabled());
}

ZTEST(graceful, test_key_cancels_queued_stop_before_any_controller_command) {
    connect_bluetooth();
    k_sem_reset(&service_blocked);
    k_sem_reset(&service_release);
    zassert_true(k_work_submit_to_queue(&service_work_q, &service_blocker) >= 0);
    zassert_ok(k_sem_take(&service_blocked, K_MSEC(100)));
    zassert_equal(defy_hybrid_peripheral_prepare_sleep(0), -EINPROGRESS);
    unsigned int calls = fake_links_snapshot().bluetooth_enable_calls;
    zassert_ok(key_event(true));
    zassert_equal(defy_hybrid_peripheral_prepare_sleep(0), -EBUSY);
    k_sleep(K_MSEC(20));
    zassert_equal(fake_links_snapshot().bluetooth_enable_calls, calls);
    k_sem_give(&service_release);
    k_sleep(K_MSEC(30));
    zassert_true(bluetooth_enabled());
    zassert_true(gate_open());
    zassert_equal(fake_bluetooth_stop_calls(), 0);
    zassert_equal(fake_links_snapshot().bluetooth_enable_calls, calls + 1);
    zassert_ok(key_event(false));
}

ZTEST_SUITE(graceful, NULL, NULL, before, NULL, NULL);
