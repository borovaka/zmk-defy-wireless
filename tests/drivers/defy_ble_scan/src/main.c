/* SPDX-License-Identifier: MIT */

#include <zephyr/kernel.h>
#include <zephyr/init.h>
#include <zephyr/ztest.h>
#include <zephyr/settings/settings.h>
#include <zephyr/bluetooth/bluetooth.h>
#include <zephyr/bluetooth/conn.h>
#include <zephyr/bluetooth/hci.h>
#include <zephyr/logging/log.h>

LOG_MODULE_REGISTER(scan_test, LOG_LEVEL_NONE);

#define ZMK_SPLIT_BLE_PERIPHERAL_COUNT 2

enum peripheral_slot_state {
    PERIPHERAL_SLOT_STATE_OPEN,
    PERIPHERAL_SLOT_STATE_CONNECTING,
    PERIPHERAL_SLOT_STATE_CONNECTED,
};

struct peripheral_slot {
    enum peripheral_slot_state state;
    struct bt_conn *conn;
};

static bool is_enabled;
static bool is_scanning;
static struct peripheral_slot peripherals[2];
static int scan_result, stop_result, create_result, reserve_result;
static int scan_calls, stop_calls, create_calls, release_calls;
static int process_calls, disconnect_calls, disconnect_result;
static enum bt_conn_state conn_state;
static uint8_t disconnect_reason;
static void (*start_hook)(void);
static void (*create_hook)(void);
static bool host_scanning, reported;
static int64_t hold_until;
static uint8_t conn_tokens[2];
static const bt_addr_le_t address = {0};
bool host_initialized;
static bool callbacks_registered_before_host, settings_loaded;
static struct bt_conn_cb conn_callbacks;
K_THREAD_STACK_DEFINE(split_central_split_run_q_stack, 1024);
struct k_work_q split_central_split_run_q;

static int finish_init(void) {
    settings_loaded = true;
    return 0;
}

int bt_conn_cb_register(struct bt_conn_cb *cb) {
    ARG_UNUSED(cb);
    callbacks_registered_before_host = !host_initialized;
    return 0;
}

static int start_scanning(void);
static bool split_central_eir_found(const bt_addr_le_t *addr);
static int split_central_bt_set_enabled(bool enabled);
static void split_central_connected(struct bt_conn *conn, uint8_t conn_err);

static int reserve_peripheral_slot(const bt_addr_le_t *addr) {
    ARG_UNUSED(addr);
    if (reserve_result < 0) {
        return reserve_result;
    }
    for (int i = 0; i < 2; i++) {
        if (peripherals[i].state == PERIPHERAL_SLOT_STATE_OPEN) {
            peripherals[i].state = PERIPHERAL_SLOT_STATE_CONNECTING;
            return i;
        }
    }
    return -ENOMEM;
}

static int release_peripheral_slot(int index) {
    peripherals[index].state = PERIPHERAL_SLOT_STATE_OPEN;
    peripherals[index].conn = NULL;
    release_calls++;
    return 0;
}

static int release_peripheral_slot_for_conn(struct bt_conn *conn) {
    for (int i = 0; i < 2; i++) {
        if (peripherals[i].conn == conn) {
            return release_peripheral_slot(i);
        }
    }
    return -ENOENT;
}

static int confirm_peripheral_slot_conn(struct bt_conn *conn) {
    for (int i = 0; i < 2; i++) {
        if (peripherals[i].conn == conn) {
            peripherals[i].state = PERIPHERAL_SLOT_STATE_CONNECTED;
            return 0;
        }
    }
    return -ENOENT;
}

static void split_central_process_connection(struct bt_conn *conn) {
    ARG_UNUSED(conn);
    process_calls++;
    start_scanning();
}

static void status_handler(struct k_work *work) { ARG_UNUSED(work); }
K_WORK_DEFINE(notify_status_work, status_handler);

int bt_le_scan_start(const struct bt_le_scan_param *param, bt_le_scan_cb_t cb) {
    ARG_UNUSED(param);
    ARG_UNUSED(cb);
    scan_calls++;
    if (scan_result == 0) {
        host_scanning = true;
        reported = false;
    }
    if (start_hook) {
        void (*hook)(void) = start_hook;
        start_hook = NULL;
        hook();
    }
    return scan_result;
}

int bt_le_scan_stop(void) {
    stop_calls++;
    if (stop_result == 0) {
        host_scanning = false;
    }
    return stop_result;
}

