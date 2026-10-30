/* SPDX-License-Identifier: MIT */

#include <errno.h>
#include <string.h>

#include <zephyr/drivers/spi.h>
#include <zephyr/init.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/atomic.h>
#include <zephyr/ztest.h>

#include <zmk/events/split_peripheral_status_changed.h>
#include <zmk/split/transport/peripheral.h>
#include <dygma/defy_split_peripheral.h>
#include <dygma/defy_wired_protocol.h>

#include "fake_links.h"

static K_MUTEX_DEFINE(lock);
static atomic_t reports_active;
static atomic_t pause_transfer;
static bool pause_requires_ack;
static K_SEM_DEFINE(transfer_paused, 0, 1);
static K_SEM_DEFINE(transfer_resume, 0, 1);
static struct defy_wired_rx receiver;
static uint8_t reply[DEFY_WIRED_FRAME_SIZE];
static struct fake_link_snapshot data;
static bool cable_online;
static bool bluetooth_available;
static bool bluetooth_connected;
static bool slow_reports;
#if IS_ENABLED(CONFIG_DEFY_HALF_SLEEP)
static bool delayed_disconnect;
static bool conn_present;
static enum bt_conn_state conn_state;
static bool advertising_allowed = true;
static int adv_stop_error;
static unsigned int adv_stop_calls;
static atomic_t conn_refs;
static atomic_t stop_paused;
static K_SEM_DEFINE(stop_started, 0, 1);
static K_SEM_DEFINE(stop_resume, 0, 1);
static int fake_conn;
struct k_work_q service_work_q;
static K_THREAD_STACK_DEFINE(service_stack, 2048);

static void advertising_handler(struct k_work *work) { ARG_UNUSED(work); }
K_WORK_DEFINE(advertising_work, advertising_handler);

static int init_service_queue(void) {
    k_work_queue_start(&service_work_q, service_stack, K_THREAD_STACK_SIZEOF(service_stack),
                       5, NULL);
    return 0;
}
SYS_INIT(init_service_queue, APPLICATION, 50);

void defy_bt_identity_set_advertising(bool allowed) {
    k_mutex_lock(&lock, K_FOREVER);
    advertising_allowed = allowed;
    k_mutex_unlock(&lock);
}

struct bt_conn *bt_conn_ref(struct bt_conn *conn) {
    zassert_equal(conn, (struct bt_conn *)&fake_conn);
    atomic_inc(&conn_refs);
    return conn;
}

void bt_conn_unref(struct bt_conn *conn) {
    zassert_equal(conn, (struct bt_conn *)&fake_conn);
    zassert_true(atomic_dec(&conn_refs) > 0);
}

void bt_conn_foreach(enum bt_conn_type type, void (*func)(struct bt_conn *, void *), void *data) {
    zassert_equal(type, BT_CONN_TYPE_LE);
    k_mutex_lock(&lock, K_FOREVER);
    bool present = conn_present;
    k_mutex_unlock(&lock);
    if (present) {
        func((struct bt_conn *)&fake_conn, data);
    }
}

int bt_conn_get_info(const struct bt_conn *conn, struct bt_conn_info *info) {
    zassert_equal(conn, (struct bt_conn *)&fake_conn);
    k_mutex_lock(&lock, K_FOREVER);
    *info = (struct bt_conn_info){.state = conn_state};
    k_mutex_unlock(&lock);
    return 0;
}

int bt_le_adv_stop(void) {
    zassert_equal(k_current_get(), &service_work_q.thread);
    k_mutex_lock(&lock, K_FOREVER);
    zassert_false(advertising_allowed);
    adv_stop_calls++;
    int err = adv_stop_error;
    k_mutex_unlock(&lock);
    if (atomic_cas(&stop_paused, 1, 0)) {
        k_sem_give(&stop_started);
        k_sem_take(&stop_resume, K_FOREVER);
    }
    return err;
}
#endif
static zmk_split_transport_peripheral_status_changed_cb_t status_callback;

static int transfer(const struct device *dev, const struct spi_config *config,
                    const struct spi_buf_set *tx, const struct spi_buf_set *rx) {
    ARG_UNUSED(dev);
    ARG_UNUSED(config);
    zassert_equal(tx->count, 1);
    zassert_equal(rx->count, 1);
    zassert_equal(tx->buffers[0].len, DEFY_WIRED_FRAME_SIZE);
    zassert_equal(rx->buffers[0].len, DEFY_WIRED_FRAME_SIZE);
    k_mutex_lock(&lock, K_FOREVER);
    data.cable_exchanges++;
    memset(rx->buffers[0].buf, 0xff, DEFY_WIRED_FRAME_SIZE);
    if (cable_online) {
        memcpy(rx->buffers[0].buf, reply, DEFY_WIRED_FRAME_SIZE);
        struct defy_wired_packet packet;
        struct defy_wired_packet ack;

        zassert_ok(defy_wired_decode(tx->buffers[0].buf, DEFY_WIRED_FRAME_SIZE, &packet));
        int result = defy_wired_rx_accept(&receiver, &packet, &ack);

        zassert_true(result >= 0);
        if (result == DEFY_WIRED_UPDATED) {
            memcpy(data.cable_keys, packet.bitmap, sizeof(data.cable_keys));
            data.cable_updates++;
        }
        defy_wired_encode(&ack, reply);
    }
    k_mutex_unlock(&lock);
    if (atomic_get(&pause_transfer)) {
        struct defy_wired_packet response;
        bool valid_ack = defy_wired_decode(rx->buffers[0].buf, DEFY_WIRED_FRAME_SIZE,
                                          &response) == 0;

        if ((!pause_requires_ack || valid_ack) && atomic_cas(&pause_transfer, 1, 0)) {
            k_sem_give(&transfer_paused);
            k_sem_take(&transfer_resume, K_FOREVER);
        }
    }
    return 0;
}

