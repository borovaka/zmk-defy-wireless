/* SPDX-License-Identifier: MIT */

#include <errno.h>
#include <string.h>

#include <zephyr/bluetooth/conn.h>
#include <zephyr/bluetooth/gatt.h>
#include <zephyr/bluetooth/uuid.h>
#include <zephyr/init.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/atomic.h>
#include <zephyr/ztest.h>

#include <dygma/defy_hybrid.h>
#include <dygma/defy_battery_central.h>
#include <dygma/defy_control.h>
#include <dygma/defy_wired_protocol.h>
#include <zmk/split/transport/central.h>

#include "fake_uptime.h"

static int battery_identify_error;
static uint32_t battery_epochs[2];
static uint8_t battery_sides_test[2];
int defy_battery_central_route_side(enum defy_link route, uint8_t source,
                                   uint32_t epoch, uint8_t *side) {
    zassert_equal(route, DEFY_LINK_BLE);
    if (battery_epochs[source] != epoch || battery_sides_test[source] > 1) {
        return -ENODATA;
    }
    *side = battery_sides_test[source];
    return 0;
}
int defy_battery_central_route_identify(enum defy_link route, uint8_t source,
                                       uint32_t epoch, uint8_t side) {
    zassert_equal(route, DEFY_LINK_BLE);
    zassert_true(source < 2 && epoch && side < 2);
    return battery_identify_error;
}
void defy_control_ble_connected(struct bt_conn *conn, uint8_t slot, uint32_t epoch) {
    ARG_UNUSED(conn);
    battery_epochs[slot] = epoch;
    battery_sides_test[slot] = DEFY_WIRED_SIDE_UNKNOWN;
}
void defy_control_ble_disconnected(struct bt_conn *conn, uint8_t slot, uint32_t epoch) {
    ARG_UNUSED(conn);
    if (battery_epochs[slot] == epoch) {
        battery_sides_test[slot] = DEFY_WIRED_SIDE_UNKNOWN;
    }
}

struct k_work_q split_central_split_run_q;
static struct k_work *queued_split_work;
static unsigned int split_work_submissions;
static bool in_service;
static bool run_system_work;
static int maintenance_wire_error;
static bool maintenance_wire_paused;
static int submit_real_work(struct k_work *work) { return k_work_submit(work); }

static int submit_test_work(struct k_work *work) {
    return run_system_work ? submit_real_work(work) : 1;
}
static int submit_test_split_work(struct k_work_q *queue, struct k_work *work) {
    zassert_equal(queue, &split_central_split_run_q);
    zassert_true(!queued_split_work || queued_split_work == work);
    queued_split_work = work;
    split_work_submissions++;
    return 1;
}

#define k_work_submit submit_test_work
#define k_work_submit_to_queue submit_test_split_work
#undef ZMK_SPLIT_TRANSPORT_CENTRAL_REGISTER
#define ZMK_SPLIT_TRANSPORT_CENTRAL_REGISTER(name, transport_api, priority)                           \
    struct zmk_split_transport_central name = {.api = transport_api}
#include "../../../../drivers/split/defy_hybrid_central.c"
#include "../../../../drivers/split/defy_hybrid_ble.c"
#undef k_work_submit
#undef k_work_submit_to_queue
#undef k_uptime_get

static struct {
    uint8_t side;
    uint8_t position;
    bool pressed;
} events[512];
static size_t event_count;
static size_t other_event_count;
static uint8_t other_event_side;
static struct {
    uint8_t side;
    uint8_t bitmap[DEFY_WIRED_BITMAP_SIZE];
    int64_t received;
    size_t preceding_key_events;
} input_snapshots[512];
static size_t input_snapshot_count;
static size_t input_side_count[2];
static unsigned int input_reset_count;
static size_t input_reset_preceding_key_events;
static int downstream_error;
static int64_t downstream_delay;
static struct {
    uint8_t kind;
    uint8_t flags;
    uint16_t value;
    uint32_t extra;
    size_t preceding_key_events;
} traces[256];
static size_t trace_count;

void defy_key_trace_add(uint8_t kind, uint8_t flags, uint16_t value, uint32_t extra) {
    zassert_true(trace_count < ARRAY_SIZE(traces));
    traces[trace_count].kind = kind;
    traces[trace_count].flags = flags;
    traces[trace_count].value = value;
    traces[trace_count].extra = extra;
    traces[trace_count++].preceding_key_events = event_count;
}
static bool raw_available = true;
static unsigned int raw_starts;

void defy_usb_input_snapshot(uint8_t side, const uint8_t bitmap[DEFY_WIRED_BITMAP_SIZE],
                              int64_t received) {
    zassert_true(side < 2);
    zassert_true(atomic_get(&enabled));
    zassert_false(defy_hybrid_central_is_maintenance());
    zassert_true(input_snapshot_count < ARRAY_SIZE(input_snapshots));
    input_snapshots[input_snapshot_count].side = side;
    memcpy(input_snapshots[input_snapshot_count].bitmap, bitmap, DEFY_WIRED_BITMAP_SIZE);
    input_snapshots[input_snapshot_count].received = received;
    input_snapshots[input_snapshot_count].preceding_key_events = event_count;
    input_snapshot_count++;
    input_side_count[side]++;
}

void defy_usb_input_reset(void) {
    input_reset_count++;
    input_reset_preceding_key_events = event_count;
}

static int raw_set_enabled(bool value) {
    raw_starts += value;
    return 0;
}
static struct zmk_split_transport_status raw_get_status(void) {
    return (struct zmk_split_transport_status){.available = raw_available};
}
static int raw_set_callback(zmk_split_transport_central_status_changed_cb_t callback) {
    zassert_equal(callback, raw_status_changed);
    return 0;
}
static int raw_send_command(uint8_t source, struct zmk_split_transport_central_command command) {
    ARG_UNUSED(command);
    return source;
}
static const struct zmk_split_transport_central_api raw_api = {
    .send_command = raw_send_command,
    .set_enabled = raw_set_enabled,
    .get_status = raw_get_status,
    .set_status_callback = raw_set_callback,
};
const struct zmk_split_transport_central defy_wired_central = {.api = &raw_api};
struct zmk_split_transport_central bt_central = {.api = &raw_api};

int defy_wired_central_set_maintenance(bool value) {
    zassert_true(defy_hybrid_central_is_maintenance());
    if (maintenance_wire_error) {
        return maintenance_wire_error;
    }
    maintenance_wire_paused = value;
    return 0;
}

int zmk_split_transport_central_peripheral_event_handler(
    const struct zmk_split_transport_central *transport, uint8_t source,
    struct zmk_split_transport_peripheral_event event) {
    zassert_equal(transport, &defy_hybrid_central);
    if (event.type != ZMK_SPLIT_TRANSPORT_PERIPHERAL_EVENT_TYPE_KEY_POSITION_EVENT) {
        other_event_count++;
        other_event_side = source;
        return downstream_error;
    }
    zassert_true(event_count < ARRAY_SIZE(events));
    events[event_count].side = source;
    events[event_count].position = event.data.key_position_event.position;
    events[event_count++].pressed = event.data.key_position_event.pressed;
    now += downstream_delay;
    downstream_delay = 0;
    return downstream_error;
}

static uint32_t connection_tokens[3];
static int connection_slots[3];
static int connection_refs[3];
static bt_security_t connection_security[3];
static int discover_result;
static int subscribe_result;
static int discover_calls;
static int subscribe_calls;
static int original_discover_calls;
static int original_notify_calls;
static int original_subscribe_calls;
static uint8_t original_subscribe_error;
static int disconnect_results[3];
static unsigned int disconnect_calls[3];
static int disconnect_refs[3];
static int64_t disconnect_times[3][8];
static void (*disconnect_hook)(struct bt_conn *conn);
static void (*subscribe_hook)(struct bt_conn *conn);
static void (*completion_hook)(struct bt_conn *conn, struct bt_gatt_subscribe_params *params);

static struct bt_conn *connection(unsigned int index) {
    return (struct bt_conn *)&connection_tokens[index];
}

static int connection_index(struct bt_conn *conn) {
    for (unsigned int i = 0; i < ARRAY_SIZE(connection_tokens); i++) {
        if (connection(i) == conn) {
            return i;
        }
    }
    zassert_unreachable();
    return -1;
}

int peripheral_slot_index_for_conn(struct bt_conn *conn) {
    return connection_slots[connection_index(conn)];
}

struct bt_conn *bt_conn_ref(struct bt_conn *conn) {
    connection_refs[connection_index(conn)]++;
    return conn;
}

void bt_conn_unref(struct bt_conn *conn) {
    zassert_true(connection_refs[connection_index(conn)] > 0);
    connection_refs[connection_index(conn)]--;
}

bt_security_t bt_conn_get_security(const struct bt_conn *conn) {
    return connection_security[connection_index((struct bt_conn *)conn)];
}

int bt_conn_disconnect(struct bt_conn *conn, uint8_t reason) {
    int index = connection_index(conn);
    zassert_false(in_service);
    zassert_equal(reason, BT_HCI_ERR_REMOTE_USER_TERM_CONN);
    zassert_true(disconnect_calls[index] < ARRAY_SIZE(disconnect_times[index]));
    disconnect_times[index][disconnect_calls[index]++] = now;
    disconnect_refs[index] = connection_refs[index];
    void (*hook)(struct bt_conn *) = disconnect_hook;
    disconnect_hook = NULL;
    if (hook) {
        hook(conn);
    }
    return disconnect_results[index];
}

int bt_gatt_discover(struct bt_conn *conn, struct bt_gatt_discover_params *params) {
    ARG_UNUSED(conn);
    ARG_UNUSED(params);
    discover_calls++;
    return discover_result;
}

int bt_gatt_subscribe(struct bt_conn *conn, struct bt_gatt_subscribe_params *params) {
    ARG_UNUSED(params);
    subscribe_calls++;
    int result = subscribe_result;
    void (*hook)(struct bt_conn *) = subscribe_hook;
    subscribe_hook = NULL;
    if (hook) {
        hook(conn);
    }
    return result;
}

uint16_t bt_gatt_attr_value_handle(const struct bt_gatt_attr *attr) {
    return ((const struct bt_gatt_chrc *)attr->user_data)->value_handle;
}

static void reset(void *fixture) {
    ARG_UNUSED(fixture);
    run_system_work = false;
    maintenance_wire_error = 0;
    maintenance_wire_paused = false;
    atomic_clear(&maintenance);
    atomic_clear(&maintenance_generation);
    k_sem_reset(&maintenance_ready);
    k_work_cancel_delayable(&timeout_work);
    k_msgq_purge(&hybrid_snapshots);
    memset(sides, 0, sizeof(sides));
    memset(ble_links, 0, sizeof(ble_links));
    atomic_set(&slot_sides[0], DEFY_WIRED_SIDE_UNKNOWN);
    atomic_set(&slot_sides[1], DEFY_WIRED_SIDE_UNKNOWN);
    memset(ble_latest, 0, sizeof(ble_latest));
    memset(peers, 0, sizeof(peers));
    battery_identify_error = 0;
    memset(battery_epochs, 0, sizeof(battery_epochs));
    memset(battery_sides_test, DEFY_WIRED_SIDE_UNKNOWN, sizeof(battery_sides_test));
    memset(discoveries, 0, sizeof(discoveries));
    memset(subscriptions, 0, sizeof(subscriptions));
    memset(diagnostics, 0, sizeof(diagnostics));
    memset(connection_refs, 0, sizeof(connection_refs));
    for (size_t i = 0; i < ARRAY_SIZE(connection_security); i++) {
        connection_security[i] = BT_SECURITY_L1;
    }
    memset(disconnect_results, 0, sizeof(disconnect_results));
    memset(disconnect_calls, 0, sizeof(disconnect_calls));
    memset(disconnect_refs, 0, sizeof(disconnect_refs));
    memset(disconnect_times, 0, sizeof(disconnect_times));
    disconnect_hook = NULL;
    subscribe_hook = NULL;
    completion_hook = NULL;
    queued_split_work = NULL;
    split_work_submissions = 0;
    in_service = false;
    connection_slots[0] = 0;
    connection_slots[1] = 1;
    connection_slots[2] = -1;
    ble_revision = 0;
    now = 0;
    event_count = 0;
    trace_count = 0;
    trace_order = 0;
    trace_applying = 0;
    other_event_count = 0;
    other_event_side = DEFY_WIRED_SIDE_UNKNOWN;
    input_snapshot_count = 0;
    memset(input_side_count, 0, sizeof(input_side_count));
    input_reset_count = 0;
    input_reset_preceding_key_events = 0;
    downstream_error = 0;
    downstream_delay = 0;
    discover_result = 0;
    subscribe_result = 0;
    discover_calls = 0;
    subscribe_calls = 0;
    original_discover_calls = 0;
    original_notify_calls = 0;
    original_subscribe_calls = 0;
    original_subscribe_error = 0;
    raw_available = true;
    raw_starts = 0;
    wired_started = false;
    ble_started = false;
    status_callback = NULL;
    atomic_set(&connected_sides, 0);
    zassert_ok(set_enabled(true));
}

static void drain(void) { process_snapshots(); }

static void poll_service(void) {
    in_service = true;
    service_work_cb(NULL);
    in_service = false;
    k_work_cancel_delayable(&timeout_work);
}

static void run_split_work(void) {
    struct k_work *work = queued_split_work;
    queued_split_work = NULL;
    if (work) {
        work->handler(work);
    }
}

static void service(void) {
    poll_service();
    run_split_work();
}

static void set_position(uint8_t *bitmap, int position) {
    memset(bitmap, 0, DEFY_HYBRID_BLE_FRAME_SIZE);
    if (position >= 0) {
        bitmap[position / 8] |= BIT(position % 8);
    }
}