int bt_conn_le_create(const bt_addr_le_t *addr, const struct bt_conn_le_create_param *create,
                      const struct bt_le_conn_param *param, struct bt_conn **conn) {
    ARG_UNUSED(addr);
    ARG_UNUSED(create);
    ARG_UNUSED(param);
    create_calls++;
    if (create_hook) {
        void (*hook)(void) = create_hook;
        create_hook = NULL;
        hook();
    }
    if (create_result == 0) {
        *conn = (struct bt_conn *)&conn_tokens[0];
    }
    return create_result;
}

int bt_conn_get_info(const struct bt_conn *conn, struct bt_conn_info *info) {
    ARG_UNUSED(conn);
    memset(info, 0, sizeof(*info));
    info->role = BT_CONN_ROLE_CENTRAL;
    info->state = conn_state;
    return 0;
}

const bt_addr_le_t *bt_conn_get_dst(const struct bt_conn *conn) {
    ARG_UNUSED(conn);
    return &address;
}

/* Like Zephyr cancelling a create that waits for the peer's advertisement:
 * the connected callback runs at once with the reason as its error. */
int bt_conn_disconnect(struct bt_conn *conn, uint8_t reason) {
    disconnect_calls++;
    disconnect_reason = reason;
    if (disconnect_result < 0) {
        return disconnect_result;
    }
    if (conn_state == BT_CONN_STATE_CONNECTING) {
        conn_state = BT_CONN_STATE_DISCONNECTED;
        split_central_connected(conn, reason);
    }
    return 0;
}

static void split_central_device_found(const bt_addr_le_t *addr, int8_t rssi, uint8_t type,
                                      struct net_buf_simple *ad) {
    ARG_UNUSED(rssi);
    ARG_UNUSED(type);
    ARG_UNUSED(ad);
    split_central_eir_found(addr);
}

int64_t defy_hybrid_ble_hold_off(uint8_t slot) {
    ARG_UNUSED(slot);
    return hold_until - k_uptime_get();
}

/* An advertisement from the half. Like the controller's duplicate filter,
 * only the first one after each scan start is reported. */
static bool advertise(void) {
    if (!host_scanning || reported) {
        return false;
    }
    reported = true;
    split_central_device_found(&address, 0, BT_GAP_ADV_TYPE_ADV_IND, NULL);
    return true;
}

#include "scan_fixture.inc"

ZTEST(scan_recovery, test_split_settings_survive_bluetooth_initialization) {
    zassert_false(callbacks_registered_before_host);
    zassert_ok(settings_load());
    zassert_true(settings_loaded);
}

static void before(void *fixture) {
    ARG_UNUSED(fixture);
    is_enabled = false;
    struct k_work_sync sync;
    k_work_cancel_delayable_sync(&scan_retry_work, &sync);
    k_work_cancel_delayable_sync(&connect_timeout_work, &sync);
    k_work_cancel_delayable_sync(&hold_off_work, &sync);
    k_work_flush(&notify_status_work, &sync);
    is_scanning = false;
    scan_restart = false;
    host_scanning = reported = false;
    hold_until = 0;
    atomic_clear(&scan_operation);
    memset(peripherals, 0, sizeof(peripherals));
    scan_result = stop_result = create_result = reserve_result = 0;
    scan_calls = stop_calls = create_calls = release_calls = 0;
    process_calls = disconnect_calls = disconnect_result = 0;
    disconnect_reason = 0;
    conn_state = BT_CONN_STATE_CONNECTING;
    start_hook = NULL;
    create_hook = NULL;
    is_enabled = true;
}

static void await_scan(int expected) {
    int64_t deadline = k_uptime_get() + 800;
    while (scan_calls < expected && k_uptime_get() < deadline) {
        k_sleep(K_MSEC(10));
    }
    zassert_equal(scan_calls, expected);
}

ZTEST(scan_recovery, test_backoff_survives_transport_polling) {
    scan_result = -EAGAIN;
    zassert_equal(start_scanning(), -EAGAIN);
    for (int i = 0; i < 4; i++) {
        k_sleep(K_MSEC(100));
        zassert_ok(split_central_bt_set_enabled(true));
    }
    zassert_equal(scan_calls, 1);
    await_scan(2);
    zassert_false(is_scanning);
    scan_result = 0;
    await_scan(3);
    zassert_true(is_scanning);
}

ZTEST(scan_recovery, test_stop_failure_releases_slot_and_recovers) {
    zassert_ok(start_scanning());
    stop_result = -EIO;
    zassert_true(advertise());
    zassert_equal(release_calls, 1);
    zassert_equal(peripherals[0].state, PERIPHERAL_SLOT_STATE_OPEN);
    zassert_equal(create_calls, 0);
    zassert_false(is_scanning);
    stop_result = 0;
    await_scan(2);
    zassert_true(advertise());
    zassert_equal(create_calls, 1);
    zassert_equal(peripherals[0].state, PERIPHERAL_SLOT_STATE_CONNECTING);
}

