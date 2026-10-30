/*
 * The Neuron's client of each half's control service (dygma/defy_control.h)
 * over the hybrid central's Bluetooth links. It reads the battery frame every
 * BATTERY_POLL_MS and sends the battery display and the host display. It resends the
 * lighting state until the half's read acknowledges it, each time after the
 * groups the state names that the half has not confirmed on this connection;
 * before the first state it reads the half's wake report. It reads the
 * version report once per connection. GATT calls may wait for buffers, so all
 * of this runs on a work queue of its own.
 *
 * SPDX-License-Identifier: MIT
 */

#include <errno.h>
#include <string.h>

#include <zephyr/bluetooth/conn.h>
#include <zephyr/bluetooth/gatt.h>
#include <zephyr/bluetooth/uuid.h>
#include <zephyr/init.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

#include <dygma/defy_battery_central.h>
#include <dygma/defy_control.h>
#include <dygma/defy_feedback.h>
#include <dygma/defy_half_version.h>
#include <dygma/defy_hybrid.h>
#include <dygma/defy_lighting.h>
#include <dygma/defy_wired_protocol.h>

LOG_MODULE_REGISTER(defy_control, CONFIG_ZMK_LOG_LEVEL);

#define BATTERY_POLL_MS 15000
#define RETRY_MS 1000
#define LIGHTING_PREFLIGHT_ATTEMPTS 3
#define LIGHTING_PREFLIGHT_RETRY_MS 500
/* How long a new link's first request waits. */
#define CONNECT_DELAY_MS 1000
#define CONTROL_STACK_SIZE 2048
#define CONTROL_PRIORITY K_PRIO_PREEMPT(8)
#define BOTH_SIDES (BIT(DEFY_WIRED_LEFT) | BIT(DEFY_WIRED_RIGHT))
/* The hybrid central's Bluetooth slots, one for each half. */
#define BLE_SLOTS 2

struct control_peer;

/*
 * At a read's end, under peer_lock: complete when the read reached its end
 * without error. Returns whether a read error deserves a warning.
 */
typedef bool (*read_done_t)(struct control_peer *peer, bool complete, uint8_t error);

struct control_peer {
    struct bt_conn *conn;
    uint32_t epoch;
    uint8_t slot;
    /* The link is up; false until the poll releases the peer. */
    bool live;
    /* A GATT request is in flight; its callback clears this. */
    bool busy;
    bool discovering;
    /* The value handles of the battery frame and command characteristics, once discovered. */
    uint16_t battery_handle;
    uint16_t command_handle;
    bool show_requested;
    bool show_queued;
    uint8_t show_side;
    int64_t show_deadline;
    struct defy_host_feedback feedback;
    int64_t feedback_deadline;
    bool feedback_pending;
    struct defy_lighting_retry lighting;
    bool lighting_queued;
    bool lighting_read_pending;
    bool lighting_preflight_done;
    bool lighting_wake_ready;
    uint8_t lighting_preflight_attempts;
    int64_t lighting_preflight_due;
    /* The half's side, once its wake report gave it. */
    bool lighting_side_known;
    uint8_t lighting_side;
    /* The groups the half confirmed on this connection. */
    uint16_t lighting_groups;
    /* The version report's value handle, once discovered, and whether the
     * discovery has looked at the characteristic after the command's. */
    uint16_t version_handle;
    bool version_checked;
    /* The report is still to be read, from version_due on. */
    bool version_pending;
    int64_t version_due;
    /* When the next battery read, or the discovery before it, is due. */
    int64_t due;
    /* The read in progress, of the battery frame, the wake report, the
     * lighting acknowledgement or the version report, what it returned so far,
     * and its end. */
    int64_t started_at;
    size_t read_length;
    uint8_t read_data[DEFY_BATTERY_FRAME_SIZE];
    read_done_t read_done;
    /* A battery frame was read whole, for forward_frame(). */
    bool battery_ready;
    struct bt_gatt_discover_params discovery;
    struct bt_gatt_read_params read;
};

BUILD_ASSERT(DEFY_BATTERY_FRAME_SIZE >= DEFY_LIGHTING_READ_SIZE,
             "a read holds the battery frame or the lighting characteristic");
BUILD_ASSERT(DEFY_BATTERY_FRAME_SIZE >= DEFY_HALF_VERSION_SIZE,
             "a read holds the version report");