static void ble(uint8_t slot, uint32_t epoch, int position) {
    uint8_t frame[DEFY_HYBRID_BLE_FRAME_SIZE];
    set_position(frame, position);
    zassert_ok(defy_hybrid_central_ble_update(slot, epoch, true, frame, sizeof(frame)));
    drain();
}

static void cable(uint8_t side, uint32_t epoch, int position) {
    uint8_t frame[DEFY_HYBRID_BLE_FRAME_SIZE];
    set_position(frame, position);
    zassert_ok(defy_hybrid_central_wired_update(side, epoch, true, frame));
    drain();
}

static void cable_off(uint8_t side, uint32_t epoch) {
    zassert_ok(defy_hybrid_central_wired_update(side, epoch, false, NULL));
    drain();
}

static void ble_off(uint8_t slot, uint32_t epoch) {
    zassert_ok(defy_hybrid_central_ble_update(slot, epoch, false, NULL, 0));
    drain();
}

static void assert_event(size_t index, uint8_t side, uint8_t position, bool pressed) {
    zassert_true(index < event_count);
    zassert_equal(events[index].side, side);
    zassert_equal(events[index].position, position);
    zassert_equal(events[index].pressed, pressed);
}

ZTEST(defy_hybrid_central, test_ble_slot_order_does_not_define_physical_side) {
    ble(0, 1, 7);
    ble(1, 1, 0);
    zassert_equal(event_count, 2);
    assert_event(0, DEFY_WIRED_RIGHT, 7, true);
    assert_event(1, DEFY_WIRED_LEFT, 0, true);
    uint8_t available[2];
    zassert_equal(get_available_source_ids(available), 2);
    zassert_equal(available[0], DEFY_WIRED_LEFT);
    zassert_equal(available[1], DEFY_WIRED_RIGHT);
}

ZTEST(defy_hybrid_central, test_empty_unknown_slot_is_mapped_by_first_physical_key) {
    ble(0, 1, -1);
    zassert_equal(event_count, 0);
    zassert_equal(atomic_get(&slot_sides[0]), DEFY_WIRED_SIDE_UNKNOWN);
    ble(0, 1, 7);
    ble(0, 1, -1);
    zassert_equal(event_count, 2);
    assert_event(1, DEFY_WIRED_RIGHT, 7, false);
}

ZTEST(defy_hybrid_central, test_cable_takes_over_without_duplicating_held_keys) {
    ble(1, 1, 0);
    cable(DEFY_WIRED_LEFT, 9, 0);
    zassert_equal(event_count, 1);
    ble(1, 1, 1);
    zassert_equal(event_count, 1);
    cable(DEFY_WIRED_LEFT, 9, 1);
    zassert_equal(event_count, 3);
    assert_event(1, DEFY_WIRED_LEFT, 0, false);
    assert_event(2, DEFY_WIRED_LEFT, 1, true);
}

ZTEST(defy_hybrid_central, test_cable_loss_keeps_held_key_continuous_until_fresh_ble_snapshot) {
    ble(1, 1, 0);
    cable(DEFY_WIRED_LEFT, 9, 0);
    cable_off(DEFY_WIRED_LEFT, 9);
    zassert_equal(event_count, 1);
    drain();
    zassert_equal(event_count, 1);
    ble(1, 1, 0);
    zassert_equal(event_count, 1);
    ble(1, 1, -1);
    assert_event(1, DEFY_WIRED_LEFT, 0, false);
}

ZTEST(defy_hybrid_central, test_handoff_grace_expires_without_fresh_ble_snapshot) {
    ble(0, 1, 0);
    cable(DEFY_WIRED_LEFT, 9, 0);
    cable_off(DEFY_WIRED_LEFT, 9);
    now = CONFIG_DEFY_HYBRID_HANDOFF_GRACE_MS - 1;
    drain();
    zassert_equal(event_count, 1);
    now++;
    drain();
    assert_event(1, DEFY_WIRED_LEFT, 0, false);
    ble(0, 1, -1);
    zassert_equal(event_count, 2);
}

ZTEST(defy_hybrid_central, test_all_source_loss_releases_held_key_during_handoff_grace) {
    ble(0, 1, 0);
    cable(DEFY_WIRED_LEFT, 9, 0);
    cable_off(DEFY_WIRED_LEFT, 9);
    ble_off(0, 1);
    zassert_equal(event_count, 2);
    assert_event(1, DEFY_WIRED_LEFT, 0, false);
}

ZTEST(defy_hybrid_central, test_slow_handler_does_not_defer_expired_handoff_release) {
    ble(0, 1, 0);
    cable(DEFY_WIRED_LEFT, 9, 0);
    cable_off(DEFY_WIRED_LEFT, 9);
    now = CONFIG_DEFY_HYBRID_HANDOFF_GRACE_MS - 1;
    downstream_delay = 2;
    ble(1, 1, 7);
    zassert_equal(event_count, 3);
    assert_event(1, DEFY_WIRED_RIGHT, 7, true);
    assert_event(2, DEFY_WIRED_LEFT, 0, false);
}

ZTEST(defy_hybrid_central, test_mixed_halves_and_partial_source_loss_are_independent) {
    cable(DEFY_WIRED_LEFT, 9, 0);
    ble(0, 1, 7);
    cable_off(DEFY_WIRED_LEFT, 9);
    zassert_equal(event_count, 3);
    assert_event(2, DEFY_WIRED_LEFT, 0, false);
    zassert_equal(sides[DEFY_WIRED_RIGHT].output[0], BIT(7));
    ble_off(0, 1);
    assert_event(3, DEFY_WIRED_RIGHT, 7, false);
}

ZTEST(defy_hybrid_central, test_cable_session_change_preserves_held_key_and_ignores_old_offline) {
    cable(DEFY_WIRED_LEFT, 9, 0);
    cable(DEFY_WIRED_LEFT, 10, 0);
    zassert_equal(event_count, 1);
    zassert_equal(sides[DEFY_WIRED_LEFT].wired_session, 10);
    cable_off(DEFY_WIRED_LEFT, 9);
    zassert_equal(event_count, 1);
    cable(DEFY_WIRED_LEFT, 10, -1);
    assert_event(1, DEFY_WIRED_LEFT, 0, false);
}

ZTEST(defy_hybrid_central, test_new_cable_session_during_ble_handoff_keeps_held_key) {
    ble(0, 1, 0);
    cable(DEFY_WIRED_LEFT, 9, 0);
    cable_off(DEFY_WIRED_LEFT, 9);
    cable(DEFY_WIRED_LEFT, 10, 0);
    zassert_equal(event_count, 1);
    zassert_false(sides[DEFY_WIRED_LEFT].await_ble_snapshot);
    ble(0, 1, -1);
    zassert_equal(event_count, 1);
    cable(DEFY_WIRED_LEFT, 10, -1);
    assert_event(1, DEFY_WIRED_LEFT, 0, false);
}

ZTEST(defy_hybrid_central, test_ble_timeout_releases_keys_and_empty_report_recovers_routing) {
    ble(1, 3, 0);
    struct zmk_split_transport_central_command command = {
        .type = ZMK_SPLIT_TRANSPORT_CENTRAL_CMD_TYPE_SET_HID_INDICATORS,
    };
    struct zmk_split_transport_peripheral_event event = {
        .type = ZMK_SPLIT_TRANSPORT_PERIPHERAL_EVENT_TYPE_BATTERY_EVENT,
        .data.battery_event.level = 80,
    };
    zassert_equal(send_command(DEFY_WIRED_LEFT, command), 1);
    zassert_ok(defy_hybrid_central_peripheral_event(1, event));
    zassert_equal(other_event_count, 1);
    zassert_equal(other_event_side, DEFY_WIRED_LEFT);
    now = CONFIG_DEFY_HYBRID_BLE_TIMEOUT_MS;
    drain();
    assert_event(1, DEFY_WIRED_LEFT, 0, false);
    zassert_equal(ble_links[1].side, DEFY_WIRED_LEFT);
    zassert_equal(ble_links[1].epoch, 3);
    zassert_false(ble_links[1].connected);
    zassert_equal(atomic_get(&slot_sides[1]), DEFY_WIRED_SIDE_UNKNOWN);
    zassert_equal(get_status().connections, ZMK_SPLIT_TRANSPORT_CONNECTIONS_STATUS_DISCONNECTED);
    zassert_equal(send_command(DEFY_WIRED_LEFT, command), -ENOTSUP);
    zassert_equal(defy_hybrid_central_peripheral_event(1, event),
                  -ENOTCONN);
    zassert_equal(defy_hybrid_central_peripheral_event(2, event), -EINVAL);
    ble(1, 3, -1);
    zassert_equal(event_count, 2);
    zassert_true(ble_links[1].connected);
    zassert_equal(atomic_get(&slot_sides[1]), DEFY_WIRED_LEFT);
    zassert_equal(get_status().connections, ZMK_SPLIT_TRANSPORT_CONNECTIONS_STATUS_SOME_CONNECTED);
    zassert_equal(send_command(DEFY_WIRED_LEFT, command), 1);
    zassert_ok(defy_hybrid_central_peripheral_event(1, event));
    zassert_equal(other_event_count, 2);
    zassert_equal(other_event_side, DEFY_WIRED_LEFT);
}

ZTEST(defy_hybrid_central, test_ble_timeout_does_not_accept_contradictory_side) {
    ble(0, 1, 0);
    now = CONFIG_DEFY_HYBRID_BLE_TIMEOUT_MS;
    drain();
    ble(0, 1, 7);
    zassert_equal(event_count, 2);
    zassert_equal(ble_links[0].side, DEFY_WIRED_LEFT);
    zassert_false(ble_links[0].connected);
    ble(0, 1, -1);
    zassert_equal(atomic_get(&slot_sides[0]), DEFY_WIRED_SIDE_UNKNOWN);
    zassert_true(ble_links[0].rejected);
}

ZTEST(defy_hybrid_central, test_expired_identity_cannot_reclaim_live_replacement_side) {
    ble(0, 1, 0);
    now = CONFIG_DEFY_HYBRID_BLE_TIMEOUT_MS;
    drain();
    ble(1, 1, 1);
    zassert_equal(event_count, 3);
    assert_event(2, DEFY_WIRED_LEFT, 1, true);
    ble(0, 1, -1);
    zassert_equal(event_count, 3);
    zassert_equal(atomic_get(&slot_sides[0]), DEFY_WIRED_SIDE_UNKNOWN);
    zassert_equal(atomic_get(&slot_sides[1]), DEFY_WIRED_LEFT);
    ble_off(0, 1);
    zassert_equal(event_count, 3);
}

ZTEST(defy_hybrid_central, test_old_epoch_reports_cannot_replace_current_ble_identity) {
    ble(0, 2, 7);
    ble(0, 1, 0);
    ble(0, 1, -1);
    ble_off(0, 1);
    zassert_equal(event_count, 1);
    zassert_equal(ble_links[0].epoch, 2);
    zassert_equal(ble_links[0].side, DEFY_WIRED_RIGHT);
    zassert_equal(atomic_get(&slot_sides[0]), DEFY_WIRED_RIGHT);
    ble(0, 2, -1);
    assert_event(1, DEFY_WIRED_RIGHT, 7, false);
}

ZTEST(defy_hybrid_central, test_disconnect_forgets_expired_identity_before_empty_report) {
    ble(0, 1, 0);
    now = CONFIG_DEFY_HYBRID_BLE_TIMEOUT_MS;
    drain();
    ble_off(0, 1);
    ble(0, 1, -1);
    zassert_equal(event_count, 2);
    zassert_equal(ble_links[0].side, DEFY_WIRED_SIDE_UNKNOWN);
    zassert_equal(atomic_get(&slot_sides[0]), DEFY_WIRED_SIDE_UNKNOWN);
    ble(0, 1, 7);
    assert_event(2, DEFY_WIRED_RIGHT, 7, true);
}

ZTEST(defy_hybrid_central, test_recovered_empty_ble_report_completes_wired_handoff) {
    ble(0, 1, 0);
    cable(DEFY_WIRED_LEFT, 9, 0);
    now = CONFIG_DEFY_HYBRID_BLE_TIMEOUT_MS;
    drain();
    zassert_equal(event_count, 1);
    ble(0, 1, -1);
    zassert_equal(event_count, 1);
    cable_off(DEFY_WIRED_LEFT, 9);
    zassert_equal(event_count, 1);
    ble(0, 1, -1);
    zassert_equal(event_count, 2);
    assert_event(1, DEFY_WIRED_LEFT, 0, false);
    zassert_false(sides[DEFY_WIRED_LEFT].await_ble_snapshot);
}

ZTEST(defy_hybrid_central, test_ble_timeout_cannot_replay_the_cached_pressed_snapshot) {
    ble(0, 1, 0);
    struct hybrid_snapshot old = ble_latest[0];
    now = CONFIG_DEFY_HYBRID_BLE_TIMEOUT_MS;
    drain();
    apply_ble(&old);
    drain();
    zassert_equal(event_count, 2);
    zassert_false(ble_links[0].connected);
    ble(0, 1, -1);
    apply_ble(&old);
    zassert_equal(event_count, 2);
    zassert_true(ble_links[0].connected);
    zassert_equal(sides[DEFY_WIRED_LEFT].output[0], 0);
}

