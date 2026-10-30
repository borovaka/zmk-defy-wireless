/* SPDX-License-Identifier: MIT */

/*
 * The half's split transport, the one ZMK selects. It reports each key on both
 * the cable and ZMK's Bluetooth peripheral, keeps each enabled while it is
 * available, and refreshes the Neuron's Bluetooth snapshot of the held keys.
 * With half sleep, it shuts Bluetooth down before the half sleeps.
 */

#include <errno.h>

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/util.h>

#include <zmk/event_manager.h>
#include <zmk/events/split_peripheral_status_changed.h>
#include <zmk/split/transport/peripheral.h>

#include <dygma/defy_half.h>
#include <dygma/defy_hybrid.h>
#include <dygma/defy_split_peripheral.h>
#include <dygma/defy_startup.h>
#include <dygma/defy_wired_protocol.h>

#include "defy_split_internal.h"

#if IS_ENABLED(CONFIG_DEFY_HALF_SLEEP)
#include <zephyr/bluetooth/conn.h>

#include <dygma/defy_bt_identity.h>
#endif

LOG_MODULE_REGISTER(defy_hybrid_peripheral, CONFIG_ZMK_LOG_LEVEL);

static struct k_spinlock state_lock;
static K_MUTEX_DEFINE(report_lock);
static bool enabled;
static bool quiescing;
static uint8_t held_keys[DEFY_WIRED_BITMAP_SIZE];
static int64_t last_key_activity_ms;
static zmk_split_transport_peripheral_status_changed_cb_t status_callback;
/* The status last reported to ZMK. */
static struct zmk_split_transport_status published_status;
static int64_t next_snapshot_ms;

static void update(struct k_work *work);
static K_WORK_DELAYABLE_DEFINE(update_work, update);

static void request_update(void) {
    k_spinlock_key_t key = k_spin_lock(&state_lock);

    if (!quiescing) {
        k_work_reschedule(&update_work, K_NO_WAIT);
    }
    k_spin_unlock(&state_lock, key);
}

#if IS_ENABLED(CONFIG_DEFY_HALF_SLEEP)
#define BLE_STOP_TIMEOUT_MS 1000

/* Bluetooth on the way to sleep. A stop that sleep no longer wants still runs
 * to its end before Bluetooth comes back. */
enum ble_stop {
    BLE_RUNNING,
    /* stop_bluetooth() is queued or running. */
    BLE_STOPPING,
    /* stop_bluetooth() ended, with ble_stop_error. */
    BLE_STOPPED,
    /* Cancelled while stop_bluetooth() is queued or running. */
    BLE_STOP_CANCELLING,
    /* Cancelled, and stop_bluetooth() ended: the next update brings Bluetooth back. */
    BLE_STOP_CANCELLED,
};

/* All three under state_lock. */
static enum ble_stop ble_stop_state;
static int ble_stop_error;
static int64_t ble_stop_deadline;

struct held_connections {
    struct bt_conn *connections[CONFIG_BT_MAX_CONN];
    size_t count;
};

static void hold_connection(struct bt_conn *conn, void *context) {
    struct held_connections *held = context;

    if (held->count < ARRAY_SIZE(held->connections)) {
        held->connections[held->count++] = bt_conn_ref(conn);
    }
}

static void connection_active(struct bt_conn *conn, void *context) {
    struct bt_conn_info info;
    bool *active = context;

    if (bt_conn_get_info(conn, &info) < 0 || info.state != BT_CONN_STATE_DISCONNECTED) {
        *active = true;
    }
}

/* Called with state_lock held, once stop_bluetooth() is done or did not run. */
static void bluetooth_stop_ended(int err) {
    ble_stop_error = err;
    ble_stop_state = ble_stop_state == BLE_STOP_CANCELLING ? BLE_STOP_CANCELLED : BLE_STOPPED;
}