static struct control_peer control_peers[CONFIG_BT_MAX_CONN];
static struct k_spinlock peer_lock;
static struct k_work_q control_work_queue;
static K_THREAD_STACK_DEFINE(control_work_stack, CONTROL_STACK_SIZE);
static void control_poll_work(struct k_work *work);
static K_WORK_DELAYABLE_DEFINE(control_poll, control_poll_work);
static const struct bt_uuid_128 battery_uuid = BT_UUID_INIT_128(DEFY_CONTROL_BATTERY_UUID);
static const struct bt_uuid_128 command_uuid = BT_UUID_INIT_128(DEFY_CONTROL_COMMAND_UUID);

static void schedule_poll(k_timeout_t delay) {
    k_work_reschedule_for_queue(&control_work_queue, &control_poll, delay);
}

/* The read callback of every request but the discovery. */
static uint8_t read_chunk(struct bt_conn *conn, uint8_t error, struct bt_gatt_read_params *params,
                          const void *data, uint16_t length) {
    struct control_peer *peer = CONTAINER_OF(params, struct control_peer, read);
    k_spinlock_key_t key = k_spin_lock(&peer_lock);
    if (peer->conn != conn || !peer->busy) {
        k_spin_unlock(&peer_lock, key);
        return BT_GATT_ITER_STOP;
    }
    if (peer->live && !error && data && length <= sizeof(peer->read_data) - peer->read_length) {
        memcpy(peer->read_data + peer->read_length, data, length);
        peer->read_length += length;
        k_spin_unlock(&peer_lock, key);
        return BT_GATT_ITER_CONTINUE;
    }
    bool warn = peer->read_done(peer, peer->live && !error && !data, error) && error;
    uint8_t slot = peer->slot;
    uint16_t handle = params->single.handle;
    peer->busy = false;
    k_spin_unlock(&peer_lock, key);
    if (warn) {
        LOG_WRN("Control BLE read slot=%u handle=%u error=%u", slot, handle, error);
    }
    schedule_poll(K_MSEC(1));
    return BT_GATT_ITER_STOP;
}

/* A battery frame read whole goes to the battery central at the next poll. */
static bool battery_read_done(struct control_peer *peer, bool complete, uint8_t error) {
    ARG_UNUSED(error);
    peer->battery_ready = complete && peer->read_length == DEFY_BATTERY_FRAME_SIZE;
    peer->due = k_uptime_get() + BATTERY_POLL_MS;
    return true;
}

static void feedback_send_work(struct k_work *work) {
    ARG_UNUSED(work);
    for (size_t i = 0; i < ARRAY_SIZE(control_peers); i++) {
        struct control_peer *peer = &control_peers[i];
        k_spinlock_key_t key = k_spin_lock(&peer_lock);
        int64_t remaining = peer->feedback_deadline - k_uptime_get();
        bool send = !defy_hybrid_central_is_maintenance() && peer->feedback_pending && peer->conn &&
                    peer->live && peer->command_handle && remaining > 0;
        struct bt_conn *conn = send ? bt_conn_ref(peer->conn) : NULL;
        struct defy_host_feedback state = peer->feedback;
        uint16_t handle = peer->command_handle;
        peer->feedback_pending = false;
        k_spin_unlock(&peer_lock, key);
        if (!conn) {
            continue;
        }
        state.remaining_ms = MIN(remaining, DEFY_HOST_FEEDBACK_DURATION_MS);
        uint8_t data[DEFY_HOST_FEEDBACK_SIZE];
        if (bt_conn_get_security(conn) >= BT_SECURITY_L2 &&
            !defy_host_feedback_encode(&state, data)) {
            bt_gatt_write_without_response(conn, handle, data, sizeof(data), false);
        }
        bt_conn_unref(conn);
    }
}

static K_WORK_DEFINE(feedback_send, feedback_send_work);

void defy_control_ble_host_feedback_show(const struct defy_host_feedback *state) {
    if (!IS_ENABLED(CONFIG_DEFY_LIGHTING) || defy_hybrid_central_is_maintenance()) {
        return;
    }
    k_spinlock_key_t key = k_spin_lock(&peer_lock);
    for (size_t i = 0; i < ARRAY_SIZE(control_peers); i++) {
        struct control_peer *peer = &control_peers[i];
        if (peer->conn && peer->live && peer->command_handle) {
            peer->feedback = *state;
            peer->feedback_deadline = k_uptime_get() + state->remaining_ms;
            peer->feedback_pending = true;
        }
    }
    k_spin_unlock(&peer_lock, key);
    k_work_submit_to_queue(&control_work_queue, &feedback_send);
}