ZTEST(defy_hybrid_central, test_disable_and_disconnect_forget_timeout_identity) {
    ble(0, 1, 0);
    now = CONFIG_DEFY_HYBRID_BLE_TIMEOUT_MS;
    drain();
    zassert_ok(set_enabled(false));
    ble_off(0, 1);
    zassert_ok(set_enabled(true));
    ble(0, 2, -1);
    zassert_equal(event_count, 2);
    zassert_equal(atomic_get(&slot_sides[0]), DEFY_WIRED_SIDE_UNKNOWN);
    zassert_equal(ble_links[0].side, DEFY_WIRED_SIDE_UNKNOWN);
    ble(0, 2, 7);
    assert_event(2, DEFY_WIRED_RIGHT, 7, true);
}

ZTEST(defy_hybrid_central, test_reused_ble_slot_resets_side_before_empty_first_snapshot) {
    ble(0, 1, 0);
    ble(0, 2, -1);
    zassert_equal(event_count, 2);
    zassert_equal(atomic_get(&slot_sides[0]), DEFY_WIRED_SIDE_UNKNOWN);
    ble(0, 2, 7);
    assert_event(2, DEFY_WIRED_RIGHT, 7, true);
}

ZTEST(defy_hybrid_central, test_ble_disconnect_cannot_release_a_cabled_half) {
    ble(0, 1, 0);
    cable(DEFY_WIRED_LEFT, 9, 0);
    ble_off(0, 1);
    zassert_equal(event_count, 1);
    cable_off(DEFY_WIRED_LEFT, 9);
    assert_event(1, DEFY_WIRED_LEFT, 0, false);
}

static void fill_queue(void) {
    uint8_t empty[DEFY_WIRED_BITMAP_SIZE] = {0};
    for (size_t i = 0; i < CONFIG_DEFY_HYBRID_QUEUE_SIZE; i++) {
        zassert_ok(defy_hybrid_central_wired_update(DEFY_WIRED_RIGHT, 99, true, empty));
    }
}

ZTEST(defy_hybrid_central, test_wired_queue_full_returns_backpressure_without_accepting_state) {
    fill_queue();
    uint8_t frame[DEFY_HYBRID_BLE_FRAME_SIZE];
    set_position(frame, 0);
    zassert_equal(defy_hybrid_central_wired_update(DEFY_WIRED_LEFT, 9, true, frame), -ENOMSG);
    drain();
    zassert_equal(event_count, 0);
    cable(DEFY_WIRED_LEFT, 9, 0);
    assert_event(0, DEFY_WIRED_LEFT, 0, true);
}

ZTEST(defy_hybrid_central, test_ble_queue_overflow_repairs_release_from_latest_full_snapshot) {
    ble(0, 1, 0);
    fill_queue();
    uint8_t empty[DEFY_HYBRID_BLE_FRAME_SIZE] = {0};
    zassert_equal(defy_hybrid_central_ble_update(0, 1, true, empty, sizeof(empty)), -ENOMSG);
    drain();
    zassert_equal(event_count, 2);
    assert_event(1, DEFY_WIRED_LEFT, 0, false);
}

ZTEST(defy_hybrid_central, test_old_mailbox_snapshot_cannot_satisfy_cable_loss_freshness) {
    cable(DEFY_WIRED_LEFT, 9, 0);
    fill_queue();
    uint8_t frame[DEFY_HYBRID_BLE_FRAME_SIZE];
    set_position(frame, 0);
    zassert_equal(defy_hybrid_central_ble_update(0, 1, true, frame, sizeof(frame)), -ENOMSG);
    struct hybrid_snapshot discarded;
    zassert_ok(k_msgq_get(&hybrid_snapshots, &discarded, K_NO_WAIT));
    zassert_ok(defy_hybrid_central_wired_update(DEFY_WIRED_LEFT, 9, false, NULL));
    drain();
    zassert_equal(event_count, 2);
    assert_event(1, DEFY_WIRED_LEFT, 0, false);
    zassert_equal(input_side_count[DEFY_WIRED_LEFT], 1);
    ble(0, 1, 0);
    assert_event(2, DEFY_WIRED_LEFT, 0, true);
    zassert_equal(input_side_count[DEFY_WIRED_LEFT], 2);
}

ZTEST(defy_hybrid_central, test_malformed_frames_do_not_extend_ble_liveness) {
    ble(0, 1, 0);
    uint8_t frame[DEFY_HYBRID_BLE_FRAME_SIZE];
    set_position(frame, 0);
    frame[0] |= BIT(7);
    now = 900;
    zassert_equal(defy_hybrid_central_ble_update(0, 1, true, frame, sizeof(frame)), -EPROTO);
    set_position(frame, 0);
    zassert_equal(defy_hybrid_central_ble_update(0, 1, true, frame, 8), -EINVAL);
    frame[15] = 1;
    zassert_equal(defy_hybrid_central_ble_update(0, 1, true, frame, sizeof(frame)), -EPROTO);
    now = 1000;
    drain();
    assert_event(1, DEFY_WIRED_LEFT, 0, false);
}

ZTEST(defy_hybrid_central, test_duplicate_side_claim_requires_new_epoch_after_recovery) {
    ble(0, 1, 0);
    ble(1, 1, 1);
    zassert_equal(event_count, 1);
    ble_off(0, 1);
    ble(1, 1, 1);
    zassert_equal(event_count, 2);
    ble(1, 2, 1);
    zassert_equal(event_count, 3);
    assert_event(2, DEFY_WIRED_LEFT, 1, true);
}

ZTEST(defy_hybrid_central, test_downstream_errors_and_raw_transport_events_are_not_replayed) {
    downstream_error = -EIO;
    ble(0, 1, 0);
    ble(0, 1, 0);
    zassert_equal(event_count, 1);
    struct zmk_split_transport_peripheral_event event = {
        .type = ZMK_SPLIT_TRANSPORT_PERIPHERAL_EVENT_TYPE_KEY_POSITION_EVENT,
        .data.key_position_event = {.position = 0, .pressed = true},
    };
    zassert_ok(defy_hybrid_central_peripheral_event(0, event));
    zassert_equal(event_count, 1);
}

static uint8_t original_notify(struct bt_conn *conn, struct bt_gatt_subscribe_params *params,
                               const void *data, uint16_t length) {
    ARG_UNUSED(conn);
    ARG_UNUSED(params);
    ARG_UNUSED(data);
    ARG_UNUSED(length);
    original_notify_calls++;
    return BT_GATT_ITER_CONTINUE;
}

static void original_subscribe(struct bt_conn *conn, uint8_t err,
                                struct bt_gatt_subscribe_params *params) {
    original_subscribe_calls++;
    original_subscribe_error = err;
    void (*hook)(struct bt_conn *, struct bt_gatt_subscribe_params *) = completion_hook;
    completion_hook = NULL;
    if (hook) {
        hook(conn, params);
    }
}

static uint8_t original_discover(struct bt_conn *conn, const struct bt_gatt_attr *attr,
                                 struct bt_gatt_discover_params *params) {
    ARG_UNUSED(conn);
    ARG_UNUSED(attr);
    ARG_UNUSED(params);
    original_discover_calls++;
    return BT_GATT_ITER_CONTINUE;
}

static void discover_position(unsigned int index, struct bt_gatt_discover_params *params,
                              uint16_t handle) {
    *params = (struct bt_gatt_discover_params){
        .type = BT_GATT_DISCOVER_CHARACTERISTIC,
        .func = original_discover,
    };
    zassert_ok(defy_hybrid_ble_discover(connection(index), params));
    struct bt_gatt_chrc characteristic = {
        .uuid = BT_UUID_DECLARE_128(ZMK_SPLIT_BT_CHAR_POSITION_STATE_UUID),
        .value_handle = handle,
    };
    struct bt_gatt_attr attribute = {.user_data = &characteristic};
    zassert_equal(params->func(connection(index), &attribute, params), BT_GATT_ITER_CONTINUE);
}

static void subscribe_position(unsigned int index, struct bt_gatt_discover_params *params,
                               struct bt_gatt_subscribe_params *subscription, uint16_t handle) {
    discover_position(index, params, handle);
    *subscription = (struct bt_gatt_subscribe_params){
        .notify = original_notify,
        .value_handle = handle,
    };
    zassert_ok(defy_hybrid_ble_subscribe(connection(index), subscription));
}

static void notify_position(unsigned int index, struct bt_gatt_subscribe_params *subscription,
                            int position) {
    uint8_t frame[DEFY_HYBRID_BLE_FRAME_SIZE];
    set_position(frame, position);
    zassert_equal(subscription->notify(connection(index), subscription, frame, sizeof(frame)),
                  BT_GATT_ITER_CONTINUE);
}

ZTEST(defy_hybrid_central, test_gatt_wrappers_leave_unrelated_connections_and_subscriptions_alone) {
    struct bt_gatt_discover_params params = {.func = original_discover};
    zassert_ok(defy_hybrid_ble_discover(connection(2), &params));
    zassert_equal(params.func, original_discover);
    struct bt_gatt_subscribe_params subscription = {.notify = original_notify, .value_handle = 9};
    zassert_ok(defy_hybrid_ble_subscribe(connection(2), &subscription));
    zassert_equal(subscription.notify, original_notify);
    discover_position(0, &params, 10);
    zassert_ok(defy_hybrid_ble_subscribe(connection(0), &subscription));
    zassert_equal(subscription.notify, original_notify);
}

ZTEST(defy_hybrid_central, test_gatt_position_snapshot_interception_bypasses_lossy_delta_callback) {
    struct bt_gatt_discover_params params;
    discover_position(0, &params, 10);
    struct bt_gatt_subscribe_params subscription = {.notify = original_notify, .value_handle = 10};
    zassert_ok(defy_hybrid_ble_subscribe(connection(0), &subscription));
    zassert_equal(subscription.notify, position_notification);
    uint8_t frame[DEFY_HYBRID_BLE_FRAME_SIZE];
    set_position(frame, 7);
    zassert_equal(subscription.notify(connection(0), &subscription, frame, sizeof(frame)),
                  BT_GATT_ITER_CONTINUE);
    drain();
    zassert_equal(original_notify_calls, 0);
    assert_event(0, DEFY_WIRED_RIGHT, 7, true);
    zassert_equal(subscription.notify(connection(0), &subscription, NULL, 0), BT_GATT_ITER_STOP);
    drain();
    zassert_equal(subscription.value_handle, 0);
    assert_event(1, DEFY_WIRED_RIGHT, 7, false);
}

ZTEST(defy_hybrid_central, test_disconnect_clears_state_even_after_upstream_forgets_slot) {
    struct bt_gatt_discover_params params;
    discover_position(0, &params, 10);
    struct bt_gatt_subscribe_params subscription = {.notify = original_notify, .value_handle = 10};
    zassert_ok(defy_hybrid_ble_subscribe(connection(0), &subscription));
    uint8_t frame[DEFY_HYBRID_BLE_FRAME_SIZE];
    set_position(frame, 0);
    subscription.notify(connection(0), &subscription, frame, sizeof(frame));
    drain();
    connection_slots[0] = -1;
    disconnected(connection(0), 0);
    drain();
    assert_event(1, DEFY_WIRED_LEFT, 0, false);
    zassert_equal(connection_refs[0], 0);
    zassert_is_null(peers[0].conn);
}

ZTEST(defy_hybrid_central, test_existing_subscription_keeps_the_snapshot_callback) {
    struct bt_gatt_discover_params params;
    discover_position(0, &params, 10);
    struct bt_gatt_subscribe_params subscription = {.notify = original_notify, .value_handle = 10};
    zassert_ok(defy_hybrid_ble_subscribe(connection(0), &subscription));
    subscribe_result = -EALREADY;
    zassert_equal(defy_hybrid_ble_subscribe(connection(0), &subscription), -EALREADY);
    zassert_equal(subscription.notify, position_notification);
    zassert_equal(peers[0].subscription, &subscription);
}

ZTEST(defy_hybrid_central, test_discovery_failure_restores_the_original_callback) {
    struct bt_gatt_discover_params params = {.func = original_discover};
    discover_result = -ENOMEM;
    zassert_equal(defy_hybrid_ble_discover(connection(0), &params), -ENOMEM);
    zassert_equal(params.func, original_discover);
    discover_result = 0;
    zassert_ok(defy_hybrid_ble_discover(connection(0), &params));
    zassert_equal(params.func, observe_discovery);
}

ZTEST(defy_hybrid_central, test_reconnect_reuses_discovery_params_with_new_connection_epoch) {
    struct bt_gatt_discover_params params;
    discover_position(0, &params, 10);
    uint32_t previous_epoch = peers[0].epoch;
    ble(0, previous_epoch, 0);
    disconnected(connection(0), 0);
    drain();
    connection_slots[0] = -1;
    connection_slots[1] = 0;
    discover_position(1, &params, 11);
    zassert_true(peers[0].epoch != previous_epoch);
    zassert_equal(peers[0].conn, connection(1));
    zassert_equal(connection_refs[0], 0);
    zassert_equal(connection_refs[1], 1);
    ble(0, peers[0].epoch, -1);
    ble(0, peers[0].epoch, 7);
    zassert_equal(event_count, 3);
    assert_event(2, DEFY_WIRED_RIGHT, 7, true);
}

static uint8_t original_service_discovery(struct bt_conn *conn, const struct bt_gatt_attr *attr,
                                         struct bt_gatt_discover_params *params) {
    ARG_UNUSED(attr);
    params->type = BT_GATT_DISCOVER_CHARACTERISTIC;
    params->func = original_discover;
    zassert_ok(defy_hybrid_ble_discover(conn, params));
    return BT_GATT_ITER_STOP;
}