static void stop_bluetooth(struct k_work *work) {
    ARG_UNUSED(work);
    k_spinlock_key_t key = k_spin_lock(&state_lock);
    bool cancelled = ble_stop_state == BLE_STOP_CANCELLING;
    int err = 0;

    k_spin_unlock(&state_lock, key);
    if (!cancelled) {
        defy_bt_identity_set_advertising(false);
        k_work_cancel(&advertising_work);
        /*
         * Hold every connection, existing or pending, until the stop is done.
         * ZMK's set_enabled(false) disconnects the connection it finds with
         * bt_conn_foreach() without taking a reference, so this keeps that
         * object valid. It also keeps a connection that ends meanwhile from
         * being freed before set_enabled(false): ZMK's recycled callback
         * queues advertising_work while its transport is still enabled.
         */
        struct held_connections held = {0};

        bt_conn_foreach(BT_CONN_TYPE_LE, hold_connection, &held);
        err = bt_le_adv_stop();
        if (err == 0) {
            err = zmk_split_transport_peripheral_bt()->api->set_enabled(false);
        }
        k_work_cancel(&advertising_work);
        for (size_t i = 0; i < held.count; i++) {
            bt_conn_unref(held.connections[i]);
        }
    }
    key = k_spin_lock(&state_lock);
    bluetooth_stop_ended(err);
    k_spin_unlock(&state_lock, key);
    request_update();
}

static K_WORK_DEFINE(stop_bluetooth_work, stop_bluetooth);

static bool bluetooth_stopping(void) {
    k_spinlock_key_t key = k_spin_lock(&state_lock);
    bool stopping = ble_stop_state != BLE_RUNNING;

    k_spin_unlock(&state_lock, key);
    return stopping;
}

/* Called with state_lock held. Returns whether a stop was under way. */
static bool cancel_bluetooth_stop(void) {
    if (ble_stop_state == BLE_STOPPING) {
        ble_stop_state = BLE_STOP_CANCELLING;
    } else if (ble_stop_state == BLE_STOPPED) {
        ble_stop_state = BLE_STOP_CANCELLED;
    }
    return ble_stop_state != BLE_RUNNING;
}

/* Brings Bluetooth back after a cancelled stop has ended; whether it did. */
static bool restore_bluetooth(void) {
    k_spinlock_key_t key = k_spin_lock(&state_lock);
    bool restore = ble_stop_state == BLE_STOP_CANCELLED;

    if (restore) {
        ble_stop_state = BLE_RUNNING;
    }
    k_spin_unlock(&state_lock, key);
    if (restore) {
        defy_bt_identity_set_advertising(true);
    }
    return restore;
}

/* Queues stop_bluetooth() unless a stop is under way, and returns whether it
 * did. *err is then -EINPROGRESS, or the error that kept it from being queued. */
static bool start_bluetooth_stop(int *err) {
    k_spinlock_key_t key = k_spin_lock(&state_lock);
    bool start = ble_stop_state == BLE_RUNNING;

    if (start) {
        ble_stop_state = BLE_STOPPING;
        ble_stop_error = 0;
        ble_stop_deadline = k_uptime_get() + BLE_STOP_TIMEOUT_MS;
    }
    k_spin_unlock(&state_lock, key);
    if (!start) {
        return false;
    }
    *err = k_work_submit_to_queue(&service_work_q, &stop_bluetooth_work);
    if (*err < 0) {
        key = k_spin_lock(&state_lock);
        bluetooth_stop_ended(*err);
        k_spin_unlock(&state_lock, key);
    } else {
        *err = -EINPROGRESS;
    }
    return true;
}

/*
 * How the stop under way goes: 0 once Bluetooth is off, -EINPROGRESS while
 * it stops, -EBUSY once cancelled, -ETIMEDOUT after BLE_STOP_TIMEOUT_MS, or
 * the error that ended it.
 */
static int bluetooth_stop_result(void) {
    k_spinlock_key_t key = k_spin_lock(&state_lock);
    enum ble_stop state = ble_stop_state;
    int err = ble_stop_error;
    bool expired = k_uptime_get() >= ble_stop_deadline;

    k_spin_unlock(&state_lock, key);
    if (state == BLE_STOP_CANCELLING || state == BLE_STOP_CANCELLED) {
        return -EBUSY;
    }
    if (expired) {
        return -ETIMEDOUT;
    }
    if (state != BLE_STOPPED) {
        return -EINPROGRESS;
    }
    if (err < 0) {
        return err;
    }
    struct zmk_split_transport_status status =
        zmk_split_transport_peripheral_bt()->api->get_status();
    bool active = status.enabled ||
                  status.connections != ZMK_SPLIT_TRANSPORT_CONNECTIONS_STATUS_DISCONNECTED;

    bt_conn_foreach(BT_CONN_TYPE_LE, connection_active, &active);
    return active ? -EINPROGRESS : 0;
}