void defy_control_ble_lighting_changed(void) {
    schedule_poll(K_NO_WAIT);
}

static void lighting_preflight_failed(struct control_peer *peer, bool retry) {
    peer->lighting_preflight_done =
        !retry || peer->lighting_preflight_attempts >= LIGHTING_PREFLIGHT_ATTEMPTS;
    peer->lighting_preflight_due = k_uptime_get() + LIGHTING_PREFLIGHT_RETRY_MS;
}

/* The wake report, before the first lighting state. A refusal for security, or for want of the
 * half's boot nonce, is retried, up to LIGHTING_PREFLIGHT_ATTEMPTS reads in all. */
static bool wake_read_done(struct control_peer *peer, bool complete, uint8_t error) {
    struct defy_lighting_wake wake;
    if (!peer->live) {
        return false;
    }
    peer->lighting_wake_ready =
        complete && defy_lighting_wake_decode(peer->read_data, peer->read_length, &wake) == 0;
    if (!peer->lighting_wake_ready) {
        lighting_preflight_failed(peer, error == BT_ATT_ERR_UNLIKELY ||
                                            error == BT_ATT_ERR_AUTHENTICATION ||
                                            error == BT_ATT_ERR_INSUFFICIENT_ENCRYPTION);
    }
    return false;
}

/* The half's acknowledgement of the lighting state. A half whose command characteristic cannot
 * be read is not sent the state again. */
static bool ack_read_done(struct control_peer *peer, bool complete, uint8_t error) {
    struct defy_lighting_state state;
    if (complete && defy_lighting_control_decode(peer->read_data, peer->read_length,
                                                 DEFY_CONTROL_LIGHTING_ACK, &state) == 0) {
        defy_lighting_retry_acknowledged(&peer->lighting, &state);
        /* The half took the state, so it holds the groups the state names. */
        if (defy_lighting_equal(&peer->lighting.target, &state)) {
            peer->lighting_groups |= state.groups;
        }
    }
    if (error == BT_ATT_ERR_READ_NOT_PERMITTED || error == BT_ATT_ERR_INVALID_HANDLE) {
        defy_lighting_retry_stop(&peer->lighting);
    }
    return false;
}

/*
 * The side of the peer's half, for its groups: the one its wake report gave,
 * else the one its battery frames give, else DEFY_WIRED_SIDE_UNKNOWN. Called
 * with peer_lock held.
 */
static uint8_t lighting_side(const struct control_peer *peer) {
    uint8_t side;
    if (peer->lighting_side_known) {
        return peer->lighting_side;
    }
    if (IS_ENABLED(CONFIG_DEFY_BATTERY) &&
        !defy_battery_central_route_side(DEFY_LINK_BLE, peer->slot, peer->epoch, &side)) {
        return side;
    }
    return DEFY_WIRED_SIDE_UNKNOWN;
}

/*
 * Writes the groups in pending to the half on side, lowest index first. It
 * stops at the first group it cannot fetch or write, with that error.
 */
static int write_groups(struct bt_conn *conn, uint16_t handle, uint8_t side, uint16_t pending) {
    for (uint8_t index = 0; pending >> index; index++) {
        if (!(pending & BIT(index))) {
            continue;
        }
        struct defy_lighting_group group;
        uint8_t data[DEFY_LIGHTING_GROUP_SIZE];
        int err = side <= DEFY_WIRED_RIGHT ? defy_lighting_central_group(side, index, &group)
                                           : -ENODATA;
        if (!err) {
            err = defy_lighting_group_encode(&group, data);
        }
        if (!err) {
            err = bt_gatt_write_without_response(conn, handle, data, sizeof(data), false);
        }
        if (err) {
            return err;
        }
    }
    return 0;
}

/*
 * Writes the state after the groups it names that the half has not confirmed.
 * When a group cannot go, the state waits for the next attempt.
 */
