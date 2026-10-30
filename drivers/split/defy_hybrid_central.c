/* SPDX-License-Identifier: MIT */

/*
 * The Neuron's split transport, the one ZMK selects. It takes key snapshots
 * from the cable central and from ZMK's Bluetooth central, picks a link for
 * each half and publishes that half's key changes to ZMK.
 */

#include <errno.h>
#include <string.h>

#include <zephyr/init.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/atomic.h>

#include <zmk/split/transport/central.h>

#include <dygma/defy_battery_central.h>
#include <dygma/defy_control.h>
#include <dygma/defy_hybrid.h>
#include <dygma/defy_key_trace.h>
#include <dygma/defy_neuron_status.h>
#include <dygma/defy_split_central.h>
#include <dygma/defy_usb_report_recovery.h>

#include "defy_split_internal.h"

LOG_MODULE_REGISTER(defy_hybrid_central, CONFIG_ZMK_LOG_LEVEL);

BUILD_ASSERT(CONFIG_ZMK_SPLIT_BLE_CENTRAL_PERIPHERALS == DEFY_WIRED_SIDES,
             "one Bluetooth split slot per half");

/* The period of the Bluetooth expiry and health checks. */
#define SERVICE_INTERVAL_MS 100

struct hybrid_snapshot {
    uint32_t epoch;
    uint32_t revision;
    int64_t received;
    /* enum defy_link. The source is the side on the cable, the split slot over Bluetooth. */
    uint8_t link;
    uint8_t source;
    uint8_t side;
    bool connected;
    bool rejected;
    uint8_t bitmap[DEFY_WIRED_BITMAP_SIZE];
#if IS_ENABLED(CONFIG_DEFY_KEY_TRACE)
    /* Enqueue order, for the key trace. It fits the struct's padding. */
    uint16_t trace_order;
#endif
};

BUILD_ASSERT(sizeof(struct hybrid_snapshot) == 32,
             "trace_order must not grow the queue's entries");

struct hybrid_ble_link {
    uint32_t epoch;
    uint32_t revision;
    int64_t received;
    uint8_t side;
    bool connected;
    bool rejected;
    uint8_t bitmap[DEFY_WIRED_BITMAP_SIZE];
};

struct hybrid_side {
    /* The cable's session and the keys it last reported. */
    uint32_t wired_session;
    bool wired_connected;
    uint8_t wired_bitmap[DEFY_WIRED_BITMAP_SIZE];
    uint8_t output[DEFY_WIRED_BITMAP_SIZE];
    bool await_ble_snapshot;
    uint32_t ble_revision_at_loss;
    int64_t handoff_deadline;
};

static struct hybrid_side sides[DEFY_WIRED_SIDES];
static struct hybrid_ble_link ble_links[CONFIG_ZMK_SPLIT_BLE_CENTRAL_PERIPHERALS];
/* The side of each Bluetooth slot's half while it is the side's link, for ZMK's events and
 * commands. */
static atomic_t slot_sides[CONFIG_ZMK_SPLIT_BLE_CENTRAL_PERIPHERALS] = {
    ATOMIC_INIT(DEFY_WIRED_SIDE_UNKNOWN), ATOMIC_INIT(DEFY_WIRED_SIDE_UNKNOWN)};
static struct hybrid_snapshot ble_latest[CONFIG_ZMK_SPLIT_BLE_CENTRAL_PERIPHERALS];
static struct k_spinlock ble_latest_lock;
static uint32_t ble_revision;
static atomic_t enabled;
static atomic_t maintenance;
static atomic_t maintenance_generation;
static atomic_t connected_sides;
static zmk_split_transport_central_status_changed_cb_t status_callback;
static bool wired_started;
static bool ble_started;
static K_MUTEX_DEFINE(maintenance_lock);
static K_SEM_DEFINE(maintenance_ready, 0, 1);

K_MSGQ_DEFINE(hybrid_snapshots, sizeof(struct hybrid_snapshot), CONFIG_DEFY_HYBRID_QUEUE_SIZE, 8);

static void service_work_cb(struct k_work *work);
static K_WORK_DEFINE(service_work, service_work_cb);
static void timeout_work_cb(struct k_work *work);
static K_WORK_DELAYABLE_DEFINE(timeout_work, timeout_work_cb);