/* Starts stopping Bluetooth for sleep, or reports how the stop goes. */
static int prepare_bluetooth_stop(void) {
    int err;

    return start_bluetooth_stop(&err) ? err : bluetooth_stop_result();
}
#else
static inline bool bluetooth_stopping(void) { return false; }
static inline bool cancel_bluetooth_stop(void) { return false; }
static inline bool restore_bluetooth(void) { return false; }
static inline int prepare_bluetooth_stop(void) { return 0; }
#endif

static bool sleep_ready(int64_t min_idle_ms) {
    k_spinlock_key_t key = k_spin_lock(&state_lock);
    bool idle = enabled && k_uptime_get() - last_key_activity_ms >= min_idle_ms &&
                defy_wired_bitmap_empty(held_keys);

    k_spin_unlock(&state_lock, key);
    return idle;
}

void defy_hybrid_peripheral_cancel_sleep(void) {
    k_mutex_lock(&report_lock, K_FOREVER);
    defy_wired_peripheral_cancel_sleep();
    k_spinlock_key_t key = k_spin_lock(&state_lock);
    bool stopping = cancel_bluetooth_stop();
    bool changed = quiescing || stopping;

    quiescing = false;
    k_spin_unlock(&state_lock, key);
    if (changed) {
        request_update();
    }
    k_mutex_unlock(&report_lock);
}

int defy_hybrid_peripheral_prepare_sleep(int64_t min_idle_ms) {
    if (min_idle_ms < 0) {
        return -EINVAL;
    }
    if (k_mutex_lock(&report_lock, K_MSEC(100)) < 0) {
        return -ETIMEDOUT;
    }
    int err = sleep_ready(min_idle_ms) ? 0 : -EBUSY;

    if (!err && IS_ENABLED(CONFIG_DEFY_HALF_SLEEP)) {
        bool cabled = defy_wired_peripheral.api->get_status().connections !=
                      ZMK_SPLIT_TRANSPORT_CONNECTIONS_STATUS_DISCONNECTED;

        err = cabled ? -EBUSY : prepare_bluetooth_stop();
    }
    if (!err) {
        k_spinlock_key_t key = k_spin_lock(&state_lock);

        quiescing = true;
        k_spin_unlock(&state_lock, key);
        k_work_cancel_delayable(&update_work);
        err = defy_wired_peripheral_prepare_sleep();
        if (!err && !sleep_ready(min_idle_ms)) {
            err = -EBUSY;
        }
    }
    if (err < 0 && err != -EINPROGRESS) {
        defy_hybrid_peripheral_cancel_sleep();
    }
    k_mutex_unlock(&report_lock);
    return err;
}

static bool is_enabled(void) {
    k_spinlock_key_t key = k_spin_lock(&state_lock);
    bool value = enabled;

    k_spin_unlock(&state_lock, key);
    return value;
}

static int status_poll_ms(int64_t now) {
    k_spinlock_key_t key = k_spin_lock(&state_lock);
    bool idle = now - last_key_activity_ms >= CONFIG_DEFY_HYBRID_IDLE_TIMEOUT_MS &&
                defy_wired_bitmap_empty(held_keys);

    k_spin_unlock(&state_lock, key);
    return idle ? CONFIG_DEFY_HYBRID_IDLE_STATUS_POLL_MS
                : CONFIG_DEFY_HYBRID_PERIPHERAL_POLL_MS;
}

static struct zmk_split_transport_status transport_status(void) {
    struct zmk_split_transport_status cable = defy_wired_peripheral.api->get_status();
    struct zmk_split_transport_status bluetooth =
        zmk_split_transport_peripheral_bt()->api->get_status();
    bool active = is_enabled();
    bool connected = cable.connections == ZMK_SPLIT_TRANSPORT_CONNECTIONS_STATUS_ALL_CONNECTED ||
                     bluetooth.connections == ZMK_SPLIT_TRANSPORT_CONNECTIONS_STATUS_ALL_CONNECTED;

