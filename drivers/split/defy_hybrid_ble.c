/* SPDX-License-Identifier: MIT */

/*
 * The hybrid central's watch over ZMK's Bluetooth split connections: it
 * follows each half's discovery and key subscription, takes its key
 * snapshots, disconnects a half that stops reporting or that the hybrid
 * central rejected, and keeps the split diagnostics (opcode 8).
 * docs/ble-input-recovery.md describes the recovery.
 */

#include <errno.h>
#include <string.h>

#include <zephyr/bluetooth/conn.h>
#include <zephyr/bluetooth/gatt.h>
#include <zephyr/bluetooth/uuid.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/byteorder.h>

#include <zmk/split/bluetooth/uuid.h>

#include <dygma/defy_control.h>
#include <dygma/defy_hybrid.h>
#include <dygma/defy_split_central.h>

#include "defy_split_internal.h"

#define BLE_HEALTH_TIMEOUT_MS 10000
#define BLE_HEALTH_RETRY_MS 1000
#define BLE_HEALTH_MAX_ATTEMPTS 3
#define BLE_SECURITY_SUBSCRIBE_ATTEMPTS 3
#define BLE_REJECT_HOLD_MS 1000
#define BLE_REJECT_HOLD_MAX_MS 60000

/* The split diagnostics' format, in byte 0 of its header. */
#define DIAGNOSTICS_VERSION 1
/* Byte 2 of a record. */
#define RECORD_CONNECTED BIT(0)
#define RECORD_POSITION_HANDLE BIT(1)
#define RECORD_SUBSCRIBED BIT(2)
#define RECORD_REPORTED BIT(3)

struct hybrid_ble_peer {
    struct bt_conn *conn;
    uint32_t epoch;
    uint16_t position_handle;
    struct bt_gatt_subscribe_params *subscription;
    int64_t last_report;
    int64_t retry_at;
    int64_t hold_until;
    int32_t hold_ms;
    uint8_t retry_count;
    bool disconnecting;
    bool rejected;
};

struct discovery_observer {
    struct bt_conn *conn;
    struct bt_gatt_discover_params *params;
    bt_gatt_discover_func_t callback;
    bool split;
};

struct subscription_observer {
    struct bt_conn *conn;
    struct bt_gatt_subscribe_params *params;
    bt_gatt_subscribe_func_t callback;
    uint32_t epoch;
    uint16_t value_handle;
    uint8_t security_retries;
    bool security_pending;
};

struct ble_diagnostic {
    struct bt_conn *conn;
    uint32_t epoch;
    int32_t discovery_error;
    int32_t subscribe_error;
    uint32_t connect_count;
    uint32_t disconnect_count;
    uint32_t report_count;
    uint32_t removed_count;
    uint32_t recovery_count;
    int64_t last_connect;
    int64_t last_report;
    uint8_t phase;
    uint8_t disconnect_reason;
    uint8_t subscribe_att_error;
    uint8_t discovery_type;
    bool reported;
    bool ever_reported;
};

/* Each indexed by ZMK's split slot. The split central discovers each slot's
 * services with one set of parameters, which discoveries[] follows. */
static struct hybrid_ble_peer peers[CONFIG_ZMK_SPLIT_BLE_CENTRAL_PERIPHERALS];
static struct discovery_observer discoveries[CONFIG_ZMK_SPLIT_BLE_CENTRAL_PERIPHERALS];
static struct subscription_observer subscriptions[CONFIG_ZMK_SPLIT_BLE_CENTRAL_PERIPHERALS];
static struct ble_diagnostic diagnostics[CONFIG_ZMK_SPLIT_BLE_CENTRAL_PERIPHERALS];
static struct k_spinlock ble_health_lock;
static K_MUTEX_DEFINE(health_io_lock);