bool defy_hybrid_central_is_maintenance(void) { return atomic_get(&maintenance) != 0; }

#if IS_ENABLED(CONFIG_DEFY_KEY_TRACE)
static uint16_t trace_order;
/* The order of the snapshot being applied, for the position events it causes. */
static uint16_t trace_applying;

/* Called with ble_latest_lock held, so the order is the queue's. */
static void trace_enqueue(struct hybrid_snapshot *snapshot) {
    if (++trace_order == 0) {
        trace_order = 1;
    }
    snapshot->trace_order = trace_order;
}

struct traced_link {
    bool connected;
    uint8_t bitmap[DEFY_WIRED_BITMAP_SIZE];
};

static void trace_enqueued(const struct hybrid_snapshot *snapshot, int err) {
    /*
     * Idle halves keep resending unchanged snapshots. Only snapshots that
     * change keys or the connection are traced, so typing stays in the ring.
     * traced[link][side on the cable, slot over Bluetooth] holds the last one.
     */
    static struct traced_link traced[2][DEFY_WIRED_SIDES];
    bool ble = snapshot->link == DEFY_LINK_BLE;
    struct traced_link *last =
        &traced[ble][MIN(ble ? snapshot->source : snapshot->side, DEFY_WIRED_SIDES - 1)];
    if (!err && last->connected == snapshot->connected &&
        !memcmp(last->bitmap, snapshot->bitmap, DEFY_WIRED_BITMAP_SIZE)) {
        return;
    }
    last->connected = snapshot->connected;
    memcpy(last->bitmap, snapshot->bitmap, DEFY_WIRED_BITMAP_SIZE);
    uint8_t flags = MIN(snapshot->side, DEFY_WIRED_SIDE_UNKNOWN) |
                    (ble ? DEFY_KEY_TRACE_BLE : 0) |
                    (ble && snapshot->source ? DEFY_KEY_TRACE_SOURCE : 0) |
                    (snapshot->connected ? DEFY_KEY_TRACE_ACTIVE : 0) |
                    (err ? DEFY_KEY_TRACE_DROPPED : 0);
    defy_key_trace_add(DEFY_KEY_TRACE_SNAPSHOT, flags, snapshot->trace_order,
                       (uint32_t)(snapshot->received * USEC_PER_MSEC));
}

static void trace_apply(const struct hybrid_snapshot *snapshot) {
    trace_applying = snapshot ? snapshot->trace_order : 0;
}

static void trace_position(uint8_t side, uint8_t position, bool pressed) {
    defy_key_trace_add(DEFY_KEY_TRACE_POSITION, side | (pressed ? DEFY_KEY_TRACE_ACTIVE : 0),
                       position, trace_applying);
}
#else
static inline void trace_enqueue(struct hybrid_snapshot *snapshot) { ARG_UNUSED(snapshot); }
static inline void trace_enqueued(const struct hybrid_snapshot *snapshot, int err) {
    ARG_UNUSED(snapshot);
    ARG_UNUSED(err);
}
static inline void trace_apply(const struct hybrid_snapshot *snapshot) { ARG_UNUSED(snapshot); }
static inline void trace_position(uint8_t side, uint8_t position, bool pressed) {
    ARG_UNUSED(side);
    ARG_UNUSED(position);
    ARG_UNUSED(pressed);
}
#endif

static int ble_slot_for_side(uint8_t side) {
    for (size_t i = 0; i < ARRAY_SIZE(ble_links); i++) {
        if (ble_links[i].connected && ble_links[i].side == side) {
            return i;
        }
    }
    return -ENOTCONN;
}

static void publish_bitmap(uint8_t side, const uint8_t *bitmap) {
    /* Releases, then presses: a snapshot does not keep the order of its changes. */
    for (int pressed = 0; pressed <= 1; pressed++) {
        for (uint8_t position = 0; position < DEFY_WIRED_KEY_COUNT; position++) {
            uint8_t mask = BIT(position % 8);
            bool previous = (sides[side].output[position / 8] & mask) != 0;
            bool current = bitmap && (bitmap[position / 8] & mask) != 0;
            if (previous == current || current != pressed) {
                continue;
            }
            struct zmk_split_transport_peripheral_event event = {
                .type = ZMK_SPLIT_TRANSPORT_PERIPHERAL_EVENT_TYPE_KEY_POSITION_EVENT,
                .data.key_position_event = {.position = position, .pressed = current},
            };
            trace_position(side, position, current);
            int err = zmk_split_transport_central_peripheral_event_handler(
                &defy_hybrid_central, side, event);
            if (err < 0) {
                LOG_WRN("Hybrid key event failed: side %u position %u error %d", side,
                        position, err);
            }
            WRITE_BIT(sides[side].output[position / 8], position % 8, current);
        }
    }
}