static void lighting_send_work(struct k_work *work) {
    ARG_UNUSED(work);
    for (size_t i = 0; i < ARRAY_SIZE(control_peers); i++) {
        struct control_peer *peer = &control_peers[i];
        struct defy_lighting_state desired;
        bool available = defy_lighting_central_get(&desired);
        k_spinlock_key_t key = k_spin_lock(&peer_lock);
        bool send = !defy_hybrid_central_is_maintenance() && available && peer->conn &&
                    peer->live && peer->lighting_queued &&
                    peer->lighting_preflight_done && peer->command_handle &&
                    defy_lighting_retry_pending(&peer->lighting) &&
                    defy_lighting_equal(&peer->lighting.target, &desired);
        struct bt_conn *conn = send ? bt_conn_ref(peer->conn) : NULL;
        uint32_t epoch = peer->epoch;
        uint16_t handle = peer->command_handle;
        uint16_t groups = desired.groups & ~peer->lighting_groups;
        uint8_t side = send && groups ? lighting_side(peer) : DEFY_WIRED_SIDE_UNKNOWN;
        peer->lighting_queued = false;
        if (send) {
            defy_lighting_retry_sent(&peer->lighting, k_uptime_get());
        }
        k_spin_unlock(&peer_lock, key);
        if (!conn) {
            continue;
        }
        int err = write_groups(conn, handle, side, groups);
        if (!err) {
            uint8_t command[DEFY_LIGHTING_CONTROL_SIZE];
            defy_lighting_control_encode(&desired, DEFY_CONTROL_LIGHTING_SET, command);
            err = bt_gatt_write_without_response(conn, handle, command, sizeof(command), false);
        }
        key = k_spin_lock(&peer_lock);
        if (!err && peer->conn == conn && peer->epoch == epoch && peer->live &&
            defy_lighting_equal(&peer->lighting.target, &desired)) {
            peer->lighting_read_pending = true;
        }
        k_spin_unlock(&peer_lock, key);
        bt_conn_unref(conn);
        schedule_poll(K_MSEC(1));
    }
}

static K_WORK_DEFINE(lighting_send, lighting_send_work);

/*
 * Hands a wake report the peer read to the lighting, dropping peer_lock (*key)
 * meanwhile. False if the peer's link ended meanwhile.
 */
static bool forward_wake(struct control_peer *peer, struct bt_conn *conn,
                         k_spinlock_key_t *key) {
    if (!IS_ENABLED(CONFIG_DEFY_LIGHTING) || !peer->lighting_wake_ready) {
        return true;
    }
    struct defy_lighting_wake wake;
    int err = defy_lighting_wake_decode(peer->read_data, peer->read_length, &wake);
    uint32_t epoch = peer->epoch;
    peer->lighting_wake_ready = false;
    if (!err) {
        wake.age_ms = MIN((uint64_t)UINT32_MAX,
                          (uint64_t)wake.age_ms + MAX(0, k_uptime_get() - peer->started_at));
        k_spin_unlock(&peer_lock, *key);
        defy_lighting_central_wake(&wake);
        *key = k_spin_lock(&peer_lock);
    }
    if (peer->conn != conn || peer->epoch != epoch || !peer->live) {
        return false;
    }
    if (!err) {
        peer->lighting_side = wake.side;
        peer->lighting_side_known = true;
    }
    peer->lighting_preflight_done = true;
    return true;
}

/*
 * Queues the lighting state for the peer when it is due, and decides whether
 * the next read is of the lighting characteristic: the wake report before the
 * first state, then each acknowledgement. False while the wake read waits for
 * its retry.
 */
static bool schedule_lighting(struct control_peer *peer, int64_t *next, bool *read_lighting,
                              bool *preflight) {
    struct defy_lighting_state desired;
    if (!IS_ENABLED(CONFIG_DEFY_LIGHTING)) {
        *read_lighting = false;
        *preflight = false;
        return true;
    }
    if (defy_lighting_central_get(&desired)) {
        if (defy_lighting_retry_follow(&peer->lighting, &desired, k_uptime_get())) {
            peer->lighting_queued = false;
            peer->lighting_read_pending = false;
        }
        if (peer->lighting_preflight_done && peer->command_handle &&
            defy_lighting_retry_pending(&peer->lighting)) {
            *next = MIN(*next, MAX(peer->lighting.due, k_uptime_get() + RETRY_MS));
            if (!peer->lighting_queued && !peer->lighting_read_pending &&
                defy_lighting_retry_due(&peer->lighting, k_uptime_get())) {
                peer->lighting_queued = true;
                k_work_submit_to_queue(&control_work_queue, &lighting_send);
            }
        }
    }
    *preflight = peer->command_handle && !peer->lighting_preflight_done &&
                 peer->lighting_preflight_due <= k_uptime_get();
    if (!peer->busy && peer->command_handle && !peer->lighting_preflight_done) {
        *next = MIN(*next, MAX(peer->lighting_preflight_due, k_uptime_get() + 1));
        if (!*preflight) {
            return false;
        }
    }
    *read_lighting = peer->command_handle && (*preflight || peer->lighting_read_pending);
    return true;
}