    return (struct zmk_split_transport_status){
        .available = cable.available || bluetooth.available,
        .enabled = active,
        .connections = active && connected ? ZMK_SPLIT_TRANSPORT_CONNECTIONS_STATUS_ALL_CONNECTED
                                           : ZMK_SPLIT_TRANSPORT_CONNECTIONS_STATUS_DISCONNECTED,
    };
}

static void enable_backend(const struct zmk_split_transport_peripheral *backend, bool active) {
    struct zmk_split_transport_status status = backend->api->get_status();
    bool desired = active && status.available;

    if (status.enabled != desired) {
        int err = backend->api->set_enabled(desired);

        if (err < 0) {
            LOG_WRN("Split backend enable failed (%d)", err);
        }
    }
}

/* Enables each link while this transport is enabled and the link available,
 * and disables it otherwise. Bluetooth is left alone while it stops for sleep.
 * Returns whether it is stopping. */
static bool enable_backends(bool active) {
    const struct zmk_split_transport_peripheral *bluetooth = zmk_split_transport_peripheral_bt();

    enable_backend(&defy_wired_peripheral, active);
    bool restored = restore_bluetooth();
    bool stopping = bluetooth_stopping();

    if (restored && active && bluetooth->api->get_status().available) {
        bluetooth->api->set_enabled(true);
    } else if (!stopping) {
        enable_backend(bluetooth, active);
    }
    return stopping;
}

/* Tells ZMK, and the start-up LEDs, when the combined status changes. */
static void publish_status(void) {
    struct zmk_split_transport_status current = transport_status();
    k_spinlock_key_t key = k_spin_lock(&state_lock);
    zmk_split_transport_peripheral_status_changed_cb_t callback = status_callback;

    k_spin_unlock(&state_lock, key);
    if (current.available == published_status.available &&
        current.enabled == published_status.enabled &&
        current.connections == published_status.connections) {
        return;
    }
    if (callback != NULL) {
        callback(&defy_hybrid_peripheral, current);
    }
    if (current.connections != published_status.connections) {
        defy_startup_split_connected(current.connections ==
                                     ZMK_SPLIT_TRANSPORT_CONNECTIONS_STATUS_ALL_CONNECTED);
    }
    published_status = current;
}

/* Notifies the Neuron of every held key at a fixed interval while Bluetooth
 * is connected, which puts its state right after a lost notification. */
static void refresh_snapshot(bool active, bool bluetooth_connected) {
    if (!active || !bluetooth_connected || k_uptime_get() < next_snapshot_ms) {
        return;
    }
    int err = send_position_state();

    if (err < 0) {
        LOG_WRN("Split snapshot refresh failed (%d)", err);
    }
    next_snapshot_ms = k_uptime_get() + CONFIG_DEFY_HYBRID_SNAPSHOT_INTERVAL_MS;
}

static void schedule_update(bool bluetooth_connected) {
    if (!is_enabled()) {
        return;
    }
    int64_t now = k_uptime_get();
    int delay_ms = status_poll_ms(now);

    if (bluetooth_connected) {
        delay_ms = MIN(delay_ms, MAX(0, next_snapshot_ms - now));
    }
    k_spinlock_key_t key = k_spin_lock(&state_lock);

    if (!quiescing) {
        k_work_schedule(&update_work, K_MSEC(delay_ms));
    }
    k_spin_unlock(&state_lock, key);
}

static void update(struct k_work *work) {
    ARG_UNUSED(work);
    k_mutex_lock(&report_lock, K_FOREVER);
    k_spinlock_key_t key = k_spin_lock(&state_lock);
    bool paused = quiescing;

    k_spin_unlock(&state_lock, key);
    if (paused) {
        k_mutex_unlock(&report_lock);
        return;
    }
    bool active = is_enabled();
    bool stopping = enable_backends(active);

    publish_status();
    bool bluetooth_connected =
        !stopping && zmk_split_transport_peripheral_bt()->api->get_status().connections ==
                         ZMK_SPLIT_TRANSPORT_CONNECTIONS_STATUS_ALL_CONNECTED;

    refresh_snapshot(active, bluetooth_connected);
    schedule_update(bluetooth_connected);
    k_mutex_unlock(&report_lock);
}