BUILD_ASSERT(DEFY_HYBRID_BLE_DIAGNOSTICS_SIZE ==
                 DEFY_HYBRID_BLE_DIAGNOSTICS_HEADER_SIZE +
                     ARRAY_SIZE(diagnostics) * DEFY_HYBRID_BLE_DIAGNOSTICS_RECORD_SIZE,
             "a header and a record for each slot");

static uint8_t observe_discovery(struct bt_conn *conn, const struct bt_gatt_attr *attr,
                                 struct bt_gatt_discover_params *params);
static void observe_subscription(struct bt_conn *conn, uint8_t err,
                                 struct bt_gatt_subscribe_params *params);

static void diagnostic_connected(struct bt_conn *conn, int slot) {
    k_spinlock_key_t key = k_spin_lock(&ble_health_lock);
    struct ble_diagnostic *diagnostic = &diagnostics[slot];
    if (diagnostic->conn != conn) {
        diagnostic->conn = conn;
        diagnostic->connect_count++;
        diagnostic->last_connect = k_uptime_get();
        diagnostic->reported = false;
        diagnostic->phase = DEFY_HYBRID_BLE_CONNECTED;
    }
    k_spin_unlock(&ble_health_lock, key);
}

static uint32_t diagnostic_age(int64_t now, int64_t then, bool known) {
    return known ? (uint32_t)MIN(MAX(now - then, 0), UINT32_MAX - 1LL) : UINT32_MAX;
}

size_t defy_hybrid_ble_diagnostics(uint8_t *out, size_t capacity) {
    if (!out || capacity < DEFY_HYBRID_BLE_DIAGNOSTICS_SIZE) {
        return 0;
    }
    k_spinlock_key_t key = k_spin_lock(&ble_health_lock);
    int64_t now = k_uptime_get();
    out[0] = DIAGNOSTICS_VERSION;
    out[1] = ARRAY_SIZE(peers);
    sys_put_le16(DEFY_HYBRID_BLE_DIAGNOSTICS_RECORD_SIZE, out + 2);
    sys_put_le32((uint32_t)now, out + 4);
    for (size_t i = 0; i < ARRAY_SIZE(peers); i++) {
        const struct ble_diagnostic *diagnostic = &diagnostics[i];
        const struct hybrid_ble_peer *peer = &peers[i];
        uint8_t *record = out + DEFY_HYBRID_BLE_DIAGNOSTICS_HEADER_SIZE +
                          i * DEFY_HYBRID_BLE_DIAGNOSTICS_RECORD_SIZE;
        bool matching = diagnostic->conn && peer->conn == diagnostic->conn;
        record[0] = i;
        record[1] = diagnostic->phase;
        record[2] = (diagnostic->conn ? RECORD_CONNECTED : 0) |
                    (matching && peer->position_handle ? RECORD_POSITION_HANDLE : 0) |
                    (matching && peer->subscription ? RECORD_SUBSCRIBED : 0) |
                    (diagnostic->conn && diagnostic->reported ? RECORD_REPORTED : 0);
        record[3] = diagnostic->disconnect_reason;
        sys_put_le32(diagnostic->epoch, record + 4);
        sys_put_le32((uint32_t)diagnostic->discovery_error, record + 8);
        sys_put_le32((uint32_t)diagnostic->subscribe_error, record + 12);
        record[16] = diagnostic->subscribe_att_error;
        record[17] = diagnostic->discovery_type;
        sys_put_le16(0, record + 18);
        sys_put_le32(diagnostic->connect_count, record + 20);
        sys_put_le32(diagnostic->disconnect_count, record + 24);
        sys_put_le32(diagnostic->report_count, record + 28);
        sys_put_le32(diagnostic->removed_count, record + 32);
        sys_put_le32(diagnostic->recovery_count, record + 36);
        sys_put_le32(diagnostic_age(now, diagnostic->last_report, diagnostic->ever_reported),
                     record + 40);
        sys_put_le32(diagnostic_age(now, diagnostic->last_connect, diagnostic->connect_count),
                     record + 44);
    }
    k_spin_unlock(&ble_health_lock, key);
    return DEFY_HYBRID_BLE_DIAGNOSTICS_SIZE;
}