static void publish_selected(uint8_t side) {
    if (!atomic_get(&enabled) || defy_hybrid_central_is_maintenance()) {
        publish_bitmap(side, NULL);
    } else if (sides[side].wired_connected) {
        publish_bitmap(side, sides[side].wired_bitmap);
    } else {
        int slot = ble_slot_for_side(side);
        if (slot >= 0 && sides[side].await_ble_snapshot &&
            k_uptime_get() < sides[side].handoff_deadline) {
            return;
        }
        publish_bitmap(side, slot < 0 || sides[side].await_ble_snapshot
                                 ? NULL
                                 : ble_links[slot].bitmap);
    }
}

static void apply_wired(const struct hybrid_snapshot *snapshot) {
    uint8_t side = snapshot->side;
    struct hybrid_side *state = &sides[side];
    if (!snapshot->connected) {
        if (!state->wired_connected || state->wired_session != snapshot->epoch) {
            return;
        }
        state->wired_connected = false;
        memset(state->wired_bitmap, 0, sizeof(state->wired_bitmap));
        state->ble_revision_at_loss = snapshot->revision;
        state->await_ble_snapshot = true;
        state->handoff_deadline = snapshot->received + CONFIG_DEFY_HYBRID_HANDOFF_GRACE_MS;
        publish_selected(side);
        return;
    }
    state->wired_connected = true;
    state->wired_session = snapshot->epoch;
    state->await_ble_snapshot = false;
    memcpy(state->wired_bitmap, snapshot->bitmap, sizeof(state->wired_bitmap));
    if (atomic_get(&enabled) && !defy_hybrid_central_is_maintenance()) {
        defy_usb_input_snapshot(side, snapshot->bitmap, snapshot->received);
    }
    publish_selected(side);
}

static void expire_ble_slot(uint8_t slot, bool forget_identity) {
    uint8_t previous_side = ble_links[slot].side;
    ble_links[slot].connected = false;
    if (forget_identity) {
        ble_links[slot].side = DEFY_WIRED_SIDE_UNKNOWN;
    }
    memset(ble_links[slot].bitmap, 0, sizeof(ble_links[slot].bitmap));
    atomic_set(&slot_sides[slot], DEFY_WIRED_SIDE_UNKNOWN);
    if (previous_side < DEFY_WIRED_SIDES) {
        publish_selected(previous_side);
    }
}

/* Drops the half on the snapshot's slot, which the Bluetooth watch then disconnects. */
static void reject_ble_slot(const struct hybrid_snapshot *snapshot) {
    ble_links[snapshot->source].rejected = true;
    expire_ble_slot(snapshot->source, false);
    defy_hybrid_ble_peer_rejected(snapshot->source, snapshot->epoch);
}

/*
 * The side of the half on the snapshot's slot: the one its battery frame
 * names, else the one its keys show, else the one the slot already has.
 * -ENODATA while nothing names it, and -EPROTO when the half claims a side
 * that its keys, its slot or the other half contradict.
 */
static int resolve_ble_side(const struct hybrid_snapshot *snapshot, uint8_t *resolved) {
    const struct hybrid_ble_link *link = &ble_links[snapshot->source];
    uint8_t side = snapshot->side;
    uint8_t battery_side;
    if (IS_ENABLED(CONFIG_DEFY_BATTERY) &&
        !defy_battery_central_route_side(DEFY_LINK_BLE, snapshot->source, snapshot->epoch,
                                         &battery_side)) {
        if (side != DEFY_WIRED_SIDE_UNKNOWN && side != battery_side) {
            return -EPROTO;
        }
        side = battery_side;
    }
    if (side == DEFY_WIRED_SIDE_UNKNOWN) {
        side = link->side;
    }
    if (side == DEFY_WIRED_SIDE_UNKNOWN) {
        return -ENODATA;
    }
    if (link->side != DEFY_WIRED_SIDE_UNKNOWN && link->side != side) {
        return -EPROTO;
    }
    int incumbent = ble_slot_for_side(side);
    if (incumbent >= 0 && incumbent != snapshot->source) {
        return -EPROTO;
    }
    if (IS_ENABLED(CONFIG_DEFY_BATTERY) &&
        defy_battery_central_route_identify(DEFY_LINK_BLE, snapshot->source, snapshot->epoch,
                                            side) == -EPROTO) {
        return -EPROTO;
    }
    *resolved = side;
    return 0;
}

