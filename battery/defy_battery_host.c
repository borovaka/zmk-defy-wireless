/*
 * The Neuron's battery reports to the computer: the standard Battery Service
 * with the lower of the halves' levels, and each half's battery record for
 * diagnostics.
 *
 * SPDX-License-Identifier: MIT
 */

#include <errno.h>
#include <string.h>

#include <zephyr/bluetooth/conn.h>
#include <zephyr/bluetooth/gatt.h>
#include <zephyr/bluetooth/uuid.h>
#include <zephyr/kernel.h>

#include <dygma/defy_battery_central.h>
#include <dygma/defy_control.h>
#include <dygma/defy_wired_protocol.h>

static void battery_notify_work(struct k_work *work);
static K_WORK_DELAYABLE_DEFINE(battery_notify, battery_notify_work);

#define NOTIFY_ATTEMPTS 4
#define NOTIFY_RETRY_MS 250

static struct {
    int64_t retry_at;
    uint32_t generation;
    uint8_t level;
    bool notified;
    bool blocked;
    uint8_t attempts;
} host_notifications[CONFIG_BT_MAX_CONN];
static struct k_spinlock notification_lock;
static uint32_t sample_revision;

static void reset_notification(uint8_t index, bool blocked) {
    k_spinlock_key_t key = k_spin_lock(&notification_lock);
    host_notifications[index].generation++;
    host_notifications[index].notified = false;
    host_notifications[index].blocked = blocked;
    host_notifications[index].attempts = 0;
    host_notifications[index].retry_at = 0;
    k_spin_unlock(&notification_lock, key);
}

static ssize_t read_level(struct bt_conn *conn, const struct bt_gatt_attr *attr, void *buf,
                          uint16_t length, uint16_t offset) {
    uint8_t level;
    if (defy_battery_central_aggregate(&level, NULL)) {
        return BT_GATT_ERR(BT_ATT_ERR_UNLIKELY);
    }
    return bt_gatt_attr_read(conn, attr, buf, length, offset, &level, sizeof(level));
}

/*
 * A host's write of the level's CCC. Zephyr stores the value after this returns,
 * before its cooperative RX context yields, so the notify work queued here sees it.
 */
static ssize_t level_subscription_write(struct bt_conn *conn, const struct bt_gatt_attr *attr,
                                         uint16_t value) {
    bool enabled = (value & BT_GATT_CCC_NOTIFY) != 0;
    if (enabled != bt_gatt_is_subscribed(conn, attr, BT_GATT_CCC_NOTIFY)) {
        reset_notification(bt_conn_index(conn), !enabled);
        defy_battery_host_changed();
    }
    return sizeof(value);
}

static struct _bt_gatt_ccc level_ccc =
    BT_GATT_CCC_INITIALIZER(NULL, level_subscription_write, NULL);

static const struct bt_gatt_cpf battery_format = {
    .format = 0x04,        /* uint8 */
    .unit = 0x27ad,        /* percentage */
    .name_space = 0x01,    /* Bluetooth SIG */
    .description = 0x0106, /* "main" */
};

BT_GATT_SERVICE_DEFINE(defy_battery_bas, BT_GATT_PRIMARY_SERVICE(BT_UUID_BAS),
                      BT_GATT_CHARACTERISTIC(BT_UUID_BAS_BATTERY_LEVEL,
                                             BT_GATT_CHRC_READ | BT_GATT_CHRC_NOTIFY,
                                             BT_GATT_PERM_READ, read_level, NULL, NULL),
                      BT_GATT_CCC_MANAGED(&level_ccc, BT_GATT_PERM_READ | BT_GATT_PERM_WRITE),
                      BT_GATT_CPF(&battery_format));

static struct {
    bool valid[2];
    uint8_t data[2][DEFY_BATTERY_DIAGNOSTIC_SIZE];
} diagnostic_reads[CONFIG_BT_MAX_CONN];
static struct k_spinlock diagnostic_lock;

static ssize_t read_diagnostic(struct bt_conn *conn, const struct bt_gatt_attr *attr, void *buf,
                               uint16_t length, uint16_t offset) {
    uint8_t side = POINTER_TO_UINT(attr->user_data);
    uint8_t index = bt_conn_index(conn);
    uint8_t data[DEFY_BATTERY_DIAGNOSTIC_SIZE];
    if (!offset && defy_battery_central_diagnostic(side, data)) {
        return BT_GATT_ERR(BT_ATT_ERR_UNLIKELY);
    }
    k_spinlock_key_t key = k_spin_lock(&diagnostic_lock);
    if (!offset) {
        memcpy(diagnostic_reads[index].data[side], data, sizeof(data));
        diagnostic_reads[index].valid[side] = true;
    } else if (!diagnostic_reads[index].valid[side]) {
        k_spin_unlock(&diagnostic_lock, key);
        return BT_GATT_ERR(BT_ATT_ERR_INVALID_OFFSET);
    }
    memcpy(data, diagnostic_reads[index].data[side], sizeof(data));
    k_spin_unlock(&diagnostic_lock, key);
    return bt_gatt_attr_read(conn, attr, buf, length, offset, data, sizeof(data));
}