ZTEST(defy_hybrid_central, test_nested_service_to_characteristic_discovery_reuses_params_safely) {
    struct bt_gatt_discover_params params = {
        .type = BT_GATT_DISCOVER_PRIMARY,
        .func = original_service_discovery,
    };
    zassert_ok(defy_hybrid_ble_discover(connection(0), &params));
    struct bt_gatt_attr service = {0};
    zassert_equal(params.func(connection(0), &service, &params), BT_GATT_ITER_STOP);
    struct bt_gatt_chrc characteristic = {
        .uuid = BT_UUID_DECLARE_128(ZMK_SPLIT_BT_CHAR_POSITION_STATE_UUID),
        .value_handle = 10,
    };
    struct bt_gatt_attr attribute = {.user_data = &characteristic};
    zassert_equal(params.func(connection(0), &attribute, &params), BT_GATT_ITER_CONTINUE);
    zassert_equal(original_discover_calls, 1);
    zassert_equal(peers[0].position_handle, 10);
    zassert_equal(discover_calls, 2);
}

ZTEST(defy_hybrid_central, test_initial_discovery_failure_recovers_without_reusing_params) {
    struct bt_gatt_discover_params params = {.func = original_discover};
    discover_result = -ENOMEM;
    zassert_equal(defy_hybrid_ble_discover(connection(0), &params), -ENOMEM);
    zassert_equal(params.func, original_discover);
    now = 8000;
    zassert_equal(defy_hybrid_ble_discover(connection(0), &params), -ENOMEM);
    now = 9999;
    service();
    zassert_equal(disconnect_calls[0], 0);
    now = 10000;
    service();
    zassert_equal(disconnect_calls[0], 1);
    zassert_equal(disconnect_refs[0], 2);
    zassert_equal(connection_refs[0], 1);
    zassert_equal(discover_calls, 2);
    zassert_equal(subscribe_calls, 0);
}

ZTEST(defy_hybrid_central, test_successful_subscription_without_reports_recovers) {
    struct bt_gatt_discover_params params;
    struct bt_gatt_subscribe_params subscription;
    subscribe_position(0, &params, &subscription, 10);
    now = 10000;
    service();
    zassert_equal(disconnect_calls[0], 1);
    zassert_equal(subscription.value_handle, 10);
    zassert_equal(subscribe_calls, 1);
}

ZTEST(defy_hybrid_central, test_removed_subscription_does_not_extend_last_report_lease) {
    struct bt_gatt_discover_params params;
    struct bt_gatt_subscribe_params subscription;
    subscribe_position(0, &params, &subscription, 10);
    now = 1000;
    notify_position(0, &subscription, 0);
    drain();
    now = 9000;
    zassert_equal(subscription.notify(connection(0), &subscription, NULL, 0), BT_GATT_ITER_STOP);
    drain();
    assert_event(1, DEFY_WIRED_LEFT, 0, false);
    now = 10999;
    service();
    zassert_equal(disconnect_calls[0], 0);
    now = 11000;
    service();
    zassert_equal(disconnect_calls[0], 1);
    zassert_equal(subscription.value_handle, 0);
    zassert_equal(subscribe_calls, 1);
}

ZTEST(defy_hybrid_central, test_healthy_empty_reports_keep_idle_connection_without_recovery_work) {
    struct bt_gatt_discover_params params;
    struct bt_gatt_subscribe_params subscription;
    subscribe_position(0, &params, &subscription, 10);
    for (int i = 0; i < 8; i++) {
        now = i * 5000;
        notify_position(0, &subscription, -1);
        service();
        now += 4999;
        service();
    }
    zassert_equal(disconnect_calls[0], 0);
    zassert_equal(split_work_submissions, 0);
    zassert_equal(event_count, 0);
    zassert_equal(atomic_get(&slot_sides[0]), DEFY_WIRED_SIDE_UNKNOWN);
    zassert_equal(connection_refs[0], 1);
}

ZTEST(defy_hybrid_central, test_malformed_notifications_do_not_extend_recovery_lease) {
    struct bt_gatt_discover_params params;
    struct bt_gatt_subscribe_params subscription;
    subscribe_position(0, &params, &subscription, 10);
    notify_position(0, &subscription, -1);
    drain();
    uint8_t frame[DEFY_HYBRID_BLE_FRAME_SIZE];
    set_position(frame, 0);
    now = 9000;
    zassert_equal(subscription.notify(connection(0), &subscription, frame, 8),
                  BT_GATT_ITER_CONTINUE);
    frame[0] |= BIT(7);
    zassert_equal(subscription.notify(connection(0), &subscription, frame, sizeof(frame)),
                  BT_GATT_ITER_CONTINUE);
    set_position(frame, -1);
    frame[15] = 1;
    zassert_equal(subscription.notify(connection(0), &subscription, frame, sizeof(frame)),
                  BT_GATT_ITER_CONTINUE);
    now = 10000;
    service();
    zassert_equal(disconnect_calls[0], 1);
    zassert_equal(event_count, 0);
}

ZTEST(defy_hybrid_central, test_stalled_peer_recovery_leaves_healthy_peer_and_host_alone) {
    struct bt_gatt_discover_params params[3];
    struct bt_gatt_subscribe_params subscriptions[2];
    subscribe_position(0, &params[0], &subscriptions[0], 10);
    subscribe_position(1, &params[1], &subscriptions[1], 11);
    params[2] = (struct bt_gatt_discover_params){.func = original_discover};
    zassert_ok(defy_hybrid_ble_discover(connection(2), &params[2]));
    notify_position(0, &subscriptions[0], 0);
    notify_position(1, &subscriptions[1], 7);
    drain();
    now = 9999;
    notify_position(1, &subscriptions[1], 7);
    service();
    now = 10000;
    service();
    zassert_equal(disconnect_calls[0], 1);
    zassert_equal(disconnect_calls[1], 0);
    zassert_equal(disconnect_calls[2], 0);
    zassert_equal(connection_refs[1], 1);
    zassert_equal(connection_refs[2], 0);
    zassert_equal(sides[DEFY_WIRED_RIGHT].output[0], BIT(7));
    zassert_equal(event_count, 3);
    assert_event(2, DEFY_WIRED_LEFT, 0, false);
}

ZTEST(defy_hybrid_central, test_disconnect_and_slot_reuse_restart_recovery_lease) {
    struct bt_gatt_discover_params params;
    struct bt_gatt_subscribe_params old_subscription;
    struct bt_gatt_subscribe_params new_subscription;
    subscribe_position(0, &params, &old_subscription, 10);
    now = 9000;
    disconnected(connection(0), 0);
    connection_slots[0] = -1;
    connection_slots[1] = 0;
    subscribe_position(1, &params, &new_subscription, 11);
    now = 10000;
    service();
    zassert_equal(disconnect_calls[0], 0);
    zassert_equal(disconnect_calls[1], 0);
    now = 18000;
    uint8_t empty[DEFY_HYBRID_BLE_FRAME_SIZE] = {0};
    zassert_equal(old_subscription.notify(connection(0), &old_subscription, empty, sizeof(empty)),
                  BT_GATT_ITER_STOP);
    now = 19000;
    service();
    zassert_equal(disconnect_calls[0], 0);
    zassert_equal(disconnect_calls[1], 1);
    zassert_equal(connection_refs[0], 0);
    zassert_equal(connection_refs[1], 1);
}

ZTEST(defy_hybrid_central, test_queue_overflow_latest_snapshot_renews_recovery_lease) {
    struct bt_gatt_discover_params params;
    struct bt_gatt_subscribe_params subscription;
    subscribe_position(0, &params, &subscription, 10);
    notify_position(0, &subscription, 0);
    drain();
    now = 9000;
    fill_queue();
    notify_position(0, &subscription, -1);
    zassert_equal(k_msgq_num_used_get(&hybrid_snapshots), CONFIG_DEFY_HYBRID_QUEUE_SIZE);
    zassert_equal(ble_latest[0].received, now);
    uint8_t diagnostic[DEFY_HYBRID_BLE_DIAGNOSTICS_SIZE];
    zassert_equal(defy_hybrid_ble_diagnostics(diagnostic, sizeof(diagnostic)), sizeof(diagnostic));
    zassert_equal(sys_get_le32(diagnostic + 36), 2);
    zassert_equal(sys_get_le32(diagnostic + 48), 0);
    now = 10000;
    service();
    zassert_equal(disconnect_calls[0], 0);
    assert_event(1, DEFY_WIRED_LEFT, 0, false);
    now = 19000;
    service();
    zassert_equal(disconnect_calls[0], 1);
}

ZTEST(defy_hybrid_central, test_valid_report_before_recovery_work_cancels_stale_request) {
    struct bt_gatt_discover_params params;
    struct bt_gatt_subscribe_params subscription;
    subscribe_position(0, &params, &subscription, 10);
    now = 10000;
    poll_service();
    zassert_not_null(queued_split_work);
    zassert_equal(disconnect_calls[0], 0);
    now++;
    notify_position(0, &subscription, -1);
    run_split_work();
    zassert_equal(disconnect_calls[0], 0);
    now = 20001;
    service();
    zassert_equal(disconnect_calls[0], 1);
}

ZTEST(defy_hybrid_central, test_queued_recovery_cannot_disconnect_a_reused_slot) {
    struct bt_gatt_discover_params params;
    struct bt_gatt_subscribe_params subscriptions[2];
    subscribe_position(0, &params, &subscriptions[0], 10);
    now = 10000;
    poll_service();
    zassert_not_null(queued_split_work);
    disconnected(connection(0), 0);
    connection_slots[0] = -1;
    connection_slots[1] = 0;
    subscribe_position(1, &params, &subscriptions[1], 11);
    run_split_work();
    zassert_equal(disconnect_calls[0], 0);
    zassert_equal(disconnect_calls[1], 0);
    zassert_equal(connection_refs[0], 0);
    zassert_equal(connection_refs[1], 1);
    now = 20000;
    service();
    zassert_equal(disconnect_calls[1], 1);
}

static void read_diagnostics(uint8_t out[DEFY_HYBRID_BLE_DIAGNOSTICS_SIZE]) {
    zassert_equal(defy_hybrid_ble_diagnostics(out, DEFY_HYBRID_BLE_DIAGNOSTICS_SIZE),
                  DEFY_HYBRID_BLE_DIAGNOSTICS_SIZE);
    zassert_equal(out[0], 1);
    zassert_equal(out[1], 2);
    zassert_equal(sys_get_le16(out + 2), 48);
    zassert_equal(sys_get_le32(out + 4), (uint32_t)now);
}

ZTEST(defy_hybrid_central, test_failed_disconnects_are_bounded_and_valid_report_rearms_recovery) {
    uint8_t out[DEFY_HYBRID_BLE_DIAGNOSTICS_SIZE];
    struct bt_gatt_discover_params params;
    struct bt_gatt_subscribe_params subscription;
    subscribe_position(0, &params, &subscription, 10);
    disconnect_results[0] = -EAGAIN;
    now = 10000;
    service();
    zassert_equal(disconnect_calls[0], 1);
    now = 10999;
    service();
    zassert_equal(disconnect_calls[0], 1);
    now = 11000;
    service();
    zassert_equal(disconnect_calls[0], 2);
    now = 11999;
    service();
    zassert_equal(disconnect_calls[0], 2);
    now = 12000;
    service();
    zassert_equal(disconnect_calls[0], 3);
    now = 30000;
    service();
    zassert_equal(disconnect_calls[0], 3);
    zassert_equal(split_work_submissions, 3);
    zassert_equal(connection_refs[0], 1);
    zassert_equal(disconnect_times[0][0], 10000);
    zassert_equal(disconnect_times[0][1], 11000);
    zassert_equal(disconnect_times[0][2], 12000);
    read_diagnostics(out);
    zassert_equal(sys_get_le32(out + 44), 3);
    zassert_equal(out[9], DEFY_HYBRID_BLE_RECOVERY);
    notify_position(0, &subscription, -1);
    now = 39999;
    service();
    zassert_equal(disconnect_calls[0], 3);
    now = 40000;
    service();
    zassert_equal(disconnect_calls[0], 4);
}

/* Slot 1's link is already going down, so its -ENOTCONN counts as accepted. */
ZTEST(defy_hybrid_central, test_accepted_disconnect_is_not_reissued_before_callback) {
    struct bt_gatt_discover_params params[2];
    struct bt_gatt_subscribe_params subscriptions[2];
    subscribe_position(0, &params[0], &subscriptions[0], 10);
    subscribe_position(1, &params[1], &subscriptions[1], 11);
    disconnect_results[1] = -ENOTCONN;
    now = 10000;
    service();
    zassert_equal(disconnect_calls[0], 1);
    zassert_equal(disconnect_calls[1], 1);
    now = 10001;
    notify_position(0, &subscriptions[0], -1);
    notify_position(1, &subscriptions[1], -1);
    now = 30000;
    service();
    zassert_equal(disconnect_calls[0], 1);
    zassert_equal(disconnect_calls[1], 1);
    zassert_equal(split_work_submissions, 1);
    disconnected(connection(0), 0);
    disconnected(connection(1), 0);
    service();
    zassert_equal(connection_refs[0], 0);
    zassert_equal(connection_refs[1], 0);
    zassert_equal(disconnect_calls[0], 1);
    zassert_equal(disconnect_calls[1], 1);
}

static void replace_peer_during_disconnect(struct bt_conn *conn) {
    static struct bt_gatt_discover_params params;
    static struct bt_gatt_subscribe_params subscription;
    zassert_equal(conn, connection(0));
    zassert_equal(connection_refs[0], 2);
    connection_slots[0] = -1;
    disconnected(conn, 0);
    zassert_equal(connection_refs[0], 1);
    connection_slots[2] = 0;
    now++;
    subscribe_position(2, &params, &subscription, 12);
    notify_position(2, &subscription, 0);
    zassert_equal(connection_refs[2], 1);
}