static DEVICE_API(spi, spi_api) = {.transceive = transfer};
SPI_DEVICE_DT_DEFINE(DT_NODELABEL(spi0), NULL, NULL, NULL, NULL, POST_KERNEL,
                     CONFIG_SPI_INIT_PRIORITY, &spi_api);

static struct zmk_split_transport_status get_status(void) {
    k_mutex_lock(&lock, K_FOREVER);
    data.bluetooth_status_reads++;
    struct zmk_split_transport_status status = {
        .available = bluetooth_available,
        .enabled = data.bluetooth_enabled,
        .connections = bluetooth_connected ? ZMK_SPLIT_TRANSPORT_CONNECTIONS_STATUS_ALL_CONNECTED
                                            : ZMK_SPLIT_TRANSPORT_CONNECTIONS_STATUS_DISCONNECTED,
    };

    k_mutex_unlock(&lock);
    return status;
}

static int set_enabled(bool enabled) {
    k_mutex_lock(&lock, K_FOREVER);
    data.bluetooth_enabled = enabled;
    data.bluetooth_enable_calls++;
    bool disconnected = !enabled && bluetooth_connected;

    if (!enabled) {
#if IS_ENABLED(CONFIG_DEFY_HALF_SLEEP)
        if (!delayed_disconnect) {
            bluetooth_connected = false;
            conn_present = false;
            conn_state = BT_CONN_STATE_DISCONNECTED;
        }
#else
        bluetooth_connected = false;
#endif
    }
    k_mutex_unlock(&lock);
    if (disconnected) {
        raise_zmk_split_peripheral_status_changed(
            (struct zmk_split_peripheral_status_changed){.connected = false});
    }
    return 0;
}

static int set_status_callback(zmk_split_transport_peripheral_status_changed_cb_t callback) {
    status_callback = callback;
    return 0;
}

static void begin_report(void) {
    bool overlapping = atomic_inc(&reports_active) != 0;

    k_mutex_lock(&lock, K_FOREVER);
    if (overlapping) {
        data.concurrent_reports++;
    }
    bool slow = slow_reports;

    k_mutex_unlock(&lock);
    if (slow) {
        k_sleep(K_MSEC(2));
    }
}

static void notify_snapshot(void) {
    if (bluetooth_connected) {
        int64_t now = k_uptime_get();

        if (data.snapshots > 0) {
            data.max_snapshot_gap_ms = MAX(data.max_snapshot_gap_ms, now - data.last_snapshot_ms);
        }
        data.last_snapshot_ms = now;
        data.snapshots++;
        memcpy(data.notified_keys, data.bluetooth_keys, sizeof(data.notified_keys));
    }
}

int send_position_state(void) {
    begin_report();
    k_mutex_lock(&lock, K_FOREVER);
    notify_snapshot();
    k_mutex_unlock(&lock);
    atomic_dec(&reports_active);
    return 0;
}

static int report_event(const struct zmk_split_transport_peripheral_event *event) {
    if (event->type != ZMK_SPLIT_TRANSPORT_PERIPHERAL_EVENT_TYPE_KEY_POSITION_EVENT) {
        return -ENOTSUP;
    }
    uint8_t position = event->data.key_position_event.position;

    zassert_true(position < 128);
    begin_report();
    k_mutex_lock(&lock, K_FOREVER);
    WRITE_BIT(data.bluetooth_keys[position / 8], position % 8,
              event->data.key_position_event.pressed != 0);
    data.bluetooth_reports++;
    notify_snapshot();
    k_mutex_unlock(&lock);
    atomic_dec(&reports_active);
    return 0;
}

static const struct zmk_split_transport_peripheral_api api = {
    .report_event = report_event,
    .set_enabled = set_enabled,
    .get_status = get_status,
    .set_status_callback = set_status_callback,
};

ZMK_SPLIT_TRANSPORT_PERIPHERAL_REGISTER(fake_bt, &api, CONFIG_ZMK_SPLIT_BLE_PRIORITY);