/* The Neuron's battery diagnostics, under the halves' control service UUID: b010 for the left
 * half, b011 for the right. */
BT_GATT_SERVICE_DEFINE(
    defy_battery_diagnostics,
    BT_GATT_PRIMARY_SERVICE(BT_UUID_DECLARE_128(DEFY_CONTROL_SERVICE_UUID)),
    BT_GATT_CHARACTERISTIC(BT_UUID_DECLARE_128(DEFY_CONTROL_DIAGNOSTIC_UUID(DEFY_WIRED_LEFT)),
                           BT_GATT_CHRC_READ, BT_GATT_PERM_READ_ENCRYPT, read_diagnostic, NULL,
                           UINT_TO_POINTER(DEFY_WIRED_LEFT)),
    BT_GATT_CHARACTERISTIC(BT_UUID_DECLARE_128(DEFY_CONTROL_DIAGNOSTIC_UUID(DEFY_WIRED_RIGHT)),
                           BT_GATT_CHRC_READ, BT_GATT_PERM_READ_ENCRYPT, read_diagnostic, NULL,
                           UINT_TO_POINTER(DEFY_WIRED_RIGHT)));

/* A bonded host's stored subscription is already restored when this runs. */
static void host_connected(struct bt_conn *conn, uint8_t err) {
    reset_notification(bt_conn_index(conn), err != 0);
    if (!err) {
        defy_battery_host_changed();
    }
}

static void host_disconnected(struct bt_conn *conn, uint8_t reason) {
    ARG_UNUSED(reason);
    uint8_t index = bt_conn_index(conn);
    reset_notification(index, true);
    k_spinlock_key_t key = k_spin_lock(&diagnostic_lock);
    memset(&diagnostic_reads[index], 0, sizeof(diagnostic_reads[index]));
    k_spin_unlock(&diagnostic_lock, key);
}

BT_CONN_CB_DEFINE(defy_battery_host_callbacks) = {
    .connected = host_connected, .disconnected = host_disconnected,
};

static void notify_host(struct bt_conn *conn, void *data) {
    uint8_t index = bt_conn_index(conn);
    int64_t *retry_at = data;
    k_spinlock_key_t key = k_spin_lock(&notification_lock);
    uint32_t generation = host_notifications[index].generation;
    bool blocked = host_notifications[index].blocked;
    k_spin_unlock(&notification_lock, key);
    if (blocked || !bt_gatt_is_subscribed(conn, &defy_battery_bas.attrs[1], BT_GATT_CCC_NOTIFY)) {
        return;
    }
    uint8_t level;
    if (defy_battery_central_aggregate(&level, NULL)) {
        return;
    }
    key = k_spin_lock(&notification_lock);
    if (host_notifications[index].generation == generation &&
        host_notifications[index].level != level) {
        host_notifications[index].level = level;
        host_notifications[index].notified = false;
        host_notifications[index].attempts = 0;
        host_notifications[index].retry_at = 0;
    }
    bool skip = host_notifications[index].generation != generation ||
                host_notifications[index].blocked ||
                host_notifications[index].notified ||
                host_notifications[index].attempts >= NOTIFY_ATTEMPTS;
    if (!skip && host_notifications[index].retry_at > k_uptime_get()) {
        *retry_at = MIN(*retry_at, host_notifications[index].retry_at);
        skip = true;
    }
    if (!skip) {
        host_notifications[index].attempts++;
    }
    k_spin_unlock(&notification_lock, key);
    if (skip) {
        return;
    }
    int err = bt_gatt_notify(conn, &defy_battery_bas.attrs[1], &level, sizeof(level));
    key = k_spin_lock(&notification_lock);
    if (host_notifications[index].generation == generation && !err) {
        host_notifications[index].level = level;
        host_notifications[index].notified = true;
    } else if (host_notifications[index].generation == generation &&
               host_notifications[index].attempts < NOTIFY_ATTEMPTS &&
               (err == -ENOMEM || err == -EAGAIN || err == -ENOBUFS)) {
        host_notifications[index].retry_at = k_uptime_get() + NOTIFY_RETRY_MS;
        *retry_at = MIN(*retry_at, host_notifications[index].retry_at);
    }
    k_spin_unlock(&notification_lock, key);
}

static void battery_notify_work(struct k_work *work) {
    ARG_UNUSED(work);
    uint8_t level;
    uint32_t revision;
    if (defy_battery_central_aggregate(&level, &revision)) {
        return;
    }
    if (sample_revision != revision) {
        sample_revision = revision;
        k_spinlock_key_t key = k_spin_lock(&notification_lock);
        for (size_t i = 0; i < ARRAY_SIZE(host_notifications); i++) {
            host_notifications[i].attempts = 0;
            host_notifications[i].retry_at = 0;
        }
        k_spin_unlock(&notification_lock, key);
    }
    int64_t retry_at = INT64_MAX;
    bt_conn_foreach(BT_CONN_TYPE_LE, notify_host, &retry_at);
    if (retry_at != INT64_MAX) {
        k_work_schedule(&battery_notify, K_MSEC(MAX(1, retry_at - k_uptime_get())));
    }
}

void defy_battery_host_changed(void) {
    k_work_reschedule(&battery_notify, K_NO_WAIT);
}