ZTEST(defy_hybrid_central, test_inflight_disconnect_return_cannot_mark_replacement_disconnecting) {
    struct bt_gatt_discover_params params;
    struct bt_gatt_subscribe_params subscription;
    subscribe_position(0, &params, &subscription, 10);
    disconnect_hook = replace_peer_during_disconnect;
    now = 10000;
    service();
    drain();
    zassert_equal(now, 10001);
    zassert_equal(disconnect_calls[0], 1);
    zassert_equal(disconnect_calls[2], 0);
    zassert_equal(connection_refs[0], 0);
    zassert_equal(connection_refs[2], 1);
    zassert_equal(event_count, 1);
    assert_event(0, DEFY_WIRED_LEFT, 0, true);
    now = 20000;
    service();
    zassert_equal(disconnect_calls[2], 0);
    now = 20001;
    service();
    zassert_equal(disconnect_calls[0], 1);
    zassert_equal(disconnect_calls[2], 1);
    zassert_equal(disconnect_refs[2], 2);
    zassert_equal(connection_refs[2], 1);
    disconnected(connection(2), 0);
    service();
    zassert_equal(connection_refs[2], 0);
}

static void stop_system_work(void) {
    struct k_work_sync sync;
    k_work_flush(&service_work, &sync);
    run_system_work = false;
    k_work_cancel_delayable_sync(&timeout_work, &sync);
    k_work_flush(&service_work, &sync);
    k_work_cancel_delayable_sync(&timeout_work, &sync);
}

ZTEST(defy_hybrid_central, test_maintenance_releases_both_sides_and_discards_cached_inputs) {
    cable(DEFY_WIRED_LEFT, 1, 0);
    ble(1, 2, 7);
    uint8_t frame[DEFY_HYBRID_BLE_FRAME_SIZE];
    set_position(frame, 21);
    zassert_ok(defy_hybrid_central_ble_update(1, 2, true, frame, sizeof(frame)));
    zassert_equal(event_count, 2);
    run_system_work = true;
    zassert_ok(defy_hybrid_central_set_maintenance(true));
    stop_system_work();
    zassert_true(maintenance_wire_paused);
    zassert_true(defy_hybrid_central_is_maintenance());
    zassert_equal(event_count, 4);
    assert_event(2, DEFY_WIRED_LEFT, 0, false);
    assert_event(3, DEFY_WIRED_RIGHT, 7, false);
    zassert_equal(atomic_get(&connected_sides), 0);
    zassert_equal(k_msgq_num_used_get(&hybrid_snapshots), 0);
    zassert_ok(defy_hybrid_central_ble_update(1, 2, true, frame, sizeof(frame)));
    zassert_ok(defy_hybrid_central_wired_update(DEFY_WIRED_RIGHT, 2, true, frame));
    zassert_equal(k_msgq_num_used_get(&hybrid_snapshots), 0);
    zassert_equal(ble_latest[1].revision, 0);
    zassert_equal(send_command(DEFY_WIRED_LEFT,
                              (struct zmk_split_transport_central_command){0}), -EBUSY);
    run_system_work = true;
    zassert_ok(defy_hybrid_central_set_maintenance(false));
    stop_system_work();
    k_work_cancel_delayable(&timeout_work);
    zassert_false(maintenance_wire_paused);
    zassert_false(defy_hybrid_central_is_maintenance());
    drain();
    zassert_equal(event_count, 4);
    ble(1, 2, -1);
    zassert_equal(atomic_get(&slot_sides[1]), DEFY_WIRED_SIDE_UNKNOWN);
    zassert_equal(event_count, 4);
    ble(1, 2, 21);
    zassert_equal(event_count, 5);
    assert_event(4, DEFY_WIRED_RIGHT, 21, true);
}

ZTEST(defy_hybrid_central, test_maintenance_preserves_disabled_state_and_enabled_changes) {
    zassert_ok(set_enabled(false));
    run_system_work = true;
    zassert_ok(defy_hybrid_central_set_maintenance(true));
    zassert_ok(defy_hybrid_central_set_maintenance(false));
    stop_system_work();
    k_work_cancel_delayable(&timeout_work);
    zassert_false(get_status().enabled);
    zassert_equal(raw_starts, 0);
    run_system_work = true;
    zassert_ok(defy_hybrid_central_set_maintenance(true));
    zassert_ok(set_enabled(true));
    zassert_ok(defy_hybrid_central_set_maintenance(false));
    stop_system_work();
    service();
    zassert_true(get_status().enabled);
    zassert_equal(raw_starts, 2);
    k_work_cancel_delayable(&timeout_work);
}

ZTEST(defy_hybrid_central, test_maintenance_timeout_stays_closed_and_can_be_retried) {
    cable(DEFY_WIRED_LEFT, 1, 0);
    zassert_equal(defy_hybrid_central_set_maintenance(true), -EAGAIN);
    zassert_true(defy_hybrid_central_is_maintenance());
    zassert_true(maintenance_wire_paused);
    run_system_work = true;
    zassert_ok(defy_hybrid_central_set_maintenance(true));
    zassert_equal(event_count, 2);
    zassert_ok(defy_hybrid_central_set_maintenance(false));
    stop_system_work();
    k_work_cancel_delayable(&timeout_work);
    zassert_false(defy_hybrid_central_is_maintenance());
}

ZTEST(defy_hybrid_central, test_maintenance_wire_failure_still_releases_and_stays_closed) {
    cable(DEFY_WIRED_LEFT, 1, 0);
    maintenance_wire_error = -ETIMEDOUT;
    run_system_work = true;
    zassert_equal(defy_hybrid_central_set_maintenance(true), -ETIMEDOUT);
    zassert_equal(event_count, 2);
    zassert_true(defy_hybrid_central_is_maintenance());
    maintenance_wire_error = 0;
    zassert_ok(defy_hybrid_central_set_maintenance(false));
    stop_system_work();
    k_work_cancel_delayable(&timeout_work);
}

ZTEST(defy_hybrid_central, test_maintenance_cancels_queued_watchdog_and_renews_resume_lease) {
    struct bt_gatt_discover_params params;
    discover_position(0, &params, 10);
    now = 10000;
    poll_service();
    zassert_not_null(queued_split_work);
    run_system_work = true;
    zassert_ok(defy_hybrid_central_set_maintenance(true));
    stop_system_work();
    run_split_work();
    now = 50000;
    service();
    zassert_equal(disconnect_calls[0], 0);
    run_system_work = true;
    zassert_ok(defy_hybrid_central_set_maintenance(false));
    stop_system_work();
    k_work_cancel_delayable(&timeout_work);
    now = 59999;
    service();
    zassert_equal(disconnect_calls[0], 0);
    now = 60000;
    service();
    zassert_equal(disconnect_calls[0], 1);
}

static int system_maintenance_result;
static void try_maintenance_from_system_work(struct k_work *work) {
    ARG_UNUSED(work);
    system_maintenance_result = defy_hybrid_central_set_maintenance(true);
}
static K_WORK_DEFINE(try_maintenance_work, try_maintenance_from_system_work);

ZTEST(defy_hybrid_central, test_maintenance_rejects_system_queue_caller) {
    struct k_work_sync sync;
    system_maintenance_result = 0;
    zassert_true(submit_real_work(&try_maintenance_work) >= 0);
    k_work_flush(&try_maintenance_work, &sync);
    zassert_equal(system_maintenance_result, -EWOULDBLOCK);
    zassert_false(defy_hybrid_central_is_maintenance());
}

ZTEST(defy_hybrid_central, test_diagnostics_empty_snapshot_is_bounded_and_read_only) {
    uint8_t out[DEFY_HYBRID_BLE_DIAGNOSTICS_SIZE + 1];
    memset(out, 0xa5, sizeof(out));
    zassert_equal(defy_hybrid_ble_diagnostics(NULL, sizeof(out)), 0);
    zassert_equal(defy_hybrid_ble_diagnostics(out, sizeof(out) - 2), 0);
    for (size_t i = 0; i < sizeof(out); i++) {
        zassert_equal(out[i], 0xa5);
    }
    now = 1234;
    read_diagnostics(out);
    for (size_t i = 0; i < 2; i++) {
        const uint8_t *record = out + 8 + 48 * i;
        zassert_equal(record[0], i);
        for (size_t j = 1; j < 40; j++) {
            zassert_equal(record[j], 0);
        }
        zassert_equal(sys_get_le32(record + 40), UINT32_MAX);
        zassert_equal(sys_get_le32(record + 44), UINT32_MAX);
    }
    zassert_equal(out[sizeof(out) - 1], 0xa5);
    zassert_equal(discover_calls, 0);
    zassert_equal(subscribe_calls, 0);
    zassert_equal(split_work_submissions, 0);
    zassert_equal(connection_refs[0], 0);
    zassert_equal(connection_refs[1], 0);
}

ZTEST(defy_hybrid_central, test_diagnostics_connect_precedes_discovery_without_preparing_peer) {
    uint8_t out[DEFY_HYBRID_BLE_DIAGNOSTICS_SIZE];
    now = 50;
    connected(connection(0), 0);
    connected(connection(1), BT_HCI_ERR_CONN_FAIL_TO_ESTAB);
    connected(connection(2), 0);
    now = 75;
    read_diagnostics(out);
    zassert_equal(out[9], DEFY_HYBRID_BLE_CONNECTED);
    zassert_equal(out[10], BIT(0));
    zassert_equal(sys_get_le32(out + 12), 0);
    zassert_equal(sys_get_le32(out + 28), 1);
    zassert_equal(sys_get_le32(out + 52), 25);
    zassert_equal(out[57], DEFY_HYBRID_BLE_IDLE);
    zassert_equal(connection_refs[0], 0);
    zassert_is_null(peers[0].conn);
    struct bt_gatt_discover_params params;
    discover_position(0, &params, 10);
    connected(connection(0), 0);
    read_diagnostics(out);
    zassert_equal(sys_get_le32(out + 28), 1);
    zassert_equal(sys_get_le32(out + 12), peers[0].epoch);
    zassert_equal(out[10], BIT(0) | BIT(1));
}

ZTEST(defy_hybrid_central, test_diagnostics_split_discovery_errors_ignore_battery_discovery) {
    uint8_t out[DEFY_HYBRID_BLE_DIAGNOSTICS_SIZE];
    struct bt_gatt_discover_params params = {
        .uuid = BT_UUID_DECLARE_128(ZMK_SPLIT_BT_SERVICE_UUID),
        .type = BT_GATT_DISCOVER_PRIMARY,
        .func = original_service_discovery,
    };
    discover_result = -ENOMEM;
    zassert_equal(defy_hybrid_ble_discover(connection(0), &params), -ENOMEM);
    read_diagnostics(out);
    zassert_equal(out[9], DEFY_HYBRID_BLE_SETUP_FAILED);
    zassert_equal((int32_t)sys_get_le32(out + 16), -ENOMEM);
    discover_result = 0;
    zassert_ok(defy_hybrid_ble_discover(connection(0), &params));
    struct bt_gatt_attr service_attribute = {0};
    params.func(connection(0), &service_attribute, &params);
    read_diagnostics(out);
    zassert_equal(out[9], DEFY_HYBRID_BLE_CHARACTERISTIC_DISCOVERY);
    zassert_equal(out[25], BT_GATT_DISCOVER_CHARACTERISTIC);
    zassert_equal(sys_get_le32(out + 16), 0);
    struct bt_gatt_discover_params battery = {
        .uuid = BT_UUID_DECLARE_16(0xb000),
        .type = BT_GATT_DISCOVER_PRIMARY,
        .func = original_discover,
    };
    discover_result = -EIO;
    zassert_equal(defy_hybrid_ble_discover(connection(0), &battery), -EIO);
    read_diagnostics(out);
    zassert_equal(out[9], DEFY_HYBRID_BLE_CHARACTERISTIC_DISCOVERY);
    zassert_equal(sys_get_le32(out + 16), 0);
    params.func(connection(0), NULL, &params);
    read_diagnostics(out);
    zassert_equal(out[9], DEFY_HYBRID_BLE_SETUP_FAILED);
    zassert_equal(sys_get_le32(out + 16), 0);
}

ZTEST(defy_hybrid_central, test_diagnostics_subscribe_failure_preserves_original_callbacks) {
    uint8_t out[DEFY_HYBRID_BLE_DIAGNOSTICS_SIZE];
    struct bt_gatt_discover_params params;
    discover_position(0, &params, 10);
    struct bt_gatt_subscribe_params subscription = {
        .notify = original_notify,
        .subscribe = original_subscribe,
        .value_handle = 10,
        .ccc_handle = 11,
    };
    subscribe_result = -ENOMEM;
    zassert_equal(defy_hybrid_ble_subscribe(connection(0), &subscription), -ENOMEM);
    read_diagnostics(out);
    zassert_equal(out[9], DEFY_HYBRID_BLE_SETUP_FAILED);
    zassert_equal((int32_t)sys_get_le32(out + 20), -ENOMEM);
    zassert_equal(out[10], BIT(0) | BIT(1));
    zassert_equal(subscription.notify, original_notify);
    zassert_equal(subscription.subscribe, original_subscribe);
    zassert_equal(original_subscribe_calls, 0);
    zassert_equal(subscribe_calls, 1);
}