static void prepare_lighting_read(struct control_peer *peer, bool preflight) {
    peer->lighting_read_pending = false;
    if (preflight) {
        peer->lighting_preflight_attempts++;
    }
    peer->read_done = preflight ? wake_read_done : ack_read_done;
    peer->read.single.handle = peer->command_handle;
}

static const struct bt_uuid_128 version_uuid = BT_UUID_INIT_128(DEFY_HALF_VERSION_UUID);

/*
 * A half with the version report has its characteristic right after the
 * command's, so the discovery looks there and stops. Firmware without it
 * gives another service's characteristic or the end of the discovery there.
 * Called with peer_lock held, for a characteristic other than the battery's
 * and the command's.
 */
static void discover_version(struct control_peer *peer,
                             const struct bt_gatt_chrc *characteristic) {
    if (!peer->command_handle || peer->version_checked) {
        return;
    }
    peer->version_checked = true;
    if (!bt_uuid_cmp(characteristic->uuid, &version_uuid.uuid)) {
        peer->version_handle = characteristic->value_handle;
        peer->version_pending = true;
        peer->version_due = k_uptime_get();
    }
}

/* Whether the version report's read is due. If it is not due yet, it puts when into *next. */
static bool version_due(const struct control_peer *peer, int64_t *next) {
    if (!peer->version_pending) {
        return false;
    }
    if (peer->version_due > k_uptime_get()) {
        *next = MIN(*next, peer->version_due);
        return false;
    }
    return true;
}

/* A read that failed, or ended during maintenance, goes again after RETRY_MS. */
static void version_retry(struct control_peer *peer) {
    peer->version_due = k_uptime_get() + RETRY_MS;
}

/*
 * The version report. It goes to the records under peer_lock, so that
 * defy_half_version_ble_down() for a disconnection always comes after it. A
 * record that does not decode is not read again, as the half would send the
 * same bytes.
 */
static bool version_read_done(struct control_peer *peer, bool complete, uint8_t error) {
    struct defy_half_version version;
    bool maintenance = defy_hybrid_central_is_maintenance();
    if (complete && !maintenance && peer->read_length == DEFY_HALF_VERSION_SIZE &&
        defy_half_version_decode(peer->read_data, &version)) {
        defy_half_version_received(DEFY_LINK_BLE, peer->slot, &version);
    }
    peer->version_pending = peer->live && (error || maintenance);
    version_retry(peer);
    return true;
}

static void prepare_version_read(struct control_peer *peer) {
    peer->read_done = version_read_done;
    peer->read.single.handle = peer->version_handle;
}

static int control_client_init(void) {
    const struct k_work_queue_config config = {.name = "defy_control"};

    k_work_queue_start(&control_work_queue, control_work_stack,
                       K_THREAD_STACK_SIZEOF(control_work_stack), CONTROL_PRIORITY, &config);
    return 0;
}

/* First among the application inits: the queue runs before ZMK's Bluetooth, whose callbacks
 * submit to it. */
SYS_INIT(control_client_init, APPLICATION, 0);

static uint8_t characteristic_discovered(struct bt_conn *conn, const struct bt_gatt_attr *attr,
                                         struct bt_gatt_discover_params *params) {
    struct control_peer *peer = CONTAINER_OF(params, struct control_peer, discovery);
    k_spinlock_key_t key = k_spin_lock(&peer_lock);
    if (peer->conn != conn || !peer->busy) {
        k_spin_unlock(&peer_lock, key);
        return BT_GATT_ITER_STOP;
    }
    if (peer->live && attr && attr->user_data) {
        const struct bt_gatt_chrc *characteristic = attr->user_data;
        if (!bt_uuid_cmp(characteristic->uuid, &battery_uuid.uuid)) {
            peer->battery_handle = characteristic->value_handle;
        } else if (!bt_uuid_cmp(characteristic->uuid, &command_uuid.uuid)) {
            peer->command_handle = characteristic->value_handle;
        } else if (IS_ENABLED(CONFIG_DEFY_HALF_VERSION)) {
            discover_version(peer, characteristic);
        }
        if (!peer->battery_handle || !peer->command_handle ||
            (IS_ENABLED(CONFIG_DEFY_HALF_VERSION) && !peer->version_checked)) {
            k_spin_unlock(&peer_lock, key);
            return BT_GATT_ITER_CONTINUE;
        }
    }
    peer->busy = false;
    peer->due = k_uptime_get() + (peer->battery_handle ? 1 : BATTERY_POLL_MS);
    k_spin_unlock(&peer_lock, key);
    schedule_poll(K_MSEC(1));
    return BT_GATT_ITER_STOP;
}