static bool peer_overdue(const struct hybrid_ble_peer *peer, int64_t now) {
    return !defy_hybrid_central_is_maintenance() && peer->conn &&
           !peer->disconnecting && peer->retry_count < BLE_HEALTH_MAX_ATTEMPTS &&
           (peer->rejected || now - peer->last_report >= BLE_HEALTH_TIMEOUT_MS) &&
           now >= peer->retry_at;
}

static void check_peers_work(struct k_work *work) {
    ARG_UNUSED(work);
    k_mutex_lock(&health_io_lock, K_FOREVER);
    for (size_t i = 0; i < ARRAY_SIZE(peers); i++) {
        struct hybrid_ble_peer *peer = &peers[i];
        k_spinlock_key_t key = k_spin_lock(&ble_health_lock);
        int64_t now = k_uptime_get();
        if (!peer_overdue(peer, now)) {
            k_spin_unlock(&ble_health_lock, key);
            continue;
        }
        struct bt_conn *conn = bt_conn_ref(peer->conn);
        uint32_t epoch = peer->epoch;
        peer->retry_count++;
        peer->retry_at = now + BLE_HEALTH_RETRY_MS;
        diagnostics[i].recovery_count++;
        diagnostics[i].phase = DEFY_HYBRID_BLE_RECOVERY;
        k_spin_unlock(&ble_health_lock, key);
        int err = bt_conn_disconnect(conn, BT_HCI_ERR_REMOTE_USER_TERM_CONN);
        key = k_spin_lock(&ble_health_lock);
        if (peer->conn == conn && peer->epoch == epoch && (err == 0 || err == -ENOTCONN)) {
            peer->disconnecting = true;
        }
        k_spin_unlock(&ble_health_lock, key);
        bt_conn_unref(conn);
    }
    k_mutex_unlock(&health_io_lock);
}

static K_WORK_DEFINE(health_work, check_peers_work);

int defy_hybrid_ble_set_maintenance(bool value) {
    int err = k_mutex_lock(&health_io_lock, K_MSEC(500));
    if (err) {
        return err;
    }
    k_spinlock_key_t key = k_spin_lock(&ble_health_lock);
    for (size_t i = 0; i < ARRAY_SIZE(peers); i++) {
        subscriptions[i].security_pending = false;
        if (!value) {
            peers[i].last_report = k_uptime_get();
            peers[i].retry_at = 0;
            peers[i].retry_count = 0;
        }
    }
    k_spin_unlock(&ble_health_lock, key);
    k_mutex_unlock(&health_io_lock);
    return 0;
}

void defy_hybrid_ble_check_health(int64_t now) {
    k_spinlock_key_t key = k_spin_lock(&ble_health_lock);
    bool overdue = false;
    for (size_t i = 0; i < ARRAY_SIZE(peers) && !overdue; i++) {
        overdue = peer_overdue(&peers[i], now);
    }
    k_spin_unlock(&ble_health_lock, key);
    if (overdue) {
        k_work_submit_to_queue(&split_central_split_run_q, &health_work);
    }
}

void defy_hybrid_ble_peer_rejected(uint8_t slot, uint32_t epoch) {
    if (slot >= ARRAY_SIZE(peers)) {
        return;
    }
    k_spinlock_key_t key = k_spin_lock(&ble_health_lock);
    struct hybrid_ble_peer *peer = &peers[slot];
    if (peer->conn && peer->epoch == epoch && !peer->rejected) {
        /* The scanner holds the slot off before it may reconnect, so a
         * persistent conflict cannot loop reconnects. The hold survives
         * reconnects and doubles with each rejection, from 1 s to 60 s; a
         * rejection more than 60 s after the last hold ended starts again at
         * 1 s. */
        int64_t now = k_uptime_get();
        if (now - peer->hold_until > BLE_REJECT_HOLD_MAX_MS) {
            peer->hold_ms = 0;
        }
        peer->hold_ms = CLAMP(peer->hold_ms * 2, BLE_REJECT_HOLD_MS, BLE_REJECT_HOLD_MAX_MS);
        peer->hold_until = now + peer->hold_ms;
        peer->rejected = true;
    }
    k_spin_unlock(&ble_health_lock, key);
    defy_hybrid_ble_check_health(k_uptime_get());
}