ZTEST(defy_hybrid_central, test_diagnostics_ccc_failure_after_null_notification_is_observed) {
    uint8_t out[DEFY_HYBRID_BLE_DIAGNOSTICS_SIZE];
    struct bt_gatt_discover_params params;
    discover_position(0, &params, 10);
    struct bt_gatt_subscribe_params subscription = {
        .notify = original_notify,
        .subscribe = original_subscribe,
        .value_handle = 10,
    };
    zassert_ok(defy_hybrid_ble_subscribe(connection(0), &subscription));
    read_diagnostics(out);
    zassert_equal(out[9], DEFY_HYBRID_BLE_CCC_DISCOVERY);
    subscription.ccc_handle = 11;
    zassert_equal(subscription.notify(connection(0), &subscription, NULL, 0), BT_GATT_ITER_STOP);
    read_diagnostics(out);
    zassert_equal(out[9], DEFY_HYBRID_BLE_SUBSCRIPTION_REMOVED);
    zassert_equal(sys_get_le32(out + 40), 1);
    subscription.subscribe(connection(0), BT_ATT_ERR_UNLIKELY, &subscription);
    read_diagnostics(out);
    zassert_equal(out[9], DEFY_HYBRID_BLE_SETUP_FAILED);
    zassert_equal(out[24], BT_ATT_ERR_UNLIKELY);
    zassert_equal(out[10], BIT(0));
    zassert_equal(original_subscribe_calls, 1);
    zassert_equal(original_subscribe_error, BT_ATT_ERR_UNLIKELY);
    zassert_equal(subscribe_calls, 1);
}

ZTEST(defy_hybrid_central, test_diagnostics_late_ccc_success_does_not_hide_valid_reporting) {
    uint8_t out[DEFY_HYBRID_BLE_DIAGNOSTICS_SIZE];
    struct bt_gatt_discover_params params;
    discover_position(0, &params, 10);
    struct bt_gatt_subscribe_params subscription = {
        .notify = original_notify,
        .subscribe = original_subscribe,
        .value_handle = 10,
        .ccc_handle = 11,
    };
    zassert_ok(defy_hybrid_ble_subscribe(connection(0), &subscription));
    read_diagnostics(out);
    zassert_equal(out[9], DEFY_HYBRID_BLE_SUBSCRIBING);
    subscription.subscribe(connection(0), 0, &subscription);
    read_diagnostics(out);
    zassert_equal(out[9], DEFY_HYBRID_BLE_SUBSCRIBED);
    now = 100;
    notify_position(0, &subscription, -1);
    subscription.subscribe(connection(0), 0, &subscription);
    now = 120;
    read_diagnostics(out);
    zassert_equal(out[9], DEFY_HYBRID_BLE_REPORTING);
    zassert_equal(out[10], 15);
    zassert_equal(sys_get_le32(out + 36), 1);
    zassert_equal(sys_get_le32(out + 48), 20);
    zassert_equal(original_subscribe_calls, 2);
    zassert_equal(subscribe_calls, 1);
}

ZTEST(defy_hybrid_central, test_diagnostics_reports_and_disconnect_history_are_per_peer) {
    uint8_t out[DEFY_HYBRID_BLE_DIAGNOSTICS_SIZE];
    struct bt_gatt_discover_params params[2];
    struct bt_gatt_subscribe_params subscription[2];
    subscribe_position(0, &params[0], &subscription[0], 10);
    now = 50;
    subscribe_position(1, &params[1], &subscription[1], 20);
    now = 100;
    notify_position(0, &subscription[0], -1);
    uint8_t invalid[DEFY_HYBRID_BLE_FRAME_SIZE] = {0};
    subscription[1].notify(connection(1), &subscription[1], invalid, sizeof(invalid) - 1);
    now = 200;
    notify_position(1, &subscription[1], -1);
    now = 250;
    connection_slots[0] = -1;
    disconnected(connection(0), BT_HCI_ERR_CONN_TIMEOUT);
    read_diagnostics(out);
    zassert_equal(out[9], DEFY_HYBRID_BLE_IDLE);
    zassert_equal(out[10], 0);
    zassert_equal(out[11], BT_HCI_ERR_CONN_TIMEOUT);
    zassert_equal(sys_get_le32(out + 28), 1);
    zassert_equal(sys_get_le32(out + 32), 1);
    zassert_equal(sys_get_le32(out + 36), 1);
    zassert_equal(sys_get_le32(out + 48), 150);
    zassert_equal(out[57], DEFY_HYBRID_BLE_REPORTING);
    zassert_equal(out[58], 15);
    zassert_equal(sys_get_le32(out + 84), 1);
    zassert_equal(sys_get_le32(out + 96), 50);
    zassert_equal(connection_refs[0], 0);
    zassert_equal(connection_refs[1], 1);
}

ZTEST(defy_hybrid_central, test_diagnostics_stale_callback_cannot_change_replacement_epoch) {
    uint8_t out[DEFY_HYBRID_BLE_DIAGNOSTICS_SIZE];
    struct bt_gatt_discover_params params[2];
    struct bt_gatt_subscribe_params subscription[2];
    subscribe_position(0, &params[0], &subscription[0], 10);
    bt_gatt_subscribe_func_t stale_callback = subscription[0].subscribe;
    uint32_t epoch = peers[0].epoch;
    disconnected(connection(0), BT_HCI_ERR_CONN_TIMEOUT);
    connection_slots[0] = -1;
    connection_slots[1] = 0;
    now = 100;
    subscribe_position(1, &params[1], &subscription[1], 11);
    subscription[1].subscribe(connection(1), 0, &subscription[1]);
    stale_callback(connection(0), BT_ATT_ERR_UNLIKELY, &subscription[0]);
    disconnected(connection(0), BT_HCI_ERR_REMOTE_USER_TERM_CONN);
    read_diagnostics(out);
    zassert_equal(out[9], DEFY_HYBRID_BLE_SUBSCRIBED);
    zassert_equal(out[24], 0);
    zassert_true(sys_get_le32(out + 12) != epoch);
    zassert_equal(sys_get_le32(out + 28), 2);
    zassert_equal(sys_get_le32(out + 32), 1);
    zassert_equal(out[11], BT_HCI_ERR_CONN_TIMEOUT);
    zassert_equal(out[10], BIT(0) | BIT(1) | BIT(2));
}

static void subscribe_encrypted_position(unsigned int index,
                                         struct bt_gatt_discover_params *discovery,
                                         struct bt_gatt_subscribe_params *subscription,
                                         uint16_t handle) {
    discover_position(index, discovery, handle);
    *subscription = (struct bt_gatt_subscribe_params){
        .notify = original_notify,
        .subscribe = original_subscribe,
        .value_handle = handle,
        .ccc_handle = handle + 1,
        .value = BT_GATT_CCC_NOTIFY,
    };
    zassert_ok(defy_hybrid_ble_subscribe(connection(index), subscription));
}

static void fail_encrypted_subscription(unsigned int index,
                                        struct bt_gatt_subscribe_params *subscription) {
    zassert_equal(subscription->notify(connection(index), subscription, NULL, 0),
                  BT_GATT_ITER_STOP);
    zassert_equal(subscription->value_handle, 0);
    zassert_is_null(peers[connection_slots[index]].subscription);
    subscription->subscribe(connection(index), BT_ATT_ERR_INSUFFICIENT_ENCRYPTION, subscription);
}

static void check_retry_after_original_completion(struct bt_conn *conn) {
    int index = connection_slots[connection_index(conn)];
    zassert_equal(original_subscribe_calls, 1);
    zassert_equal(original_subscribe_error, BT_ATT_ERR_INSUFFICIENT_ENCRYPTION);
    zassert_equal(peers[index].subscription->value_handle, 10);
    zassert_equal(peers[index].subscription->ccc_handle, 11);
    zassert_equal(peers[index].subscription->value, BT_GATT_CCC_NOTIFY);
    zassert_equal(connection_refs[connection_index(conn)], 2);
}

ZTEST(defy_hybrid_central, test_encrypted_subscription_retries_after_removal_when_already_secure) {
    struct bt_gatt_discover_params discovery;
    struct bt_gatt_subscribe_params subscription;
    uint8_t out[DEFY_HYBRID_BLE_DIAGNOSTICS_SIZE];
    subscribe_encrypted_position(0, &discovery, &subscription, 10);
    connection_security[0] = BT_SECURITY_L2;
    subscribe_hook = check_retry_after_original_completion;
    fail_encrypted_subscription(0, &subscription);
    zassert_equal(subscribe_calls, 2);
    zassert_equal(discover_calls, 1);
    zassert_equal(disconnect_calls[0], 0);
    zassert_equal(subscription.notify, position_notification);
    zassert_equal(connection_refs[0], 1);
    read_diagnostics(out);
    zassert_equal(out[9], DEFY_HYBRID_BLE_SUBSCRIBING);
    zassert_equal(out[24], BT_ATT_ERR_INSUFFICIENT_ENCRYPTION);
    subscription.subscribe(connection(0), 0, &subscription);
    notify_position(0, &subscription, 0);
    drain();
    read_diagnostics(out);
    zassert_equal(out[9], DEFY_HYBRID_BLE_REPORTING);
    zassert_equal(out[24], 0);
    zassert_equal(event_count, 1);
    zassert_equal(events[0].position, 0);
    zassert_true(events[0].pressed);
}

ZTEST(defy_hybrid_central, test_encrypted_subscription_waits_for_successful_security_change) {
    struct bt_gatt_discover_params discovery;
    struct bt_gatt_subscribe_params subscription;
    subscribe_encrypted_position(0, &discovery, &subscription, 10);
    fail_encrypted_subscription(0, &subscription);
    zassert_equal(subscribe_calls, 1);
    security_changed(connection(0), BT_SECURITY_L1, BT_SECURITY_ERR_SUCCESS);
    security_changed(connection(0), BT_SECURITY_L2, BT_SECURITY_ERR_AUTH_FAIL);
    security_changed(connection(0), BT_SECURITY_L2, BT_SECURITY_ERR_SUCCESS);
    zassert_equal(subscribe_calls, 1);
    connection_security[0] = BT_SECURITY_L2;
    security_changed(connection(0), BT_SECURITY_L2, BT_SECURITY_ERR_SUCCESS);
    zassert_equal(subscribe_calls, 2);
    security_changed(connection(0), BT_SECURITY_L2, BT_SECURITY_ERR_SUCCESS);
    subscription.subscribe(connection(0), BT_ATT_ERR_INSUFFICIENT_ENCRYPTION, &subscription);
    zassert_equal(subscribe_calls, 2);
    subscription.subscribe(connection(0), 0, &subscription);
    security_changed(connection(0), BT_SECURITY_L2, BT_SECURITY_ERR_SUCCESS);
    zassert_equal(subscribe_calls, 2);
    zassert_equal(connection_refs[0], 1);
}

ZTEST(defy_hybrid_central, test_encrypted_subscription_retry_budget_keeps_watchdog_fallback) {
    struct bt_gatt_discover_params discovery;
    struct bt_gatt_subscribe_params subscription;
    subscribe_encrypted_position(0, &discovery, &subscription, 10);
    connection_security[0] = BT_SECURITY_L2;
    for (int i = 0; i < 4; i++) {
        fail_encrypted_subscription(0, &subscription);
        zassert_equal(subscribe_calls, 1 + MIN(i + 1, 3));
        zassert_equal(connection_refs[0], 1);
    }
    subscription.subscribe(connection(0), BT_ATT_ERR_INSUFFICIENT_ENCRYPTION, &subscription);
    security_changed(connection(0), BT_SECURITY_L2, BT_SECURITY_ERR_SUCCESS);
    zassert_equal(subscribe_calls, 4);
    now = 9999;
    service();
    zassert_equal(disconnect_calls[0], 0);
    now = 10000;
    service();
    zassert_equal(disconnect_calls[0], 1);
}

ZTEST(defy_hybrid_central, test_encrypted_subscription_pending_retry_cannot_cross_connection_epoch) {
    struct bt_gatt_discover_params discovery[2];
    struct bt_gatt_subscribe_params subscription[2];
    subscribe_encrypted_position(0, &discovery[0], &subscription[0], 10);
    bt_gatt_subscribe_func_t stale_completion = subscription[0].subscribe;
    fail_encrypted_subscription(0, &subscription[0]);
    disconnected(connection(0), BT_HCI_ERR_CONN_TIMEOUT);
    connection_slots[0] = -1;
    connection_slots[1] = 0;
    subscribe_encrypted_position(1, &discovery[1], &subscription[1], 20);
    connection_security[0] = BT_SECURITY_L2;
    stale_completion(connection(0), BT_ATT_ERR_INSUFFICIENT_ENCRYPTION, &subscription[0]);
    security_changed(connection(0), BT_SECURITY_L2, BT_SECURITY_ERR_SUCCESS);
    zassert_equal(subscribe_calls, 2);
    zassert_equal(peers[0].subscription, &subscription[1]);
    zassert_equal(subscription[1].value_handle, 20);
    zassert_equal(connection_refs[0], 0);
    zassert_equal(connection_refs[1], 1);
}

ZTEST(defy_hybrid_central, test_encrypted_subscription_maintenance_cancels_pending_and_new_retries) {
    struct bt_gatt_discover_params discovery[2];
    struct bt_gatt_subscribe_params subscription[2];
    subscribe_encrypted_position(0, &discovery[0], &subscription[0], 10);
    subscribe_encrypted_position(1, &discovery[1], &subscription[1], 20);
    fail_encrypted_subscription(0, &subscription[0]);
    run_system_work = true;
    zassert_ok(defy_hybrid_central_set_maintenance(true));
    stop_system_work();
    connection_security[0] = BT_SECURITY_L2;
    connection_security[1] = BT_SECURITY_L2;
    security_changed(connection(0), BT_SECURITY_L2, BT_SECURITY_ERR_SUCCESS);
    fail_encrypted_subscription(1, &subscription[1]);
    zassert_equal(subscribe_calls, 2);
    run_system_work = true;
    zassert_ok(defy_hybrid_central_set_maintenance(false));
    stop_system_work();
    k_work_cancel_delayable(&timeout_work);
    security_changed(connection(0), BT_SECURITY_L2, BT_SECURITY_ERR_SUCCESS);
    security_changed(connection(1), BT_SECURITY_L2, BT_SECURITY_ERR_SUCCESS);
    zassert_equal(subscribe_calls, 2);
    zassert_equal(connection_refs[0], 1);
    zassert_equal(connection_refs[1], 1);
}