static void apply_ble(const struct hybrid_snapshot *snapshot) {
    struct hybrid_ble_link *link = &ble_links[snapshot->source];
    int32_t revision_delta = snapshot->revision - link->revision;
    if (revision_delta < 0 ||
        (!revision_delta && (link->side != DEFY_WIRED_SIDE_UNKNOWN || link->rejected)) ||
        (link->epoch && (int32_t)(snapshot->epoch - link->epoch) < 0)) {
        return;
    }
    if (link->epoch != snapshot->epoch) {
        expire_ble_slot(snapshot->source, true);
        link->rejected = false;
    }
    if (!snapshot->connected) {
        expire_ble_slot(snapshot->source, true);
    }
    link->epoch = snapshot->epoch;
    link->revision = snapshot->revision;
    if (link->rejected || snapshot->rejected) {
        reject_ble_slot(snapshot);
        return;
    }
    if (!snapshot->connected ||
        k_uptime_get() - snapshot->received >= CONFIG_DEFY_HYBRID_BLE_TIMEOUT_MS) {
        return;
    }
    uint8_t side;
    int err = resolve_ble_side(snapshot, &side);
    if (err == -ENODATA) {
        return;
    }
    if (err) {
        reject_ble_slot(snapshot);
        return;
    }
    link->connected = true;
    link->side = side;
    link->received = snapshot->received;
    atomic_set(&slot_sides[snapshot->source], side);
    memcpy(link->bitmap, snapshot->bitmap, sizeof(link->bitmap));
    if (sides[side].await_ble_snapshot &&
        (int32_t)(snapshot->revision - sides[side].ble_revision_at_loss) > 0) {
        sides[side].await_ble_snapshot = false;
    }
    if (atomic_get(&enabled) && !defy_hybrid_central_is_maintenance() &&
        !sides[side].wired_connected && !sides[side].await_ble_snapshot) {
        defy_usb_input_snapshot(side, snapshot->bitmap, snapshot->received);
    }
    publish_selected(side);
}

static struct zmk_split_transport_status get_status(void) {
    atomic_val_t mask = atomic_get(&connected_sides);
    return (struct zmk_split_transport_status){
        .available = true,
        .enabled = atomic_get(&enabled) != 0,
        .connections = mask == BIT_MASK(DEFY_WIRED_SIDES)
                           ? ZMK_SPLIT_TRANSPORT_CONNECTIONS_STATUS_ALL_CONNECTED
                       : mask ? ZMK_SPLIT_TRANSPORT_CONNECTIONS_STATUS_SOME_CONNECTED
                              : ZMK_SPLIT_TRANSPORT_CONNECTIONS_STATUS_DISCONNECTED,
    };
}

static void update_status(void) {
    atomic_val_t mask = 0;
    for (uint8_t side = 0; side < ARRAY_SIZE(sides); side++) {
        if (sides[side].wired_connected || ble_slot_for_side(side) >= 0) {
            mask |= BIT(side);
        }
    }
    if (atomic_set(&connected_sides, mask) != mask && status_callback) {
        status_callback(&defy_hybrid_central, get_status());
    }
}

static int raw_status_changed(const struct zmk_split_transport_central *transport,
                              struct zmk_split_transport_status status) {
    ARG_UNUSED(transport);
    ARG_UNUSED(status);
    k_work_submit(&service_work);
    return 0;
}

static void start_transport(const struct zmk_split_transport_central *transport, bool *started) {
    if (*started || !transport->api->get_status().available) {
        return;
    }
    int err = transport->api->set_enabled(true);
    if (err == 0) {
        *started = true;
    } else {
        LOG_WRN("Hybrid split transport start failed: %d", err);
    }
}