void defy_control_ble_connected(struct bt_conn *conn, uint8_t slot, uint32_t epoch) {
    if (!conn || slot >= BLE_SLOTS || !epoch) {
        return;
    }
    struct bt_conn *reference = bt_conn_ref(conn);
    k_spinlock_key_t key = k_spin_lock(&peer_lock);
    struct control_peer *free_peer = NULL;
    for (size_t i = 0; i < ARRAY_SIZE(control_peers); i++) {
        struct control_peer *peer = &control_peers[i];
        if (peer->conn == conn && peer->epoch == epoch && peer->live) {
            k_spin_unlock(&peer_lock, key);
            bt_conn_unref(reference);
            return;
        }
        if (!peer->conn) {
            free_peer = peer;
        }
    }
    if (free_peer) {
        *free_peer = (struct control_peer){.conn = reference, .slot = slot, .epoch = epoch,
                                           .live = true,
                                           .due = k_uptime_get() + CONNECT_DELAY_MS};
    }
    k_spin_unlock(&peer_lock, key);
    if (!free_peer) {
        bt_conn_unref(reference);
        LOG_WRN("Control BLE client full");
        return;
    }
    if (IS_ENABLED(CONFIG_DEFY_BATTERY)) {
        defy_battery_central_route_live(DEFY_LINK_BLE, slot, epoch, DEFY_WIRED_SIDE_UNKNOWN);
    }
    schedule_poll(K_MSEC(CONNECT_DELAY_MS));
}

void defy_control_ble_disconnected(struct bt_conn *conn, uint8_t slot, uint32_t epoch) {
    k_spinlock_key_t key = k_spin_lock(&peer_lock);
    for (size_t i = 0; i < ARRAY_SIZE(control_peers); i++) {
        struct control_peer *peer = &control_peers[i];
        if (peer->conn == conn && peer->slot == slot && peer->epoch == epoch) {
            peer->live = false;
            peer->show_requested = false;
            peer->show_queued = false;
            peer->feedback_pending = false;
            peer->lighting_queued = false;
            peer->lighting_read_pending = false;
        }
    }
    k_spin_unlock(&peer_lock, key);
    if (IS_ENABLED(CONFIG_DEFY_BATTERY)) {
        defy_battery_central_disconnect(DEFY_LINK_BLE, slot, epoch);
    }
    if (IS_ENABLED(CONFIG_DEFY_HALF_VERSION)) {
        defy_half_version_ble_down(slot);
    }
    schedule_poll(K_NO_WAIT);
}

static void battery_show_work(struct k_work *work);
static K_WORK_DEFINE(battery_show_send, battery_show_work);

void defy_control_ble_battery_show(uint8_t side_mask) {
    if (!IS_ENABLED(CONFIG_DEFY_BATTERY) || defy_hybrid_central_is_maintenance()) {
        return;
    }
    uint8_t queued = 0;
    k_spinlock_key_t key = k_spin_lock(&peer_lock);
    for (size_t i = 0; i < ARRAY_SIZE(control_peers); i++) {
        struct control_peer *peer = &control_peers[i];
        uint8_t side;
        if (!peer->conn || !peer->live ||
            defy_battery_central_route_side(DEFY_LINK_BLE, peer->slot, peer->epoch, &side) ||
            !(side_mask & BIT(side))) {
            continue;
        }
        peer->show_side = side;
        peer->show_requested = true;
        peer->show_deadline = k_uptime_get() + DEFY_CONTROL_TIMEOUT_MS;
        queued |= BIT(side);
    }
    k_spin_unlock(&peer_lock, key);
    if (queued) {
        schedule_poll(K_NO_WAIT);
    }
    if ((side_mask & BOTH_SIDES) != queued) {
        LOG_WRN("Battery display unavailable sides=%u",
                (unsigned int)((side_mask & BOTH_SIDES) & ~queued));
    }
}