void defy_hybrid_ble_clear_hold_off(void) {
    k_spinlock_key_t key = k_spin_lock(&ble_health_lock);
    for (size_t i = 0; i < ARRAY_SIZE(peers); i++) {
        peers[i].hold_until = 0;
        peers[i].hold_ms = 0;
    }
    k_spin_unlock(&ble_health_lock, key);
}

int64_t defy_hybrid_ble_hold_off(uint8_t slot) {
    if (slot >= ARRAY_SIZE(peers)) {
        return 0;
    }
    k_spinlock_key_t key = k_spin_lock(&ble_health_lock);
    int64_t wait = peers[slot].hold_until - k_uptime_get();
    k_spin_unlock(&ble_health_lock, key);
    return wait;
}

static struct hybrid_ble_peer *peer_for_conn(struct bt_conn *conn) {
    for (size_t i = 0; i < ARRAY_SIZE(peers); i++) {
        if (peers[i].conn == conn) {
            return &peers[i];
        }
    }
    return NULL;
}

static void forget_peer(struct hybrid_ble_peer *peer) {
    k_spinlock_key_t key = k_spin_lock(&ble_health_lock);
    struct bt_conn *conn = peer->conn;
    uint32_t epoch = peer->epoch;
    peer->conn = NULL;
    peer->position_handle = 0;
    peer->subscription = NULL;
    k_spin_unlock(&ble_health_lock, key);
    if (!conn) {
        return;
    }
    if (IS_ENABLED(CONFIG_DEFY_CONTROL)) {
        defy_control_ble_disconnected(conn, peer - peers, epoch);
    }
    defy_hybrid_central_ble_update(peer - peers, epoch, false, NULL, 0);
    struct discovery_observer *discovery = &discoveries[peer - peers];
    if (discovery->conn == conn) {
        memset(discovery, 0, sizeof(*discovery));
    }
    struct subscription_observer *observer = &subscriptions[peer - peers];
    if (observer->conn == conn) {
        if (observer->params->subscribe == observe_subscription) {
            observer->params->subscribe = observer->callback;
        }
        memset(observer, 0, sizeof(*observer));
    }
    bt_conn_unref(conn);
}

static struct hybrid_ble_peer *prepare_peer(struct bt_conn *conn, int slot) {
    struct hybrid_ble_peer *peer = &peers[slot];
    if (peer->conn == conn) {
        return peer;
    }
    forget_peer(peer);
    struct bt_conn *reference = bt_conn_ref(conn);
    k_spinlock_key_t key = k_spin_lock(&ble_health_lock);
    peer->epoch++;
    if (peer->epoch == 0) {
        peer->epoch++;
    }
    peer->conn = reference;
    peer->last_report = k_uptime_get();
    peer->retry_at = 0;
    peer->retry_count = 0;
    peer->disconnecting = false;
    peer->rejected = false;
    diagnostics[slot].epoch = peer->epoch;
    k_spin_unlock(&ble_health_lock, key);
    if (IS_ENABLED(CONFIG_DEFY_CONTROL)) {
        defy_control_ble_connected(conn, slot, peer->epoch);
    }
    return peer;
}