static void process_snapshots(void) {
    struct hybrid_snapshot snapshot;
    while (k_msgq_get(&hybrid_snapshots, &snapshot, K_NO_WAIT) == 0) {
        trace_apply(&snapshot);
        if (snapshot.link == DEFY_LINK_WIRED) {
            apply_wired(&snapshot);
        } else {
            apply_ble(&snapshot);
        }
    }
    /*
     * Each slot's latest snapshot goes in again after the queue. enqueue_ble_snapshot()
     * keeps it even when the queue is full, so this applies a snapshot the queue dropped.
     * And while the slot's side is unknown, apply_ble() takes the same snapshot again, so
     * the half's keys reach ZMK once its battery frame names its side.
     */
    for (uint8_t slot = 0; slot < ARRAY_SIZE(ble_latest); slot++) {
        k_spinlock_key_t key = k_spin_lock(&ble_latest_lock);
        snapshot = ble_latest[slot];
        k_spin_unlock(&ble_latest_lock, key);
        if (snapshot.revision != 0) {
            trace_apply(&snapshot);
            apply_ble(&snapshot);
        }
        trace_apply(NULL);
        if (ble_links[slot].connected &&
            k_uptime_get() - ble_links[slot].received >= CONFIG_DEFY_HYBRID_BLE_TIMEOUT_MS) {
            expire_ble_slot(slot, false);
        }
    }
    bool active = atomic_get(&enabled);

    for (uint8_t side = 0; side < ARRAY_SIZE(sides); side++) {
        if (!active ||
            (sides[side].await_ble_snapshot && k_uptime_get() >= sides[side].handoff_deadline)) {
            publish_bitmap(side, NULL);
        }
    }
    update_status();
}

static void service_work_cb(struct k_work *work) {
    ARG_UNUSED(work);
    if (defy_hybrid_central_is_maintenance()) {
        k_spinlock_key_t key = k_spin_lock(&ble_latest_lock);
        k_msgq_purge(&hybrid_snapshots);
        memset(ble_latest, 0, sizeof(ble_latest));
        k_spin_unlock(&ble_latest_lock, key);
        for (uint8_t side = 0; side < ARRAY_SIZE(sides); side++) {
            publish_bitmap(side, NULL);
        }
        memset(sides, 0, sizeof(sides));
        memset(ble_links, 0, sizeof(ble_links));
        for (size_t slot = 0; slot < ARRAY_SIZE(slot_sides); slot++) {
            atomic_set(&slot_sides[slot], DEFY_WIRED_SIDE_UNKNOWN);
        }
        update_status();
        k_sem_give(&maintenance_ready);
        return;
    }
    if (atomic_get(&enabled)) {
        start_transport(&defy_wired_central, &wired_started);
        start_transport(&bt_central, &ble_started);
    }
    process_snapshots();
    int64_t now = k_uptime_get();
    defy_hybrid_ble_check_health(now);
    int64_t delay = SERVICE_INTERVAL_MS;
    for (uint8_t side = 0; side < ARRAY_SIZE(sides); side++) {
        if (sides[side].await_ble_snapshot && sides[side].handoff_deadline > now) {
            delay = MIN(delay, sides[side].handoff_deadline - now);
        }
    }
    k_work_reschedule(&timeout_work, K_MSEC(delay));
}

static void timeout_work_cb(struct k_work *work) {
    ARG_UNUSED(work);
    k_work_submit(&service_work);
}

int defy_hybrid_central_wired_update(uint8_t side, uint32_t session, bool connected,
                                     const uint8_t bitmap[DEFY_WIRED_BITMAP_SIZE]) {
    atomic_val_t generation = atomic_get(&maintenance_generation);
    if (side > DEFY_WIRED_RIGHT || !session ||
        (connected && !defy_wired_bitmap_valid(side, bitmap))) {
        return -EINVAL;
    }
    struct hybrid_snapshot snapshot = {
        .link = DEFY_LINK_WIRED,
        .source = side,
        .side = side,
        .epoch = session,
        .connected = connected,
        .received = k_uptime_get(),
    };
    if (connected) {
        memcpy(snapshot.bitmap, bitmap, sizeof(snapshot.bitmap));
    }
    k_spinlock_key_t key = k_spin_lock(&ble_latest_lock);
    if (defy_hybrid_central_is_maintenance() ||
        generation != atomic_get(&maintenance_generation)) {
        k_spin_unlock(&ble_latest_lock, key);
        return 0;
    }
    if (!connected) {
        snapshot.revision = ble_revision;
    }
    trace_enqueue(&snapshot);
    int err = k_msgq_put(&hybrid_snapshots, &snapshot, K_NO_WAIT);
    trace_enqueued(&snapshot, err);
    k_spin_unlock(&ble_latest_lock, key);
    if (err == 0) {
        k_work_submit(&service_work);
    }
    return err;
}