static void battery_show_work(struct k_work *work) {
    ARG_UNUSED(work);
    const uint8_t command[DEFY_CONTROL_BATTERY_SHOW_SIZE] = {
        DEFY_CONTROL_BATTERY_SHOW_VERSION, DEFY_CONTROL_BATTERY_SHOW,
    };
    for (size_t i = 0; i < ARRAY_SIZE(control_peers); i++) {
        struct control_peer *peer = &control_peers[i];
        k_spinlock_key_t key = k_spin_lock(&peer_lock);
        uint8_t side;
        bool send = !defy_hybrid_central_is_maintenance() && peer->conn && peer->live &&
                    peer->show_queued && peer->show_requested &&
                    peer->command_handle && k_uptime_get() < peer->show_deadline &&
                    defy_battery_central_route_side(DEFY_LINK_BLE, peer->slot,
                                                    peer->epoch, &side) == 0 &&
                    side == peer->show_side;
        struct bt_conn *conn = send ? bt_conn_ref(peer->conn) : NULL;
        uint16_t handle = peer->command_handle;
        if (peer->show_queued) {
            peer->show_requested = false;
            peer->show_queued = false;
        }
        k_spin_unlock(&peer_lock, key);
        if (conn) {
            int err = bt_gatt_write_without_response(conn, handle, command, sizeof(command),
                                                     false);
            if (err) {
                LOG_WRN("Battery display write error=%d", err);
            }
            bt_conn_unref(conn);
        }
    }
}

/* Queues the battery display for the peer until it is sent, or drops it once too late. */
static void schedule_show(struct control_peer *peer, int64_t *next) {
    if (!IS_ENABLED(CONFIG_DEFY_BATTERY) || !peer->show_requested) {
        return;
    }
    if (k_uptime_get() >= peer->show_deadline) {
        peer->show_requested = false;
        peer->show_queued = false;
    } else {
        *next = MIN(*next, peer->show_deadline);
        if (peer->command_handle && !peer->show_queued) {
            peer->show_queued = true;
            k_work_submit_to_queue(&control_work_queue, &battery_show_send);
        }
    }
}

/*
 * The poll's steps for one peer. Each runs with peer_lock held as *key, and
 * may drop it to call out; *next is when the poll runs again.
 */

/* A peer whose link ended: cancels its request, or releases the peer once
 * none runs. Unlocks. */
static void release_peer(struct control_peer *peer, k_spinlock_key_t key, int64_t *next) {
    struct bt_conn *conn = peer->conn;
    bool busy = peer->busy;
    bool release = !busy;
    void *params = peer->discovering ? (void *)&peer->discovery : (void *)&peer->read;
    if (release) {
        memset(peer, 0, sizeof(*peer));
    }
    k_spin_unlock(&peer_lock, key);
    if (busy) {
        bt_gatt_cancel(conn, params);
        *next = MIN(*next, k_uptime_get() + RETRY_MS);
    } else if (release) {
        bt_conn_unref(conn);
    }
}

/* During maintenance: drops the commands not yet sent and asks again later. */
static void hold_peer(struct control_peer *peer, int64_t *next) {
    peer->show_requested = false;
    peer->show_queued = false;
    peer->lighting_queued = false;
    *next = MIN(*next, k_uptime_get() + RETRY_MS);
}

/* Passes a battery frame the peer read to the battery central, aged by the time since its read
 * started. */
static void forward_frame(struct control_peer *peer, k_spinlock_key_t *key) {
    if (!IS_ENABLED(CONFIG_DEFY_BATTERY) || !peer->battery_ready) {
        return;
    }
    uint8_t frame[DEFY_BATTERY_FRAME_SIZE];
    memcpy(frame, peer->read_data, sizeof(frame));
    uint32_t elapsed = MIN((uint64_t)UINT32_MAX,
                           (uint64_t)MAX(0, k_uptime_get() - peer->started_at));
    uint8_t slot = peer->slot;
    uint32_t epoch = peer->epoch;
    peer->battery_ready = false;
    k_spin_unlock(&peer_lock, *key);
    int err = defy_battery_central_accept(DEFY_LINK_BLE, slot, epoch, frame, sizeof(frame),
                                          elapsed);
    if (err == -EPROTO || err == -EADDRINUSE) {
        defy_hybrid_central_ble_reject(slot, epoch);
    }
    if (err) {
        LOG_WRN("Battery BLE sample slot=%u error=%d", slot, err);
    }
    *key = k_spin_lock(&peer_lock);
}

/*
 * Whether the discovery is still to run: until it found the battery frame's
 * characteristic, with battery support, and otherwise the command's. It looks
 * for both either way.
 */
static bool discovery_needed(const struct control_peer *peer) {
    return IS_ENABLED(CONFIG_DEFY_BATTERY) ? !peer->battery_handle : !peer->command_handle;
}