static uint8_t observe_discovery(struct bt_conn *conn, const struct bt_gatt_attr *attr,
                                 struct bt_gatt_discover_params *params) {
    struct hybrid_ble_peer *peer = peer_for_conn(conn);
    struct discovery_observer *observer = peer ? &discoveries[peer - peers] : NULL;
    if (!observer || observer->conn != conn || observer->params != params ||
        !observer->callback) {
        return BT_GATT_ITER_STOP;
    }
    if (attr && attr->user_data && params->type == BT_GATT_DISCOVER_CHARACTERISTIC) {
        const struct bt_gatt_chrc *characteristic = attr->user_data;
        if (bt_uuid_cmp(characteristic->uuid,
                        BT_UUID_DECLARE_128(ZMK_SPLIT_BT_CHAR_POSITION_STATE_UUID)) == 0) {
            k_spinlock_key_t key = k_spin_lock(&ble_health_lock);
            peer->position_handle = bt_gatt_attr_value_handle(attr);
            k_spin_unlock(&ble_health_lock, key);
        }
    }
    if (observer->split && !attr && !peer->position_handle) {
        k_spinlock_key_t key = k_spin_lock(&ble_health_lock);
        diagnostics[peer - peers].phase = DEFY_HYBRID_BLE_SETUP_FAILED;
        k_spin_unlock(&ble_health_lock, key);
    }
    return observer->callback(conn, attr, params);
}

int defy_hybrid_ble_discover(struct bt_conn *conn, struct bt_gatt_discover_params *params) {
    int slot = peripheral_slot_index_for_conn(conn);
    if (slot < 0 || slot >= ARRAY_SIZE(peers) || !params || !params->func) {
        return bt_gatt_discover(conn, params);
    }
    diagnostic_connected(conn, slot);
    struct hybrid_ble_peer *peer = prepare_peer(conn, slot);
    struct discovery_observer *observer = &discoveries[slot];
    if (observer->params && observer->params != params) {
        /* Another discovery on the connection, not the split central's own. */
        return bt_gatt_discover(conn, params);
    }
    if (params->type == BT_GATT_DISCOVER_PRIMARY) {
        observer->split = params->uuid &&
            bt_uuid_cmp(params->uuid, BT_UUID_DECLARE_128(ZMK_SPLIT_BT_SERVICE_UUID)) == 0;
    }
    if (params->func != observe_discovery) {
        observer->callback = params->func;
    }
    observer->conn = conn;
    observer->params = params;
    params->func = observe_discovery;
    uint32_t epoch = peer->epoch;
    bool split = observer->split;
    if (split) {
        k_spinlock_key_t key = k_spin_lock(&ble_health_lock);
        diagnostics[slot].discovery_type = params->type;
        diagnostics[slot].phase = params->type == BT_GATT_DISCOVER_PRIMARY ?
            DEFY_HYBRID_BLE_SERVICE_DISCOVERY : DEFY_HYBRID_BLE_CHARACTERISTIC_DISCOVERY;
        k_spin_unlock(&ble_health_lock, key);
    }
    int err = bt_gatt_discover(conn, params);
    if (split) {
        k_spinlock_key_t key = k_spin_lock(&ble_health_lock);
        if (peer->conn == conn && peer->epoch == epoch) {
            diagnostics[slot].discovery_error = err;
            if (err < 0) {
                diagnostics[slot].phase = DEFY_HYBRID_BLE_SETUP_FAILED;
            }
        }
        k_spin_unlock(&ble_health_lock, key);
    }
    if (err < 0) {
        params->func = observer->callback;
    }
    return err;
}