static int enqueue_ble_snapshot(struct hybrid_snapshot *snapshot, atomic_val_t generation) {
    k_spinlock_key_t key = k_spin_lock(&ble_latest_lock);
    if (defy_hybrid_central_is_maintenance() ||
        generation != atomic_get(&maintenance_generation)) {
        k_spin_unlock(&ble_latest_lock, key);
        return 0;
    }
    const struct hybrid_snapshot *latest = &ble_latest[snapshot->source];
    if (latest->epoch && (int32_t)(snapshot->epoch - latest->epoch) < 0) {
        k_spin_unlock(&ble_latest_lock, key);
        return 0;
    }
    snapshot->rejected |= latest->epoch == snapshot->epoch && latest->rejected;
    snapshot->revision = ++ble_revision;
    if (snapshot->revision == 0) {
        snapshot->revision = ++ble_revision;
    }
    trace_enqueue(snapshot);
    ble_latest[snapshot->source] = *snapshot;
    int err = k_msgq_put(&hybrid_snapshots, snapshot, K_NO_WAIT);
    trace_enqueued(snapshot, err);
    k_spin_unlock(&ble_latest_lock, key);
    k_work_submit(&service_work);
    return err;
}

void defy_hybrid_central_ble_reject(uint8_t slot, uint32_t epoch) {
    if (slot >= ARRAY_SIZE(ble_links) || !epoch) {
        return;
    }
    struct hybrid_snapshot snapshot = {
        .link = DEFY_LINK_BLE, .source = slot, .epoch = epoch, .rejected = true,
        .side = DEFY_WIRED_SIDE_UNKNOWN,
    };
    enqueue_ble_snapshot(&snapshot, atomic_get(&maintenance_generation));
}

int defy_hybrid_central_ble_update(uint8_t slot, uint32_t epoch, bool connected,
                                   const uint8_t *frame, size_t length) {
    atomic_val_t generation = atomic_get(&maintenance_generation);
    if (slot >= ARRAY_SIZE(ble_links) || epoch == 0 ||
        (connected && (!frame || length != DEFY_HYBRID_BLE_FRAME_SIZE))) {
        return -EINVAL;
    }
    struct hybrid_snapshot snapshot = {
        .link = DEFY_LINK_BLE,
        .source = slot,
        .side = DEFY_WIRED_SIDE_UNKNOWN,
        .epoch = epoch,
        .connected = connected,
        .received = k_uptime_get(),
    };
    if (connected) {
        for (size_t i = DEFY_WIRED_BITMAP_SIZE; i < length; i++) {
            if (frame[i]) {
                return -EPROTO;
            }
        }
        bool left = defy_wired_bitmap_valid(DEFY_WIRED_LEFT, frame);
        bool right = defy_wired_bitmap_valid(DEFY_WIRED_RIGHT, frame);
        if (!left && !right) {
            return -EPROTO;
        }
        if (left != right) {
            snapshot.side = left ? DEFY_WIRED_LEFT : DEFY_WIRED_RIGHT;
        }
        memcpy(snapshot.bitmap, frame, sizeof(snapshot.bitmap));
    }
    return enqueue_ble_snapshot(&snapshot, generation);
}

int defy_hybrid_central_peripheral_event(uint8_t source,
                                         struct zmk_split_transport_peripheral_event event) {
    if (defy_hybrid_central_is_maintenance()) {
        return 0;
    }
    /* Key positions come from the snapshots instead. */
    if (event.type == ZMK_SPLIT_TRANSPORT_PERIPHERAL_EVENT_TYPE_KEY_POSITION_EVENT) {
        return 0;
    }
    if (source >= ARRAY_SIZE(slot_sides)) {
        return -EINVAL;
    }
    atomic_val_t side = atomic_get(&slot_sides[source]);
    if (side == DEFY_WIRED_SIDE_UNKNOWN) {
        return -ENOTCONN;
    }
    return zmk_split_transport_central_peripheral_event_handler(&defy_hybrid_central, side,
                                                                event);
}