ZTEST(scan_recovery, test_create_failure_retries_with_backoff) {
    zassert_ok(start_scanning());
    create_result = -ENOMEM;
    zassert_true(advertise());
    zassert_equal(release_calls, 1);
    zassert_equal(scan_calls, 1);
    zassert_is_null(peripherals[0].conn);
    create_result = 0;
    await_scan(2);
    zassert_true(advertise());
    zassert_equal(create_calls, 2);
}

ZTEST(scan_recovery, test_async_connection_failure_releases_slot_and_retries) {
    zassert_ok(start_scanning());
    zassert_true(advertise());
    split_central_connected(peripherals[0].conn, 0x3e);
    zassert_equal(release_calls, 1);
    zassert_equal(process_calls, 0);
    zassert_equal(scan_calls, 1);
    await_scan(2);
    zassert_true(is_scanning);
}

ZTEST(scan_recovery, test_retry_cannot_interrupt_connection_attempt) {
    peripherals[0].state = PERIPHERAL_SLOT_STATE_CONNECTING;
    peripherals[0].conn = (struct bt_conn *)&conn_tokens[0];
    retry_scanning();
    k_sleep(K_MSEC(600));
    zassert_equal(scan_calls, 0);
    zassert_false(k_work_delayable_is_pending(&scan_retry_work));
    split_central_connected(peripherals[0].conn, 0);
    zassert_equal(scan_calls, 1);
    zassert_equal(process_calls, 1);
}

ZTEST(scan_recovery, test_all_connected_stops_retrying) {
    retry_scanning();
    for (int i = 0; i < 2; i++) {
        peripherals[i].state = PERIPHERAL_SLOT_STATE_CONNECTED;
        peripherals[i].conn = (struct bt_conn *)&conn_tokens[i];
    }
    k_sleep(K_MSEC(600));
    zassert_equal(scan_calls, 0);
    zassert_false(k_work_delayable_is_pending(&scan_retry_work));
}

ZTEST(scan_recovery, test_disable_cancels_retry_and_ignores_late_advertisement) {
    scan_result = -EAGAIN;
    start_scanning();
    zassert_ok(split_central_bt_set_enabled(false));
    split_central_eir_found(&address);
    scan_result = 0;
    k_sleep(K_MSEC(600));
    zassert_equal(scan_calls, 1);
    zassert_equal(create_calls, 0);
    zassert_false(k_work_delayable_is_pending(&scan_retry_work));
    zassert_ok(split_central_bt_set_enabled(true));
    zassert_equal(scan_calls, 2);
}

static void disable_during_start(void) { split_central_bt_set_enabled(false); }

ZTEST(scan_recovery, test_disable_during_start_stops_completed_scan) {
    start_hook = disable_during_start;
    start_scanning();
    zassert_false(is_enabled);
    zassert_false(is_scanning);
    zassert_equal(stop_calls, 1);
    zassert_false(k_work_delayable_is_pending(&scan_retry_work));
}

static void advertisement_during_start(void) { zassert_true(advertise()); }

ZTEST(scan_recovery, test_overlapping_scan_operations_do_not_leak_slots) {
    start_hook = advertisement_during_start;
    zassert_ok(start_scanning());
    zassert_equal(stop_calls, 0);
    zassert_equal(create_calls, 0);
    zassert_equal(release_calls, 1);
    zassert_equal(peripherals[0].state, PERIPHERAL_SLOT_STATE_OPEN);
    zassert_true(is_scanning);
    zassert_false(advertise());
    await_scan(2);
    zassert_equal(stop_calls, 1);
    zassert_true(advertise());
    zassert_equal(create_calls, 1);
}

ZTEST(scan_recovery, test_report_dropped_by_busy_slot_is_seen_after_release) {
    zassert_ok(start_scanning());
    reserve_result = -ENOMEM;
    zassert_true(advertise());
    zassert_equal(create_calls, 0);
    zassert_false(advertise());
    reserve_result = 0;
    zassert_ok(start_scanning());
    zassert_equal(stop_calls, 1);
    zassert_equal(scan_calls, 2);
    zassert_ok(start_scanning());
    zassert_equal(scan_calls, 2);
    zassert_true(advertise());
    zassert_equal(create_calls, 1);
    zassert_equal(peripherals[0].state, PERIPHERAL_SLOT_STATE_CONNECTING);
}