struct zmk_split_transport_peripheral *zmk_split_transport_peripheral_bt(void) {
    return &fake_bt;
}

const struct zmk_event_type zmk_event_zmk_split_peripheral_status_changed;
extern const struct zmk_listener zmk_listener_defy_hybrid_peripheral;

/* ZMK's event manager, for the hybrid's listener alone. */
int raise_zmk_split_peripheral_status_changed(struct zmk_split_peripheral_status_changed status) {
    struct zmk_split_peripheral_status_changed_event event = {
        .header = {.event = &zmk_event_zmk_split_peripheral_status_changed},
        .data = status,
    };

    k_mutex_lock(&lock, K_FOREVER);
    if (status.connected) {
        data.zmk_connected_events++;
    } else {
        data.zmk_disconnected_events++;
    }
    k_mutex_unlock(&lock);
    return zmk_listener_defy_hybrid_peripheral.callback(&event.header);
}

void fake_links_reset(void) {
    k_mutex_lock(&lock, K_FOREVER);
    memset(&data, 0, sizeof(data));
    memset(reply, 0, sizeof(reply));
    defy_wired_rx_init(&receiver);
    cable_online = false;
    bluetooth_available = false;
    bluetooth_connected = false;
    slow_reports = false;
#if IS_ENABLED(CONFIG_DEFY_HALF_SLEEP)
    delayed_disconnect = false;
    conn_present = false;
    conn_state = BT_CONN_STATE_DISCONNECTED;
    advertising_allowed = true;
    adv_stop_error = 0;
    adv_stop_calls = 0;
    zassert_equal(atomic_get(&conn_refs), 0);
    atomic_clear(&stop_paused);
    k_sem_reset(&stop_started);
    k_sem_reset(&stop_resume);
#endif
    atomic_clear(&reports_active);
    atomic_clear(&pause_transfer);
    k_sem_reset(&transfer_paused);
    k_sem_reset(&transfer_resume);
    k_mutex_unlock(&lock);
}

void fake_cable_online(bool value) {
    k_mutex_lock(&lock, K_FOREVER);
    cable_online = value;
    k_mutex_unlock(&lock);
}

void fake_cable_pause_transfer(bool require_ack) {
    pause_requires_ack = require_ack;
    k_sem_reset(&transfer_paused);
    k_sem_reset(&transfer_resume);
    atomic_set(&pause_transfer, 1);
}

int fake_cable_await_pause(void) { return k_sem_take(&transfer_paused, K_MSEC(350)); }

void fake_cable_resume_transfer(void) { k_sem_give(&transfer_resume); }

void fake_bluetooth_available(bool value) {
    k_mutex_lock(&lock, K_FOREVER);
    bluetooth_available = value;
    k_mutex_unlock(&lock);
    if (status_callback != NULL) {
        status_callback(&fake_bt, get_status());
    }
}

void fake_bluetooth_connected(bool value) {
    k_mutex_lock(&lock, K_FOREVER);
    bluetooth_connected = value;
#if IS_ENABLED(CONFIG_DEFY_HALF_SLEEP)
    conn_present = value;
    conn_state = value ? BT_CONN_STATE_CONNECTED : BT_CONN_STATE_DISCONNECTED;
#endif
    k_mutex_unlock(&lock);
    raise_zmk_split_peripheral_status_changed(
        (struct zmk_split_peripheral_status_changed){.connected = value});
}

void fake_bluetooth_slow_reports(bool value) {
    k_mutex_lock(&lock, K_FOREVER);
    slow_reports = value;
    k_mutex_unlock(&lock);
}

struct fake_link_snapshot fake_links_snapshot(void) {
    k_mutex_lock(&lock, K_FOREVER);
    struct fake_link_snapshot snapshot = data;

    k_mutex_unlock(&lock);
    return snapshot;
}

#if IS_ENABLED(CONFIG_DEFY_HALF_SLEEP)
void fake_bluetooth_delay_disconnect(bool value) {
    k_mutex_lock(&lock, K_FOREVER);
    delayed_disconnect = value;
    k_mutex_unlock(&lock);
}

void fake_bluetooth_state(enum bt_conn_state state) {
    k_mutex_lock(&lock, K_FOREVER);
    conn_present = true;
    conn_state = state;
    k_mutex_unlock(&lock);
}

void fake_bluetooth_pause_stop(void) {
    k_sem_reset(&stop_started);
    k_sem_reset(&stop_resume);
    atomic_set(&stop_paused, 1);
}
int fake_bluetooth_await_stop(void) { return k_sem_take(&stop_started, K_MSEC(350)); }
void fake_bluetooth_resume_stop(void) { k_sem_give(&stop_resume); }
void fake_bluetooth_stop_error(int error) { adv_stop_error = error; }
bool fake_bluetooth_advertising_allowed(void) { return advertising_allowed; }
unsigned int fake_bluetooth_stop_calls(void) { return adv_stop_calls; }
int fake_bluetooth_ref_count(void) { return atomic_get(&conn_refs); }
#endif