static int set_enabled(bool value) {
    atomic_set(&enabled, value);
    if (value) {
        defy_wired_central.api->set_status_callback(raw_status_changed);
        bt_central.api->set_status_callback(raw_status_changed);
    } else {
        defy_usb_input_reset();
        defy_wired_central.api->set_enabled(false);
        bt_central.api->set_enabled(false);
        wired_started = false;
        ble_started = false;
    }
    k_work_submit(&service_work);
    return 0;
}

static int get_available_source_ids(uint8_t *sources) {
    atomic_val_t mask = atomic_get(&connected_sides);
    int count = 0;
    for (uint8_t side = 0; side < ARRAY_SIZE(sides); side++) {
        if (mask & BIT(side)) {
            sources[count++] = side;
        }
    }
    return count;
}

static int send_command(uint8_t source, struct zmk_split_transport_central_command command) {
    if (source > DEFY_WIRED_RIGHT) {
        return -EINVAL;
    }
    if (defy_hybrid_central_is_maintenance()) {
        return -EBUSY;
    }
    if (command.type == ZMK_SPLIT_TRANSPORT_CENTRAL_CMD_TYPE_POLL_EVENTS) {
        return 0;
    }
    for (uint8_t slot = 0; slot < ARRAY_SIZE(slot_sides); slot++) {
        if (atomic_get(&slot_sides[slot]) == source) {
            return bt_central.api->send_command(slot, command);
        }
    }
    /* The cable carries no ZMK commands. */
    return -ENOTSUP;
}

int defy_hybrid_central_set_maintenance(bool value) {
    if (k_is_in_isr() || k_current_get() == k_work_queue_thread_get(&k_sys_work_q)) {
        return -EWOULDBLOCK;
    }
    if (k_mutex_lock(&maintenance_lock, K_NO_WAIT) < 0) {
        return -EBUSY;
    }
    if (!value && !defy_hybrid_central_is_maintenance()) {
        k_mutex_unlock(&maintenance_lock);
        return 0;
    }
    /* Leaving maintenance passes through here too, so the purge runs before the links resume. */
    k_spinlock_key_t key = k_spin_lock(&ble_latest_lock);
    atomic_set(&maintenance, 1);
    atomic_inc(&maintenance_generation);
    k_spin_unlock(&ble_latest_lock, key);
    defy_usb_input_reset();
    int err = defy_wired_central_set_maintenance(true);
    int health_err = defy_hybrid_ble_set_maintenance(true);
    if (!err) {
        err = health_err;
    }
    k_sem_reset(&maintenance_ready);
    int submitted = k_work_submit(&service_work);
    int release_err = submitted < 0 ? submitted : k_sem_take(&maintenance_ready, K_MSEC(500));
    if (!err) {
        err = release_err;
    }
    if (!err && !value) {
        err = defy_wired_central_set_maintenance(false);
        if (!err) {
            err = defy_hybrid_ble_set_maintenance(false);
        }
        if (!err) {
            key = k_spin_lock(&ble_latest_lock);
            atomic_inc(&maintenance_generation);
            atomic_clear(&maintenance);
            k_spin_unlock(&ble_latest_lock, key);
            k_work_submit(&service_work);
        }
    }
    k_mutex_unlock(&maintenance_lock);
    if (IS_ENABLED(CONFIG_DEFY_NEURON_STATUS_LED)) {
        defy_neuron_status_update();
    }
    return err;
}

static int set_status_callback(zmk_split_transport_central_status_changed_cb_t callback) {
    status_callback = callback;
    return 0;
}

static const struct zmk_split_transport_central_api hybrid_api = {
    .send_command = send_command,
    .get_available_source_ids = get_available_source_ids,
    .set_enabled = set_enabled,
    .get_status = get_status,
    .set_status_callback = set_status_callback,
};

/*
 * ZMK enables the first available transport in the order of the string
 * "<priority>_<name>", so priority 0 puts this one, which is always
 * available, ahead of ZMK's Bluetooth central.
 */
ZMK_SPLIT_TRANSPORT_CENTRAL_REGISTER(defy_hybrid_central, &hybrid_api, 0);