static uint8_t position_notification(struct bt_conn *conn,
                                      struct bt_gatt_subscribe_params *params,
                                      const void *data, uint16_t length) {
    struct hybrid_ble_peer *peer = peer_for_conn(conn);
    if (!peer || peer->subscription != params) {
        return BT_GATT_ITER_STOP;
    }
    if (!data) {
        defy_hybrid_central_ble_update(peer - peers, peer->epoch, false, NULL, 0);
        k_spinlock_key_t key = k_spin_lock(&ble_health_lock);
        diagnostics[peer - peers].removed_count++;
        diagnostics[peer - peers].phase = DEFY_HYBRID_BLE_SUBSCRIPTION_REMOVED;
        params->value_handle = 0;
        peer->position_handle = 0;
        peer->subscription = NULL;
        k_spin_unlock(&ble_health_lock, key);
        return BT_GATT_ITER_STOP;
    }
    uint32_t epoch = peer->epoch;
    int err = defy_hybrid_central_ble_update(peer - peers, epoch, true, data, length);
    /*
     * -ENOMSG: the hybrid central's queue was full. The snapshot is still the
     * slot's latest, which the central applies after the queue, so the half
     * did report.
     */
    if (err == 0 || err == -ENOMSG) {
        k_spinlock_key_t key = k_spin_lock(&ble_health_lock);
        if (peer->conn == conn && peer->epoch == epoch && !peer->rejected) {
            peer->last_report = k_uptime_get();
            peer->retry_at = 0;
            peer->retry_count = 0;
            struct ble_diagnostic *diagnostic = &diagnostics[peer - peers];
            diagnostic->report_count++;
            diagnostic->last_report = peer->last_report;
            diagnostic->reported = true;
            diagnostic->ever_reported = true;
            diagnostic->phase = DEFY_HYBRID_BLE_REPORTING;
        }
        k_spin_unlock(&ble_health_lock, key);
    }
    return BT_GATT_ITER_CONTINUE;
}

static void retry_secure_subscription(struct bt_conn *conn, uint32_t epoch) {
    struct hybrid_ble_peer *peer = peer_for_conn(conn);
    if (!peer) {
        return;
    }
    struct subscription_observer *observer = &subscriptions[peer - peers];
    k_spinlock_key_t key = k_spin_lock(&ble_health_lock);
    if (defy_hybrid_central_is_maintenance()) {
        observer->security_pending = false;
    }
    if (!observer->security_pending || observer->conn != conn || observer->epoch != epoch ||
        peer->epoch != epoch || peer->subscription || peer->disconnecting ||
        !observer->params || !observer->params->ccc_handle ||
        observer->params->value != BT_GATT_CCC_NOTIFY || !observer->value_handle ||
        observer->security_retries >= BLE_SECURITY_SUBSCRIBE_ATTEMPTS) {
        k_spin_unlock(&ble_health_lock, key);
        return;
    }
    struct bt_conn *reference = bt_conn_ref(conn);
    k_spin_unlock(&ble_health_lock, key);
    if (bt_conn_get_security(reference) < BT_SECURITY_L2) {
        bt_conn_unref(reference);
        return;
    }
    key = k_spin_lock(&ble_health_lock);
    if (peer->conn != conn || peer->epoch != epoch || observer->conn != conn ||
        observer->epoch != epoch || !observer->security_pending ||
        defy_hybrid_central_is_maintenance()) {
        k_spin_unlock(&ble_health_lock, key);
        bt_conn_unref(reference);
        return;
    }
    struct bt_gatt_subscribe_params *params = observer->params;
    params->value_handle = observer->value_handle;
    peer->position_handle = observer->value_handle;
    observer->security_pending = false;
    observer->security_retries++;
    k_spin_unlock(&ble_health_lock, key);
    defy_hybrid_ble_subscribe(reference, params);
    bt_conn_unref(reference);
}

static void observe_subscription(struct bt_conn *conn, uint8_t err,
                                 struct bt_gatt_subscribe_params *params) {
    for (size_t i = 0; i < ARRAY_SIZE(subscriptions); i++) {
        struct subscription_observer *observer = &subscriptions[i];
        if (observer->conn != conn || observer->params != params) {
            continue;
        }
        bt_gatt_subscribe_func_t callback = observer->callback;
        uint32_t epoch = observer->epoch;
        k_spinlock_key_t key = k_spin_lock(&ble_health_lock);
        if (peers[i].conn == conn && peers[i].epoch == observer->epoch) {
            diagnostics[i].subscribe_att_error = err;
            observer->security_pending = err == BT_ATT_ERR_INSUFFICIENT_ENCRYPTION;
            if (err || !diagnostics[i].reported) {
                diagnostics[i].phase = err ? DEFY_HYBRID_BLE_SETUP_FAILED :
                                             DEFY_HYBRID_BLE_SUBSCRIBED;
            }
        }
        k_spin_unlock(&ble_health_lock, key);
        if (callback) {
            callback(conn, err, params);
        }
        if (err == BT_ATT_ERR_INSUFFICIENT_ENCRYPTION) {
            retry_secure_subscription(conn, epoch);
        }
        return;
    }
}