static int backend_status_changed(const struct zmk_split_transport_peripheral *backend,
                                  struct zmk_split_transport_status status) {
    ARG_UNUSED(backend);
    ARG_UNUSED(status);
    request_update();
    return 0;
}

/*
 * ZMK's Bluetooth peripheral reports its link changes through this event alone, not
 * through its status callback. Update at once rather than at the next poll.
 */
static int bluetooth_status_changed(const zmk_event_t *event) {
    ARG_UNUSED(event);
    request_update();
    return ZMK_EV_EVENT_BUBBLE;
}

ZMK_LISTENER(defy_hybrid_peripheral, bluetooth_status_changed);
ZMK_SUBSCRIPTION(defy_hybrid_peripheral, zmk_split_peripheral_status_changed);

static int set_enabled(bool value) {
    k_mutex_lock(&report_lock, K_FOREVER);
    defy_wired_peripheral.api->set_status_callback(backend_status_changed);
    zmk_split_transport_peripheral_bt()->api->set_status_callback(backend_status_changed);

    k_spinlock_key_t key = k_spin_lock(&state_lock);

    if (value && !enabled) {
        last_key_activity_ms = k_uptime_get();
    }
    enabled = value;
    k_spin_unlock(&state_lock, key);
    if (IS_ENABLED(CONFIG_DEFY_HALF_SLEEP) && !value) {
        defy_hybrid_peripheral_cancel_sleep();
    }
    request_update();
    k_mutex_unlock(&report_lock);
    return 0;
}

static int report_event(const struct zmk_split_transport_peripheral_event *event) {
    if (event->type == ZMK_SPLIT_TRANSPORT_PERIPHERAL_EVENT_TYPE_KEY_POSITION_EVENT &&
        !defy_wired_position_owned(DEFY_HALF_ID, event->data.key_position_event.position)) {
        return -EINVAL;
    }

    k_mutex_lock(&report_lock, K_FOREVER);
    k_spinlock_key_t key = k_spin_lock(&state_lock);
    int blocked = !enabled ? -EACCES : (quiescing ? -EBUSY : 0);

    k_spin_unlock(&state_lock, key);
    if (blocked) {
        k_mutex_unlock(&report_lock);
        return blocked;
    }
    if (event->type == ZMK_SPLIT_TRANSPORT_PERIPHERAL_EVENT_TYPE_KEY_POSITION_EVENT) {
        uint8_t position = event->data.key_position_event.position;
        bool pressed = event->data.key_position_event.pressed != 0;
        key = k_spin_lock(&state_lock);
        bool changed = ((held_keys[position / 8] & BIT(position % 8)) != 0) != pressed;

        if (changed) {
            WRITE_BIT(held_keys[position / 8], position % 8, pressed);
            last_key_activity_ms = k_uptime_get();
        }
        k_spin_unlock(&state_lock, key);
        if (changed) {
            if (bluetooth_stopping()) {
                defy_hybrid_peripheral_cancel_sleep();
            }
            request_update();
        }
    }
    int cable_err = defy_wired_peripheral.api->report_event(event);
    int bluetooth_err = zmk_split_transport_peripheral_bt()->api->report_event(event);

    k_mutex_unlock(&report_lock);
    return cable_err == 0 || bluetooth_err == 0 ? 0 : bluetooth_err;
}

static int set_status_callback(zmk_split_transport_peripheral_status_changed_cb_t callback) {
    k_spinlock_key_t key = k_spin_lock(&state_lock);

    status_callback = callback;
    k_spin_unlock(&state_lock, key);
    return 0;
}

static const struct zmk_split_transport_peripheral_api api = {
    .report_event = report_event,
    .set_enabled = set_enabled,
    .get_status = transport_status,
    .set_status_callback = set_status_callback,
};

/*
 * ZMK enables the first available transport in the order of the string
 * "<priority>_<name>", so priority 0 puts this one ahead of the cable and ZMK's
 * Bluetooth peripheral, which it drives itself.
 */
ZMK_SPLIT_TRANSPORT_PERIPHERAL_REGISTER(defy_hybrid_peripheral, &api, 0);