/*
 * Starts the peer's next GATT request once it is due: the characteristics'
 * discovery, then reads of the battery frame with battery support, or of the
 * lighting characteristic when read_lighting. The version report's read goes
 * when it is due and neither the discovery nor a battery read is. Unlocks.
 */
static void start_request(struct control_peer *peer, struct bt_conn *conn, k_spinlock_key_t key,
                          bool read_lighting, bool preflight, int64_t *next) {
    bool discover = !read_lighting && discovery_needed(peer);
    bool read_version = IS_ENABLED(CONFIG_DEFY_HALF_VERSION) && !read_lighting &&
                        version_due(peer, next) &&
                        (peer->due > k_uptime_get() ||
                         (!discover && !IS_ENABLED(CONFIG_DEFY_BATTERY)));
    if (peer->busy || (!read_lighting && !read_version && peer->due > k_uptime_get())) {
        *next = MIN(*next, peer->busy ? k_uptime_get() + RETRY_MS : peer->due);
        k_spin_unlock(&peer_lock, key);
        return;
    }
    if (!read_lighting && !discover && !read_version && !IS_ENABLED(CONFIG_DEFY_BATTERY)) {
        k_spin_unlock(&peer_lock, key);
        return;
    }
    discover = discover && !read_version;
    peer->busy = true;
    peer->discovering = discover;
    if (discover) {
        peer->discovery = (struct bt_gatt_discover_params){
            .uuid = NULL, .func = characteristic_discovered,
            .start_handle = BT_ATT_FIRST_ATTRIBUTE_HANDLE,
            .end_handle = BT_ATT_LAST_ATTRIBUTE_HANDLE,
            .type = BT_GATT_DISCOVER_CHARACTERISTIC,
        };
    } else {
        peer->read_length = 0;
        peer->started_at = k_uptime_get();
        peer->read = (struct bt_gatt_read_params){
            .func = read_chunk, .handle_count = 1, .single.handle = peer->battery_handle,
        };
        peer->read_done = battery_read_done;
        if (IS_ENABLED(CONFIG_DEFY_LIGHTING) && read_lighting) {
            prepare_lighting_read(peer, preflight);
        } else if (read_version) {
            prepare_version_read(peer);
        }
    }
    k_spin_unlock(&peer_lock, key);
    int err = discover ? bt_gatt_discover(conn, &peer->discovery)
                       : bt_gatt_read(conn, &peer->read);
    key = k_spin_lock(&peer_lock);
    if (err) {
        peer->busy = false;
        if (read_version) {
            version_retry(peer);
        } else {
            peer->due = k_uptime_get() + RETRY_MS;
        }
        if (!discover && read_lighting && preflight) {
            lighting_preflight_failed(peer, true);
        }
    }
    *next = MIN(*next, k_uptime_get() + RETRY_MS);
    k_spin_unlock(&peer_lock, key);
    if (err) {
        LOG_WRN("Control BLE request error=%d", err);
    }
}

/* One peer's turn in the poll; whether it holds a link. */
static bool poll_peer(struct control_peer *peer, int64_t *next) {
    k_spinlock_key_t key = k_spin_lock(&peer_lock);
    struct bt_conn *conn = peer->conn;
    if (!conn) {
        k_spin_unlock(&peer_lock, key);
        return false;
    }
    if (!peer->live) {
        release_peer(peer, key, next);
        return true;
    }
    if (defy_hybrid_central_is_maintenance()) {
        hold_peer(peer, next);
        k_spin_unlock(&peer_lock, key);
        return true;
    }
    if (!forward_wake(peer, conn, &key)) {
        k_spin_unlock(&peer_lock, key);
        return true;
    }
    forward_frame(peer, &key);
    schedule_show(peer, next);

    bool read_lighting;
    bool preflight;
    if (!schedule_lighting(peer, next, &read_lighting, &preflight)) {
        k_spin_unlock(&peer_lock, key);
        return true;
    }
    start_request(peer, conn, key, read_lighting, preflight, next);
    return true;
}

static void control_poll_work(struct k_work *work) {
    ARG_UNUSED(work);
    int64_t next = k_uptime_get() + BATTERY_POLL_MS;
    bool pending = false;
    for (size_t i = 0; i < ARRAY_SIZE(control_peers); i++) {
        pending |= poll_peer(&control_peers[i], &next);
    }
    if (pending) {
        schedule_poll(K_MSEC(MAX(1, next - k_uptime_get())));
    }
}