int defy_hybrid_ble_subscribe(struct bt_conn *conn, struct bt_gatt_subscribe_params *params) {
    struct hybrid_ble_peer *peer = peer_for_conn(conn);
    if (!peer || !peer->position_handle || !params ||
        params->value_handle != peer->position_handle) {
        return bt_gatt_subscribe(conn, params);
    }
    bt_gatt_notify_func_t original = params->notify;
    struct subscription_observer *observer = &subscriptions[peer - peers];
    struct subscription_observer previous = *observer;
    if (params->subscribe != observe_subscription) {
        observer->callback = params->subscribe;
    }
    observer->conn = conn;
    observer->params = params;
    observer->epoch = peer->epoch;
    observer->value_handle = params->value_handle;
    uint32_t epoch = peer->epoch;
    params->subscribe = observe_subscription;
    k_spinlock_key_t key = k_spin_lock(&ble_health_lock);
    peer->subscription = params;
    diagnostics[peer - peers].phase = params->ccc_handle ? DEFY_HYBRID_BLE_SUBSCRIBING :
                                                         DEFY_HYBRID_BLE_CCC_DISCOVERY;
    k_spin_unlock(&ble_health_lock, key);
    params->notify = position_notification;
    int err = bt_gatt_subscribe(conn, params);
    key = k_spin_lock(&ble_health_lock);
    bool current = peer->conn == conn && peer->epoch == epoch;
    if (current) {
        diagnostics[peer - peers].subscribe_error = err;
    }
    if (current && err < 0 && err != -EALREADY) {
        params->notify = original;
        peer->subscription = NULL;
        diagnostics[peer - peers].phase = DEFY_HYBRID_BLE_SETUP_FAILED;
        params->subscribe = observer->callback;
        *observer = previous;
        if (previous.params == params && previous.conn == conn) {
            params->subscribe = observe_subscription;
        }
    }
    k_spin_unlock(&ble_health_lock, key);
    return err;
}

static void connected(struct bt_conn *conn, uint8_t err) {
    int slot = peripheral_slot_index_for_conn(conn);
    if (!err && slot >= 0 && slot < ARRAY_SIZE(peers)) {
        diagnostic_connected(conn, slot);
    }
}

static void security_changed(struct bt_conn *conn, bt_security_t level, enum bt_security_err err) {
    struct hybrid_ble_peer *peer = peer_for_conn(conn);
    if (!err && level >= BT_SECURITY_L2 && peer) {
        retry_secure_subscription(conn, peer->epoch);
    }
}

static void disconnected(struct bt_conn *conn, uint8_t reason) {
    k_spinlock_key_t key = k_spin_lock(&ble_health_lock);
    for (size_t i = 0; i < ARRAY_SIZE(diagnostics); i++) {
        if (diagnostics[i].conn == conn) {
            diagnostics[i].conn = NULL;
            diagnostics[i].disconnect_count++;
            diagnostics[i].disconnect_reason = reason;
            diagnostics[i].phase = DEFY_HYBRID_BLE_IDLE;
        }
    }
    k_spin_unlock(&ble_health_lock, key);
    struct hybrid_ble_peer *peer = peer_for_conn(conn);
    if (peer) {
        forget_peer(peer);
    }
}

BT_CONN_CB_DEFINE(defy_hybrid_ble_callbacks) = {
    .connected = connected,
    .disconnected = disconnected,
    .security_changed = security_changed,
};