ZTEST(defy_hybrid_central, test_encrypted_subscription_sync_retry_error_stops_without_spinning) {
    struct bt_gatt_discover_params discovery;
    struct bt_gatt_subscribe_params subscription;
    uint8_t out[DEFY_HYBRID_BLE_DIAGNOSTICS_SIZE];
    subscribe_encrypted_position(0, &discovery, &subscription, 10);
    connection_security[0] = BT_SECURITY_L2;
    subscribe_result = -ENOMEM;
    fail_encrypted_subscription(0, &subscription);
    security_changed(connection(0), BT_SECURITY_L2, BT_SECURITY_ERR_SUCCESS);
    zassert_equal(subscribe_calls, 2);
    zassert_equal(connection_refs[0], 1);
    read_diagnostics(out);
    zassert_equal(out[9], DEFY_HYBRID_BLE_SETUP_FAILED);
    zassert_equal((int32_t)sys_get_le32(out + 20), -ENOMEM);
    zassert_equal(out[24], BT_ATT_ERR_INSUFFICIENT_ENCRYPTION);
    now = 10000;
    service();
    zassert_equal(disconnect_calls[0], 1);
}

static void remove_ccc_on_subscription_completion(
    struct bt_conn *conn, struct bt_gatt_subscribe_params *params) {
    ARG_UNUSED(conn);
    params->ccc_handle = 0;
}

static void unsubscribe_on_subscription_completion(
    struct bt_conn *conn, struct bt_gatt_subscribe_params *params) {
    ARG_UNUSED(conn);
    params->value = 0;
}

ZTEST(defy_hybrid_central, test_encrypted_subscription_does_not_retry_changed_ccc_request) {
    struct bt_gatt_discover_params discovery[2];
    struct bt_gatt_subscribe_params subscription[2];
    subscribe_encrypted_position(0, &discovery[0], &subscription[0], 10);
    subscribe_encrypted_position(1, &discovery[1], &subscription[1], 20);
    connection_security[0] = BT_SECURITY_L2;
    connection_security[1] = BT_SECURITY_L2;
    completion_hook = remove_ccc_on_subscription_completion;
    fail_encrypted_subscription(0, &subscription[0]);
    completion_hook = unsubscribe_on_subscription_completion;
    fail_encrypted_subscription(1, &subscription[1]);
    security_changed(connection(0), BT_SECURITY_L2, BT_SECURITY_ERR_SUCCESS);
    security_changed(connection(1), BT_SECURITY_L2, BT_SECURITY_ERR_SUCCESS);
    zassert_equal(subscribe_calls, 2);
    zassert_equal(discover_calls, 2);
    zassert_equal(subscription[0].value_handle, 0);
    zassert_equal(subscription[1].value_handle, 0);
}

ZTEST(defy_hybrid_central, test_other_att_errors_do_not_start_security_retry) {
    struct bt_gatt_discover_params discovery;
    struct bt_gatt_subscribe_params subscription;
    subscribe_encrypted_position(0, &discovery, &subscription, 10);
    connection_security[0] = BT_SECURITY_L2;
    zassert_equal(subscription.notify(connection(0), &subscription, NULL, 0), BT_GATT_ITER_STOP);
    subscription.subscribe(connection(0), BT_ATT_ERR_AUTHENTICATION, &subscription);
    security_changed(connection(0), BT_SECURITY_L2, BT_SECURITY_ERR_SUCCESS);
    zassert_equal(subscribe_calls, 1);
    zassert_equal(connection_refs[0], 1);
}

ZTEST(defy_hybrid_central, test_usb_input_uses_receipt_time_before_key_publication) {
    uint8_t frame[DEFY_HYBRID_BLE_FRAME_SIZE];
    now = 10;
    set_position(frame, 0);
    zassert_ok(defy_hybrid_central_wired_update(DEFY_WIRED_LEFT, 9, true, frame));
    now = 20;
    set_position(frame, 7);
    zassert_ok(defy_hybrid_central_ble_update(0, 1, true, frame, sizeof(frame)));
    now = 100;
    drain();
    zassert_equal(input_snapshot_count, 2);
    zassert_equal(input_snapshots[0].side, DEFY_WIRED_LEFT);
    zassert_equal(input_snapshots[0].received, 10);
    zassert_equal(input_snapshots[0].preceding_key_events, 0);
    zassert_equal(input_snapshots[1].side, DEFY_WIRED_RIGHT);
    zassert_equal(input_snapshots[1].received, 20);
    zassert_equal(input_snapshots[1].preceding_key_events, 1);
    zassert_equal(event_count, 2);
}

ZTEST(defy_hybrid_central, test_usb_input_excludes_background_route_and_handoff_release) {
    ble(0, 1, 0);
    cable(DEFY_WIRED_LEFT, 9, 0);
    zassert_equal(input_side_count[DEFY_WIRED_LEFT], 2);
    ble(0, 1, 1);
    zassert_equal(input_side_count[DEFY_WIRED_LEFT], 2);
    cable_off(DEFY_WIRED_LEFT, 9);
    now = CONFIG_DEFY_HYBRID_HANDOFF_GRACE_MS;
    drain();
    zassert_equal(input_side_count[DEFY_WIRED_LEFT], 2);
    zassert_equal(input_reset_count, 0);
    now++;
    ble(0, 1, 0);
    zassert_equal(input_side_count[DEFY_WIRED_LEFT], 3);
    zassert_equal(input_snapshots[2].bitmap[0], BIT(0));
    zassert_equal(input_snapshots[2].received, now);
}

ZTEST(defy_hybrid_central, test_usb_input_reconnect_retains_real_release_baseline) {
    ble(0, 1, 0);
    ble(0, 1, -1);
    zassert_equal(input_snapshot_count, 2);
    zassert_equal(input_snapshots[1].bitmap[0], 0);
    ble_off(0, 1);
    now = 50000;
    ble(1, 2, 0);
    zassert_equal(input_reset_count, 0);
    zassert_equal(input_snapshot_count, 3);
    zassert_equal(input_snapshots[2].side, DEFY_WIRED_LEFT);
    zassert_equal(input_snapshots[2].bitmap[0], BIT(0));
    zassert_equal(input_snapshots[2].received, 50000);
}

ZTEST(defy_hybrid_central, test_usb_input_timeout_and_session_reset_do_not_fabricate_release) {
    ble(0, 1, 0);
    now = CONFIG_DEFY_HYBRID_BLE_TIMEOUT_MS;
    drain();
    zassert_equal(event_count, 2);
    zassert_equal(input_snapshot_count, 1);
    ble(0, 2, 0);
    cable(DEFY_WIRED_LEFT, 9, 0);
    cable(DEFY_WIRED_LEFT, 10, 0);
    zassert_equal(input_snapshot_count, 4);
    for (size_t i = 0; i < input_snapshot_count; i++) {
        zassert_equal(input_snapshots[i].bitmap[0], BIT(0));
    }
    zassert_equal(input_reset_count, 0);
}

ZTEST(defy_hybrid_central, test_usb_input_maintenance_resets_before_cleanup_and_drops_queue) {
    cable(DEFY_WIRED_LEFT, 9, 0);
    uint8_t frame[DEFY_HYBRID_BLE_FRAME_SIZE];
    set_position(frame, 1);
    zassert_ok(defy_hybrid_central_wired_update(DEFY_WIRED_LEFT, 9, true, frame));
    run_system_work = true;
    zassert_ok(defy_hybrid_central_set_maintenance(true));
    stop_system_work();
    zassert_equal(input_reset_count, 1);
    zassert_equal(input_reset_preceding_key_events, 1);
    zassert_equal(event_count, 2);
    zassert_equal(input_snapshot_count, 1);
    cable(DEFY_WIRED_LEFT, 9, 1);
    zassert_equal(input_snapshot_count, 1);
    run_system_work = true;
    zassert_ok(defy_hybrid_central_set_maintenance(false));
    stop_system_work();
    k_work_cancel_delayable(&timeout_work);
    zassert_equal(input_reset_count, 2);
    cable(DEFY_WIRED_LEFT, 9, 1);
    zassert_equal(input_snapshot_count, 2);
    zassert_equal(input_snapshots[1].bitmap[0], BIT(1));
}

ZTEST(defy_hybrid_central, test_usb_input_disable_invalidates_and_stops_admission) {
    cable(DEFY_WIRED_LEFT, 9, 0);
    zassert_ok(set_enabled(false));
    zassert_equal(input_reset_count, 1);
    zassert_equal(input_reset_preceding_key_events, 1);
    drain();
    cable(DEFY_WIRED_LEFT, 9, 1);
    ble(1, 1, 7);
    zassert_equal(input_snapshot_count, 1);
    zassert_ok(set_enabled(true));
    cable(DEFY_WIRED_LEFT, 9, 1);
    zassert_equal(input_snapshot_count, 2);
}

ZTEST(defy_hybrid_central, test_usb_input_ignores_unknown_empty_and_conflicting_ble_side) {
    ble(0, 1, -1);
    zassert_equal(input_snapshot_count, 0);
    ble(0, 1, 0);
    ble(1, 1, 1);
    zassert_equal(input_snapshot_count, 1);
    ble(0, 1, -1);
    zassert_equal(input_snapshot_count, 2);
    zassert_equal(input_snapshots[1].bitmap[0], 0);
    zassert_equal(input_reset_count, 0);
}

ZTEST_SUITE(defy_hybrid_central, NULL, NULL, reset, NULL, NULL);

ZTEST(defy_hybrid_central, test_battery_identity_routes_empty_reports_without_keypress) {
    battery_epochs[1] = 4;
    battery_sides_test[1] = DEFY_WIRED_LEFT;
    ble(1, 4, -1);
    zassert_equal(atomic_get(&slot_sides[1]), DEFY_WIRED_LEFT);
    zassert_true(ble_links[1].connected);
    zassert_equal(event_count, 0);
    struct zmk_split_transport_central_command command = {
        .type = ZMK_SPLIT_TRANSPORT_CENTRAL_CMD_TYPE_SET_HID_INDICATORS,
    };
    zassert_equal(send_command(DEFY_WIRED_LEFT, command), 1);
}

ZTEST(defy_hybrid_central, test_later_battery_identity_reconsiders_fresh_empty_snapshot) {
    ble(0, 3, -1);
    zassert_false(ble_links[0].connected);
    now = 100;
    battery_epochs[0] = 3;
    battery_sides_test[0] = DEFY_WIRED_RIGHT;
    drain();
    zassert_true(ble_links[0].connected);
    zassert_equal(atomic_get(&slot_sides[0]), DEFY_WIRED_RIGHT);
    zassert_equal(event_count, 0);
}

ZTEST(defy_hybrid_central, test_battery_identity_does_not_revive_stale_empty_snapshot) {
    ble(0, 3, -1);
    now = CONFIG_DEFY_HYBRID_BLE_TIMEOUT_MS;
    battery_epochs[0] = 3;
    battery_sides_test[0] = DEFY_WIRED_RIGHT;
    drain();
    zassert_false(ble_links[0].connected);
    ble(0, 3, -1);
    zassert_true(ble_links[0].connected);
    zassert_equal(atomic_get(&slot_sides[0]), DEFY_WIRED_RIGHT);
}

ZTEST(defy_hybrid_central, test_reused_slot_requires_current_epoch_battery_identity) {
    battery_epochs[0] = 1;
    battery_sides_test[0] = DEFY_WIRED_LEFT;
    ble(0, 1, 0);
    ble(0, 2, -1);
    zassert_equal(event_count, 2);
    zassert_false(ble_links[0].connected);
    battery_epochs[0] = 2;
    battery_sides_test[0] = DEFY_WIRED_RIGHT;
    drain();
    zassert_equal(atomic_get(&slot_sides[0]), DEFY_WIRED_RIGHT);
    defy_hybrid_central_ble_reject(0, 1);
    drain();
    zassert_true(ble_links[0].connected);
    zassert_false(ble_links[0].rejected);
}

ZTEST(defy_hybrid_central, test_battery_identity_rejects_contradictory_first_key_frame) {
    battery_epochs[0] = 1;
    battery_sides_test[0] = DEFY_WIRED_RIGHT;
    ble(0, 1, 0);
    zassert_equal(event_count, 0);
    zassert_true(ble_links[0].rejected);
    ble(0, 1, 7);
    zassert_equal(event_count, 0);
    ble(0, 2, 7);
    zassert_equal(event_count, 1);
    zassert_false(ble_links[0].rejected);
}

ZTEST(defy_hybrid_central, test_late_conflict_releases_keys_and_cannot_refresh_recovery) {
    struct bt_gatt_discover_params params;
    struct bt_gatt_subscribe_params subscription;
    subscribe_position(0, &params, &subscription, 10);
    notify_position(0, &subscription, 0);
    drain();
    now = 1;
    notify_position(0, &subscription, 7);
    poll_service();
    zassert_true(peers[0].rejected);
    zassert_true(ble_links[0].rejected);
    zassert_equal(event_count, 2);
    zassert_false(events[1].pressed);
    disconnect_results[0] = -EAGAIN;
    run_split_work();
    zassert_equal(disconnect_calls[0], 1);
    for (unsigned int i = 1; i <= 5; i++) {
        now = 1 + i * BLE_HEALTH_RETRY_MS;
        notify_position(0, &subscription, 0);
        service();
    }
    zassert_equal(disconnect_calls[0], BLE_HEALTH_MAX_ATTEMPTS);
    zassert_equal(peers[0].last_report, 1);
    zassert_equal(event_count, 2);
    zassert_false(ble_links[0].connected);
}