ZTEST(scan_recovery, test_rejected_half_waits_out_its_hold) {
    hold_until = k_uptime_get() + 300;
    zassert_ok(start_scanning());
    zassert_true(advertise());
    zassert_equal(create_calls, 0);
    zassert_equal(stop_calls, 0);
    zassert_true(is_scanning);
    zassert_equal(peripherals[0].state, PERIPHERAL_SLOT_STATE_OPEN);
    zassert_false(advertise());
    k_sleep(K_MSEC(200));
    zassert_equal(scan_calls, 1);
    await_scan(2);
    zassert_equal(stop_calls, 1);
    zassert_true(advertise());
    zassert_equal(create_calls, 1);
}

ZTEST(scan_recovery, test_disable_cancels_pending_connection) {
    zassert_ok(start_scanning());
    zassert_true(advertise());
    zassert_equal(peripherals[0].state, PERIPHERAL_SLOT_STATE_CONNECTING);
    zassert_ok(split_central_bt_set_enabled(false));
    zassert_equal(disconnect_calls, 1);
    zassert_equal(disconnect_reason, BT_HCI_ERR_REMOTE_USER_TERM_CONN);
    zassert_equal(peripherals[0].state, PERIPHERAL_SLOT_STATE_OPEN);
    k_sleep(K_MSEC(600));
    zassert_equal(scan_calls, 1);
    zassert_ok(split_central_bt_set_enabled(true));
    zassert_equal(scan_calls, 2);
}

static void disable_during_create(void) { split_central_bt_set_enabled(false); }

ZTEST(scan_recovery, test_disable_during_create_cancels_it) {
    create_hook = disable_during_create;
    zassert_ok(start_scanning());
    zassert_true(advertise());
    zassert_equal(create_calls, 1);
    zassert_equal(disconnect_calls, 1);
    zassert_equal(disconnect_reason, BT_HCI_ERR_REMOTE_USER_TERM_CONN);
    zassert_equal(peripherals[0].state, PERIPHERAL_SLOT_STATE_OPEN);
    zassert_false(is_scanning);
}

ZTEST(scan_recovery, test_already_scanning_host_state_is_reset_before_retry) {
    scan_result = -EALREADY;
    zassert_equal(start_scanning(), -EALREADY);
    zassert_false(is_scanning);
    zassert_equal(stop_calls, 1);
    scan_result = 0;
    await_scan(2);
    zassert_true(is_scanning);
}

ZTEST(scan_recovery, test_unanswered_connection_attempt_is_cancelled) {
    zassert_ok(start_scanning());
    zassert_true(advertise());
    zassert_equal(peripherals[0].state, PERIPHERAL_SLOT_STATE_CONNECTING);
    zassert_equal(peripherals[1].state, PERIPHERAL_SLOT_STATE_OPEN);
    zassert_equal(start_scanning(), 0);
    zassert_equal(scan_calls, 1);

    k_sleep(K_MSEC(SPLIT_CONNECT_TIMEOUT_MS - 100));
    zassert_equal(disconnect_calls, 0);
    k_sleep(K_MSEC(200));
    zassert_equal(disconnect_calls, 1);
    zassert_equal(disconnect_reason, BT_HCI_ERR_UNKNOWN_CONN_ID);
    zassert_equal(release_calls, 1);
    zassert_equal(peripherals[0].state, PERIPHERAL_SLOT_STATE_OPEN);
    await_scan(2);
    zassert_true(is_scanning);
}

ZTEST(scan_recovery, test_connection_completing_at_timeout_is_kept) {
    zassert_ok(start_scanning());
    zassert_true(advertise());
    conn_state = BT_CONN_STATE_CONNECTED;
    k_sleep(K_MSEC(SPLIT_CONNECT_TIMEOUT_MS + 100));
    zassert_equal(disconnect_calls, 0);
    zassert_equal(peripherals[0].state, PERIPHERAL_SLOT_STATE_CONNECTING);
    split_central_connected(peripherals[0].conn, 0);
    zassert_equal(peripherals[0].state, PERIPHERAL_SLOT_STATE_CONNECTED);
    zassert_equal(process_calls, 1);
}

ZTEST(scan_recovery, test_failed_cancel_is_retried) {
    zassert_ok(start_scanning());
    zassert_true(advertise());
    disconnect_result = -ENOBUFS;
    k_sleep(K_MSEC(SPLIT_CONNECT_TIMEOUT_MS + 100));
    zassert_equal(disconnect_calls, 1);
    zassert_equal(peripherals[0].state, PERIPHERAL_SLOT_STATE_CONNECTING);
    disconnect_result = 0;
    k_sleep(K_MSEC(SPLIT_CONNECT_CANCEL_RETRY_MS));
    zassert_equal(disconnect_calls, 2);
    zassert_equal(peripherals[0].state, PERIPHERAL_SLOT_STATE_OPEN);
    await_scan(2);
}

ZTEST_SUITE(scan_recovery, NULL, NULL, before, NULL, NULL);