/* Connects slot 0 as a half whose battery says right but whose key is
 * left, so every connection is rejected and disconnected. */
static void connect_conflicting_half(struct bt_gatt_discover_params *params,
                                     struct bt_gatt_subscribe_params *subscription) {
    unsigned int disconnects = disconnect_calls[0];
    subscribe_position(0, params, subscription, 10);
    battery_sides_test[0] = DEFY_WIRED_RIGHT;
    notify_position(0, subscription, 0);
    poll_service();
    zassert_true(peers[0].rejected);
    run_split_work();
    zassert_equal(disconnect_calls[0], disconnects + 1);
    disconnected(connection(0), 0);
}

ZTEST(defy_hybrid_central, test_persistent_identity_conflict_backs_off_reconnects) {
    static const int32_t holds[] = {1000, 2000, 4000, 8000, 16000, 32000, 60000, 60000};
    struct bt_gatt_discover_params params;
    struct bt_gatt_subscribe_params subscription;
    zassert_true(defy_hybrid_ble_hold_off(0) <= 0);
    for (size_t i = 0; i < ARRAY_SIZE(holds); i++) {
        connect_conflicting_half(&params, &subscription);
        zassert_equal(defy_hybrid_ble_hold_off(0), holds[i]);
        zassert_true(defy_hybrid_ble_hold_off(1) <= 0);
        now += holds[i];
        zassert_equal(defy_hybrid_ble_hold_off(0), 0);
        now += 500;
    }
    zassert_equal(event_count, 0);
}

ZTEST(defy_hybrid_central, test_identity_conflict_hold_counts_each_connection_once) {
    struct bt_gatt_discover_params params;
    struct bt_gatt_subscribe_params subscription;
    subscribe_position(0, &params, &subscription, 10);
    battery_sides_test[0] = DEFY_WIRED_RIGHT;
    notify_position(0, &subscription, 0);
    poll_service();
    zassert_equal(defy_hybrid_ble_hold_off(0), BLE_REJECT_HOLD_MS);
    now = 500;
    notify_position(0, &subscription, 0);
    poll_service();
    zassert_true(peers[0].rejected);
    zassert_equal(defy_hybrid_ble_hold_off(0), BLE_REJECT_HOLD_MS - 500);
}

ZTEST(defy_hybrid_central, test_identity_conflict_hold_resets_after_quiet_minute) {
    struct bt_gatt_discover_params params;
    struct bt_gatt_subscribe_params subscription;
    connect_conflicting_half(&params, &subscription);
    now += BLE_REJECT_HOLD_MS;
    connect_conflicting_half(&params, &subscription);
    zassert_equal(defy_hybrid_ble_hold_off(0), 2 * BLE_REJECT_HOLD_MS);
    now += 2 * BLE_REJECT_HOLD_MS + BLE_REJECT_HOLD_MAX_MS;
    connect_conflicting_half(&params, &subscription);
    zassert_equal(defy_hybrid_ble_hold_off(0), 4 * BLE_REJECT_HOLD_MS);
    now += 4 * BLE_REJECT_HOLD_MS + BLE_REJECT_HOLD_MAX_MS + 1;
    connect_conflicting_half(&params, &subscription);
    zassert_equal(defy_hybrid_ble_hold_off(0), BLE_REJECT_HOLD_MS);
}

ZTEST(defy_hybrid_central, test_cleared_hold_off_ends_both_holds_and_starts_doubling_again) {
    struct bt_gatt_discover_params params;
    struct bt_gatt_subscribe_params subscription;
    connect_conflicting_half(&params, &subscription);
    now += BLE_REJECT_HOLD_MS;
    connect_conflicting_half(&params, &subscription);
    zassert_equal(defy_hybrid_ble_hold_off(0), 2 * BLE_REJECT_HOLD_MS);
    defy_hybrid_ble_clear_hold_off();
    zassert_true(defy_hybrid_ble_hold_off(0) <= 0);
    zassert_true(defy_hybrid_ble_hold_off(1) <= 0);
    connect_conflicting_half(&params, &subscription);
    zassert_equal(defy_hybrid_ble_hold_off(0), BLE_REJECT_HOLD_MS);
}

ZTEST(defy_hybrid_central, test_queued_battery_rejection_survives_queue_overflow) {
    struct bt_gatt_discover_params params;
    struct bt_gatt_subscribe_params subscription;
    subscribe_position(0, &params, &subscription, 10);
    notify_position(0, &subscription, 0);
    drain();
    fill_queue();
    defy_hybrid_central_ble_reject(0, peers[0].epoch);
    notify_position(0, &subscription, 0);
    service();
    zassert_true(ble_links[0].rejected);
    zassert_true(peers[0].rejected);
    zassert_equal(disconnect_calls[0], 1);
    zassert_equal(event_count, 2);
}

ZTEST(defy_hybrid_central, test_new_peer_clears_quarantine_without_inheriting_identity) {
    struct bt_gatt_discover_params params;
    struct bt_gatt_subscribe_params subscriptions_test[2];
    subscribe_position(0, &params, &subscriptions_test[0], 10);
    notify_position(0, &subscriptions_test[0], 0);
    drain();
    notify_position(0, &subscriptions_test[0], 7);
    poll_service();
    disconnected(connection(0), 0);
    connection_slots[0] = -1;
    connection_slots[1] = 0;
    subscribe_position(1, &params, &subscriptions_test[1], 11);
    notify_position(1, &subscriptions_test[1], -1);
    service();
    zassert_false(peers[0].rejected);
    zassert_false(ble_links[0].rejected);
    zassert_false(ble_links[0].connected);
    zassert_equal(disconnect_calls[1], 0);
    battery_epochs[0] = peers[0].epoch;
    battery_sides_test[0] = DEFY_WIRED_RIGHT;
    service();
    zassert_true(ble_links[0].connected);
    zassert_equal(atomic_get(&slot_sides[0]), DEFY_WIRED_RIGHT);
}

ZTEST(defy_hybrid_central, test_battery_identity_empty_report_respects_wired_handoff_order) {
    cable(DEFY_WIRED_LEFT, 9, 0);
    ble(1, 1, -1);
    cable_off(DEFY_WIRED_LEFT, 9);
    battery_epochs[1] = 1;
    battery_sides_test[1] = DEFY_WIRED_LEFT;
    drain();
    zassert_true(sides[DEFY_WIRED_LEFT].await_ble_snapshot);
    zassert_equal(event_count, 2);
    ble(1, 1, -1);
    zassert_false(sides[DEFY_WIRED_LEFT].await_ble_snapshot);
    zassert_equal(event_count, 2);
}

ZTEST(defy_hybrid_central, test_battery_metadata_cannot_publish_stale_queued_keypress) {
    uint8_t frame[DEFY_HYBRID_BLE_FRAME_SIZE];
    set_position(frame, 0);
    zassert_ok(defy_hybrid_central_ble_update(0, 1, true, frame, sizeof(frame)));
    battery_epochs[0] = 1;
    battery_sides_test[0] = DEFY_WIRED_LEFT;
    now = CONFIG_DEFY_HYBRID_BLE_TIMEOUT_MS;
    drain();
    zassert_equal(event_count, 0);
    zassert_false(ble_links[0].connected);
    ble(0, 1, 0);
    zassert_equal(event_count, 1);
}

ZTEST(defy_hybrid_central, test_concurrent_battery_identification_conflict_rejects_keys) {
    battery_identify_error = -EPROTO;
    ble(0, 1, 0);
    zassert_equal(event_count, 0);
    zassert_false(ble_links[0].connected);
    zassert_true(ble_links[0].rejected);
}

static void assert_trace(size_t index, uint8_t kind, uint8_t flags, uint16_t value,
                         uint32_t extra) {
    zassert_true(index < trace_count);
    zassert_equal(traces[index].kind, kind);
    zassert_equal(traces[index].flags, flags);
    zassert_equal(traces[index].value, value);
    zassert_equal(traces[index].extra, extra);
}

#define LEFT_KEY 17
#define RIGHT_KEY 7
#define PRESSED DEFY_KEY_TRACE_ACTIVE

ZTEST(defy_hybrid_central, test_key_trace_links_queue_order_to_published_positions) {
    uint8_t frame[DEFY_HYBRID_BLE_FRAME_SIZE];
    now = 5;
    set_position(frame, RIGHT_KEY);
    zassert_ok(defy_hybrid_central_wired_update(DEFY_WIRED_RIGHT, 9, true, frame));
    now = 6;
    set_position(frame, LEFT_KEY);
    zassert_ok(defy_hybrid_central_ble_update(1, 3, true, frame, sizeof(frame)));
    zassert_equal(trace_count, 2);
    assert_trace(0, DEFY_KEY_TRACE_SNAPSHOT, DEFY_WIRED_RIGHT | PRESSED, 1, 5000);
    assert_trace(1, DEFY_KEY_TRACE_SNAPSHOT,
                 DEFY_WIRED_LEFT | DEFY_KEY_TRACE_BLE | DEFY_KEY_TRACE_SOURCE | PRESSED, 2, 6000);
    drain();
    zassert_equal(event_count, 2);
    assert_event(0, DEFY_WIRED_RIGHT, RIGHT_KEY, true);
    assert_event(1, DEFY_WIRED_LEFT, LEFT_KEY, true);
    zassert_equal(trace_count, 4);
    assert_trace(2, DEFY_KEY_TRACE_POSITION, DEFY_WIRED_RIGHT | PRESSED, RIGHT_KEY, 1);
    assert_trace(3, DEFY_KEY_TRACE_POSITION, DEFY_WIRED_LEFT | PRESSED, LEFT_KEY, 2);
    zassert_equal(traces[2].preceding_key_events, 0);
    zassert_equal(traces[3].preceding_key_events, 1);

    now = 7;
    set_position(frame, -1);
    zassert_ok(defy_hybrid_central_ble_update(1, 3, true, frame, sizeof(frame)));
    zassert_ok(defy_hybrid_central_wired_update(DEFY_WIRED_RIGHT, 9, true, frame));
    zassert_ok(defy_hybrid_central_wired_update(DEFY_WIRED_RIGHT, 9, false, NULL));
    drain();
    zassert_equal(trace_count, 9);
    assert_trace(4, DEFY_KEY_TRACE_SNAPSHOT,
                 DEFY_WIRED_SIDE_UNKNOWN | DEFY_KEY_TRACE_BLE | DEFY_KEY_TRACE_SOURCE | PRESSED,
                 3, 7000);
    assert_trace(5, DEFY_KEY_TRACE_SNAPSHOT, DEFY_WIRED_RIGHT | PRESSED, 4, 7000);
    assert_trace(6, DEFY_KEY_TRACE_SNAPSHOT, DEFY_WIRED_RIGHT, 5, 7000);
    assert_trace(7, DEFY_KEY_TRACE_POSITION, DEFY_WIRED_LEFT, LEFT_KEY, 3);
    assert_trace(8, DEFY_KEY_TRACE_POSITION, DEFY_WIRED_RIGHT, RIGHT_KEY, 4);
    zassert_equal(event_count, 4);
    assert_event(2, DEFY_WIRED_LEFT, LEFT_KEY, false);
    assert_event(3, DEFY_WIRED_RIGHT, RIGHT_KEY, false);
}

ZTEST(defy_hybrid_central, test_key_trace_marks_full_queue_and_unlinked_releases) {
    uint8_t frame[DEFY_HYBRID_BLE_FRAME_SIZE];
    for (int i = 0; i < CONFIG_DEFY_HYBRID_QUEUE_SIZE; i++) {
        set_position(frame, i % 2 ? -1 : LEFT_KEY);
        zassert_ok(defy_hybrid_central_wired_update(DEFY_WIRED_LEFT, 9, true, frame));
    }
    zassert_equal(defy_hybrid_central_wired_update(DEFY_WIRED_LEFT, 9, true, frame), -ENOMSG);
    zassert_equal(trace_count, CONFIG_DEFY_HYBRID_QUEUE_SIZE + 1);
    assert_trace(CONFIG_DEFY_HYBRID_QUEUE_SIZE - 1, DEFY_KEY_TRACE_SNAPSHOT,
                 DEFY_WIRED_LEFT | PRESSED, CONFIG_DEFY_HYBRID_QUEUE_SIZE, 0);
    assert_trace(CONFIG_DEFY_HYBRID_QUEUE_SIZE, DEFY_KEY_TRACE_SNAPSHOT,
                 DEFY_WIRED_LEFT | PRESSED | DEFY_KEY_TRACE_DROPPED,
                 CONFIG_DEFY_HYBRID_QUEUE_SIZE + 1, 0);
    k_msgq_purge(&hybrid_snapshots);
    memset(sides, 0, sizeof(sides));

    ble(0, 1, 0);
    cable(DEFY_WIRED_LEFT, 9, 0);
    cable_off(DEFY_WIRED_LEFT, 9);
    now = CONFIG_DEFY_HYBRID_HANDOFF_GRACE_MS;
    trace_count = 0;
    drain();
    zassert_equal(event_count, 2);
    assert_event(1, DEFY_WIRED_LEFT, 0, false);
    zassert_equal(trace_count, 1);
    assert_trace(0, DEFY_KEY_TRACE_POSITION, DEFY_WIRED_LEFT, 0, 0);
}

