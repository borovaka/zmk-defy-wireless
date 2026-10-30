/* SPDX-License-Identifier: MIT */

/*
 * The Neuron's end of the cables: an SPI slave on each socket and one worker
 * that services both. Each half's key snapshots go to the hybrid central, and
 * the worker acknowledges them. A transfer can carry one of the Neuron's
 * control commands in place of an acknowledgement the half already has: the battery display,
 * the lighting state, a group the state names or the host display. The
 * halves' battery telemetry and version reports go to their records on the
 * Neuron.
 *
 * Only the hybrid central uses this transport. ZMK selects the hybrid, which
 * is always available, so this one is not registered with ZMK.
 */

#include <errno.h>
#include <string.h>

#include <zephyr/device.h>
#include <zephyr/drivers/spi.h>
#include <zephyr/init.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/atomic.h>

#include <zmk/split/transport/central.h>

#include <dygma/defy_battery_central.h>
#include <dygma/defy_control.h>
#include <dygma/defy_feedback.h>
#include <dygma/defy_half_version.h>
#include <dygma/defy_hybrid.h>
#include <dygma/defy_key_trace.h>
#include <dygma/defy_lighting.h>
#include <dygma/defy_neuron_cables.h>
#include <dygma/defy_wired_protocol.h>

#include "defy_split_internal.h"

LOG_MODULE_REGISTER(defy_wired_central, CONFIG_ZMK_LOG_LEVEL);

/* How often the worker checks for a silent cable when nothing wakes it. */
#define SILENCE_CHECK_MS 10

struct wired_port {
    const struct device *device;
    /* The half on this port, and the keys it last reported, which the hybrid central has. */
    struct defy_wired_rx receiver;
    struct defy_wired_packet reply;
    struct spi_buf tx_buf;
    struct spi_buf rx_buf;
    struct spi_buf_set tx_set;
    struct spi_buf_set rx_set;
    uint8_t tx[DEFY_WIRED_FRAME_SIZE];
    uint8_t rx[DEFY_WIRED_FRAME_SIZE];
    atomic_t completed;
    int result;
    uint32_t completed_at;
    int64_t last_seen;
    bool armed;
    /* The last attempt to arm failed, which was logged once. */
    bool arm_failed;
    bool reply_valid;
    bool discard_completion;
    /* The armed transfer carries a command in place of the reply. */
    bool command_sent;
#if IS_ENABLED(CONFIG_DEFY_LIGHTING)
    /* The last plain acknowledgement that went out whole. */
    uint32_t sent_ack_session;
    uint32_t sent_ack_sequence;
    bool sent_ack_valid;
    /* The lighting state sent to this port's half, in the session and on the side it was for. */
    struct defy_lighting_retry lighting;
    uint32_t lighting_session;
    uint8_t lighting_side;
    /* The groups the half confirmed in that session, and those sent since the state last went. */
    uint16_t lighting_groups;
    uint16_t lighting_groups_sent;
#endif
};

static struct wired_port ports[] = {
    {.device = DEVICE_DT_GET(DT_PHANDLE_BY_IDX(DEFY_NEURON_CABLES_NODE, spi_ports, 0))},
    {.device = DEVICE_DT_GET(DT_PHANDLE_BY_IDX(DEFY_NEURON_CABLES_NODE, spi_ports, 1))},
};

static const struct spi_config spi_config = {
    .operation = SPI_OP_MODE_SLAVE | SPI_MODE_CPHA | SPI_WORD_SET(8) | SPI_TRANSFER_MSB,
};

static atomic_t enabled;
static atomic_t connected_sides;
static zmk_split_transport_central_status_changed_cb_t status_callback;
static struct k_thread worker_thread;
static K_THREAD_STACK_DEFINE(worker_stack, CONFIG_DEFY_WIRED_CENTRAL_STACK_SIZE);
static K_SEM_DEFINE(worker_wake, 0, 1);
static K_MUTEX_DEFINE(ports_lock);
static atomic_t maintenance;

#if IS_ENABLED(CONFIG_DEFY_BATTERY) || IS_ENABLED(CONFIG_DEFY_LIGHTING)
/* For each side, the port and session its half is on, and the control
 * commands waiting for it: the battery display and the latest host display. */
static struct k_spinlock routes_lock;
static struct side_route {
    uint32_t session;
    int64_t last_seen;
    uint8_t port;
    bool connected;
    bool battery_show_pending;
    int64_t battery_show_expires;
    struct defy_host_feedback feedback;
    int64_t feedback_expires;
    bool feedback_pending;
} side_routes[DEFY_WIRED_SIDES];

static void drop_route_commands(struct side_route *route) {
    route->battery_show_pending = false;
    route->feedback_pending = false;
}

static void clear_commands(void) {
    k_spinlock_key_t key = k_spin_lock(&routes_lock);
    for (size_t i = 0; i < ARRAY_SIZE(side_routes); i++) {
        drop_route_commands(&side_routes[i]);
    }
    k_spin_unlock(&routes_lock, key);
}

/* Whether the half on side has a live cable link that can take a command now. */
static bool route_live(uint8_t side, int64_t now) {
    return atomic_get(&enabled) && !defy_hybrid_central_is_maintenance() &&
           side_routes[side].connected &&
           now - side_routes[side].last_seen < CONFIG_DEFY_WIRED_CENTRAL_TIMEOUT_MS;
}

static void update_routes(void) {
    k_spinlock_key_t key = k_spin_lock(&routes_lock);
    for (uint8_t side = 0; side < ARRAY_SIZE(side_routes); side++) {
        struct side_route *route = &side_routes[side];
        struct wired_port *owner = NULL;
        for (size_t i = 0; i < ARRAY_SIZE(ports); i++) {
            if (ports[i].receiver.connected && ports[i].receiver.side == side) {
                owner = &ports[i];
                break;
            }
        }
        if (owner == NULL) {
            route->connected = false;
            drop_route_commands(route);
            continue;
        }
        uint8_t port = owner - ports;
        if (!route->connected || route->port != port ||
            route->session != owner->receiver.session) {
            drop_route_commands(route);
        }
        route->connected = true;
        route->session = owner->receiver.session;
        route->port = port;
        route->last_seen = owner->last_seen;
    }
    k_spin_unlock(&routes_lock, key);
}

/* Whether the route still leads to the half on this port, in its session. Called with
 * routes_lock held. */
static bool route_matches(const struct side_route *route, const struct wired_port *port) {
    return route->session == port->receiver.session && route->port == port - ports;
}

/* A command may take the reply's place only when the reply is a plain
 * acknowledgement and the transfer before carried no command, so that at
 * least every other transfer carries the acknowledgement. */
static bool reply_replaceable(const struct wired_port *port) {
    return port->receiver.connected && port->reply_valid && port->reply.flags == 0 &&
           !port->command_sent;
}
#else
static inline void clear_commands(void) {}
static inline void update_routes(void) {}
#endif

#if IS_ENABLED(CONFIG_DEFY_BATTERY)
int defy_wired_battery_show(uint8_t side) {
    if (side > DEFY_WIRED_RIGHT) {
        return -EINVAL;
    }
    int64_t now = k_uptime_get();
    k_spinlock_key_t key = k_spin_lock(&routes_lock);
    if (!route_live(side, now)) {
        k_spin_unlock(&routes_lock, key);
        return -ENOTCONN;
    }
    side_routes[side].battery_show_pending = true;
    side_routes[side].battery_show_expires = now + DEFY_CONTROL_TIMEOUT_MS;
    k_spin_unlock(&routes_lock, key);
    k_sem_give(&worker_wake);
    return 0;
}

static bool take_battery_show(struct wired_port *port, int64_t now) {
    if (!atomic_get(&enabled) || !reply_replaceable(port)) {
        return false;
    }
    uint8_t side = port->receiver.side;
    struct side_route *route = &side_routes[side];
    k_spinlock_key_t key = k_spin_lock(&routes_lock);
    bool valid = route->battery_show_pending && now < route->battery_show_expires &&
                 route_matches(route, port);
    route->battery_show_pending = false;
    if (valid) {
        struct defy_wired_battery_show_command command = {
            .side = side,
            .session = port->receiver.session,
        };
        defy_wired_battery_show_encode(&command, port->tx);
    }
    k_spin_unlock(&routes_lock, key);
    return valid;
}
#else
static inline bool take_battery_show(struct wired_port *port, int64_t now) {
    ARG_UNUSED(port);
    ARG_UNUSED(now);
    return false;
}
#endif

#if IS_ENABLED(CONFIG_DEFY_LIGHTING)
/* Whether the half already has the acknowledgement armed now, so that a
 * command may replace this copy of it. */
static bool last_ack_delivered(const struct wired_port *port) {
    return port->sent_ack_valid && port->sent_ack_session == port->reply.session &&
           port->sent_ack_sequence == port->reply.sequence;
}

/* Records the plain acknowledgement a completed transfer delivered, before the
 * next reply replaces it. */
static void record_delivered_ack(struct wired_port *port) {
    struct defy_wired_packet sent;
    if (port->result == DEFY_WIRED_FRAME_SIZE &&
        defy_wired_decode(port->tx, sizeof(port->tx), &sent) == 0 &&
        sent.kind == DEFY_WIRED_ACK && sent.flags == 0) {
        port->sent_ack_valid = true;
        port->sent_ack_session = sent.session;
        port->sent_ack_sequence = sent.sequence;
    }
}

/* The port's half is gone: the next one starts the lighting state afresh. */
static void forget_lighting(struct wired_port *port) {
    port->lighting_session = 0;
    port->sent_ack_valid = false;
}

void defy_wired_host_feedback_show(const struct defy_host_feedback *state) {
    int64_t now = k_uptime_get();
    k_spinlock_key_t key = k_spin_lock(&routes_lock);
    for (uint8_t side = 0; side < ARRAY_SIZE(side_routes); side++) {
        if (route_live(side, now)) {
            side_routes[side].feedback = *state;
            side_routes[side].feedback_expires = now + state->remaining_ms;
            side_routes[side].feedback_pending = true;
        }
    }
    k_spin_unlock(&routes_lock, key);
    k_sem_give(&worker_wake);
}

static bool take_feedback(struct wired_port *port, int64_t now) {
    if (!atomic_get(&enabled) || !reply_replaceable(port) || !last_ack_delivered(port)) {
        return false;
    }
    uint8_t side = port->receiver.side;
    struct side_route *route = &side_routes[side];
    k_spinlock_key_t key = k_spin_lock(&routes_lock);
    int64_t remaining = route->feedback_expires - now;
    bool valid = route->feedback_pending && remaining > 0 && route_matches(route, port);
    route->feedback_pending = false;
    if (valid) {
        struct defy_wired_host_feedback command = {
            .side = side, .session = port->receiver.session, .state = route->feedback,
        };
        command.state.remaining_ms = MIN(remaining, DEFY_HOST_FEEDBACK_DURATION_MS);
        valid = defy_wired_host_feedback_encode(&command, port->tx) == 0;
    }
    k_spin_unlock(&routes_lock, key);
    return valid;
}

void defy_wired_lighting_changed(void) { k_sem_give(&worker_wake); }

/*
 * Puts the next group that state names and the half has not confirmed in the
 * transfer, lowest index first: 1 when it did, 0 when none is left before the
 * state, and an error when the group cannot go.
 */
static int take_lighting_group(struct wired_port *port, const struct defy_lighting_state *state) {
    uint16_t pending = state->groups & ~(port->lighting_groups | port->lighting_groups_sent);
    if (!pending) {
        return 0;
    }
    uint8_t index = find_lsb_set(pending) - 1;
    struct defy_wired_lighting_group_command command = {
        .side = port->receiver.side,
        .session = port->receiver.session,
    };
    int err = defy_lighting_central_group(command.side, index, &command.group);
    if (!err) {
        err = defy_wired_lighting_group_encode(&command, port->tx);
    }
    if (err) {
        return err;
    }
    port->lighting_groups_sent |= BIT(index);
    return 1;
}

/*
 * Puts the lighting state in the transfer when it is due, after the groups it
 * names that the half has not confirmed, one per transfer. A group that cannot
 * go costs the attempt, and the next one sends the groups again.
 */
static bool take_lighting(struct wired_port *port, int64_t now) {
    struct defy_lighting_state desired;
    if (!atomic_get(&enabled) || !port->receiver.connected || !port->reply_valid ||
        port->reply.flags != 0 ||
        now - port->last_seen >= CONFIG_DEFY_WIRED_CENTRAL_TIMEOUT_MS ||
        !defy_lighting_central_get(&desired)) {
        return false;
    }
    if (port->lighting_session != port->receiver.session ||
        port->lighting_side != port->receiver.side) {
        defy_lighting_retry_start(&port->lighting, &desired, now);
        port->lighting_session = port->receiver.session;
        port->lighting_side = port->receiver.side;
        port->lighting_groups = 0;
        port->lighting_groups_sent = 0;
    } else if (defy_lighting_retry_follow(&port->lighting, &desired, now)) {
        port->lighting_groups_sent = 0;
    }
    if (!defy_lighting_retry_due(&port->lighting, now) || !reply_replaceable(port) ||
        !last_ack_delivered(port)) {
        return false;
    }
    int group = take_lighting_group(port, &desired);
    if (group > 0) {
        return true;
    }
    if (group < 0) {
        defy_lighting_retry_sent(&port->lighting, now);
        port->lighting_groups_sent = 0;
        return false;
    }
    struct defy_wired_lighting_command command = {
        .opcode = DEFY_CONTROL_LIGHTING_SET,
        .side = port->receiver.side,
        .session = port->receiver.session,
        .state = desired,
    };
    if (defy_wired_lighting_encode(&command, port->tx) < 0) {
        return false;
    }
    defy_lighting_retry_sent(&port->lighting, now);
    port->lighting_groups_sent = 0;
    return true;
}

static void receive_lighting_ack(struct wired_port *port) {
    struct defy_wired_control control;
    struct defy_wired_lighting_command command;
    if (!port->receiver.connected ||
        defy_wired_control_check(port->rx, sizeof(port->rx), &control) < 0 ||
        control.opcode != DEFY_CONTROL_LIGHTING_ACK ||
        defy_wired_lighting_decode(port->rx, &control, &command) < 0 ||
        command.side != port->receiver.side || command.session != port->receiver.session ||
        port->lighting_session != command.session) {
        return;
    }
    defy_lighting_retry_acknowledged(&port->lighting, &command.state);
    /* The half took the state, so it holds the groups the state names. */
    if (defy_lighting_equal(&port->lighting.target, &command.state)) {
        port->lighting_groups |= command.state.groups;
    }
}
#else
static inline void record_delivered_ack(struct wired_port *port) { ARG_UNUSED(port); }
static inline void forget_lighting(struct wired_port *port) { ARG_UNUSED(port); }
static inline bool take_feedback(struct wired_port *port, int64_t now) {
    ARG_UNUSED(port);
    ARG_UNUSED(now);
    return false;
}
static inline bool take_lighting(struct wired_port *port, int64_t now) {
    ARG_UNUSED(port);
    ARG_UNUSED(now);
    return false;
}
static inline void receive_lighting_ack(struct wired_port *port) { ARG_UNUSED(port); }
#endif

/* A 'DV' frame: the version report of the half on this port, for its side's records. */
static void receive_version(struct wired_port *port) {
    uint8_t record[DEFY_HALF_VERSION_SIZE];
    struct defy_half_version version;
    if (IS_ENABLED(CONFIG_DEFY_HALF_VERSION) && port->receiver.connected &&
        defy_wired_half_version_decode(port->rx, sizeof(port->rx), record) == 0 &&
        defy_half_version_decode(record, &version) && version.side == port->receiver.side) {
        defy_half_version_received(DEFY_LINK_WIRED, port - ports, &version);
    }
}

static struct zmk_split_transport_status get_status(void) {
    atomic_val_t connected = atomic_get(&connected_sides);
    return (struct zmk_split_transport_status){
        .available = device_is_ready(ports[0].device) && device_is_ready(ports[1].device),
        .enabled = atomic_get(&enabled) != 0,
        .connections = connected == BIT_MASK(DEFY_WIRED_SIDES)
                           ? ZMK_SPLIT_TRANSPORT_CONNECTIONS_STATUS_ALL_CONNECTED
                       : connected ? ZMK_SPLIT_TRANSPORT_CONNECTIONS_STATUS_SOME_CONNECTED
                                   : ZMK_SPLIT_TRANSPORT_CONNECTIONS_STATUS_DISCONNECTED,
    };
}

static void update_status(void) {
    atomic_val_t connected = 0;
    for (size_t i = 0; i < ARRAY_SIZE(ports); i++) {
        if (ports[i].receiver.connected) {
            connected |= BIT(ports[i].receiver.side);
        }
    }

    update_routes();

    if (atomic_set(&connected_sides, connected) != connected && status_callback) {
        status_callback(&defy_wired_central, get_status());
    }
}

static bool disconnect_port(struct wired_port *port) {
    port->reply_valid = false;
    if (port->receiver.connected) {
        if (defy_hybrid_central_wired_update(port->receiver.side, port->receiver.session,
                                              false, NULL) < 0) {
            return false;
        }
        if (IS_ENABLED(CONFIG_DEFY_BATTERY)) {
            defy_battery_central_disconnect(DEFY_LINK_WIRED, port - ports,
                                            port->receiver.session);
        }
        if (IS_ENABLED(CONFIG_DEFY_HALF_VERSION)) {
            defy_half_version_wired_down(port->receiver.side);
        }
    }
    defy_wired_rx_init(&port->receiver);
    forget_lighting(port);
    return true;
}

static void receive_frame(struct wired_port *port, int64_t now) {
    if (port->result != DEFY_WIRED_FRAME_SIZE) {
        return;
    }
    if (defy_wired_frame_is(port->rx, 'L')) {
        receive_lighting_ack(port);
        return;
    }
    if (defy_wired_frame_is(port->rx, 'B')) {
        if (IS_ENABLED(CONFIG_DEFY_BATTERY) && port->receiver.connected) {
            defy_battery_central_accept(DEFY_LINK_WIRED, port - ports, port->receiver.session,
                                        port->rx, sizeof(port->rx), 0);
        }
        return;
    }
    if (defy_wired_frame_is(port->rx, 'V')) {
        receive_version(port);
        return;
    }
    struct defy_wired_packet packet;
    if (defy_wired_decode(port->rx, sizeof(port->rx), &packet) < 0) {
        return;
    }

    struct defy_wired_rx candidate = port->receiver;
    struct defy_wired_packet reply;
    int result = defy_wired_rx_accept(&candidate, &packet, &reply);
    if (result < 0) {
        return;
    }
    if (result == DEFY_WIRED_RESYNC) {
        port->reply = reply;
        port->reply_valid = true;
        return;
    }

    port->reply_valid = false;
    for (size_t i = 0; i < ARRAY_SIZE(ports); i++) {
        if (&ports[i] != port && ports[i].receiver.connected &&
            ports[i].receiver.side == candidate.side) {
            if (now - ports[i].last_seen < CONFIG_DEFY_WIRED_CENTRAL_TIMEOUT_MS ||
                !disconnect_port(&ports[i])) {
                return;
            }
        }
    }
    port->last_seen = now;

    if (port->receiver.connected && port->receiver.side != candidate.side) {
        if (!disconnect_port(port)) {
            return;
        }
    }
    bool new_route = !port->receiver.connected || port->receiver.session != candidate.session;
    if (new_route || memcmp(port->receiver.bitmap, candidate.bitmap, DEFY_WIRED_BITMAP_SIZE)) {
        defy_key_trace_add(DEFY_KEY_TRACE_WIRED_POLL,
                           candidate.side | DEFY_KEY_TRACE_ACTIVE |
                               (port == &ports[1] ? DEFY_KEY_TRACE_SOURCE : 0),
                           candidate.sequence, port->completed_at);
        if (defy_hybrid_central_wired_update(candidate.side, candidate.session, true,
                                              candidate.bitmap) < 0) {
            return;
        }
    }
    if (IS_ENABLED(CONFIG_DEFY_BATTERY) && new_route) {
        defy_battery_central_route_live(DEFY_LINK_WIRED, port - ports, candidate.session,
                                        candidate.side);
    }
    port->receiver = candidate;
    port->reply = reply;
    port->reply_valid = true;
}

static void transfer_complete(const struct device *device, int result, void *userdata) {
    struct wired_port *port = userdata;
    ARG_UNUSED(device);
    port->result = result;
    port->completed_at = defy_key_trace_now();
    atomic_set(&port->completed, 1);
    k_sem_give(&worker_wake);
}

static void arm_port(struct wired_port *port, int64_t now) {
    memset(port->rx, 0, sizeof(port->rx));
    memset(port->tx, 0, sizeof(port->tx));
    if (port->reply_valid) {
        defy_wired_encode(&port->reply, port->tx);
    }
    /* At most one command per transfer, the battery display first. */
    port->command_sent = take_battery_show(port, now) || take_lighting(port, now) ||
                         take_feedback(port, now);
    port->armed = true;
    int err = spi_transceive_cb(port->device, &spi_config, &port->tx_set, &port->rx_set,
                               transfer_complete, port);
    if (err < 0) {
        port->armed = false;
        if (!port->arm_failed) {
            LOG_WRN("Wired SPI arm failed: %d", err);
        }
    } else if (port->arm_failed) {
        LOG_INF("Wired SPI armed again");
    }
    port->arm_failed = err < 0;
}

static void service_ports(int64_t now) {
    k_mutex_lock(&ports_lock, K_FOREVER);
    bool paused = atomic_get(&maintenance) || defy_hybrid_central_is_maintenance();
    bool is_enabled = atomic_get(&enabled) != 0 && !paused;
    for (size_t i = 0; i < ARRAY_SIZE(ports); i++) {
        struct wired_port *port = &ports[i];
        if (atomic_cas(&port->completed, 1, 0)) {
            port->armed = false;
            if (!paused && !port->discard_completion) {
                record_delivered_ack(port);
            }
            if (is_enabled && !port->discard_completion) {
                receive_frame(port, now);
            }
            port->discard_completion = false;
        }

        if (!is_enabled || (port->receiver.connected &&
                            now - port->last_seen >= CONFIG_DEFY_WIRED_CENTRAL_TIMEOUT_MS)) {
            disconnect_port(port);
        }
        if (!port->armed && !paused) {
            arm_port(port, now);
        }
    }
    update_status();
    k_mutex_unlock(&ports_lock);
}

int defy_wired_central_set_maintenance(bool value) {
    int err = k_mutex_lock(&ports_lock, K_MSEC(500));
    if (err) {
        return err;
    }
    atomic_set(&maintenance, value);
    if (value) {
        for (size_t i = 0; i < ARRAY_SIZE(ports); i++) {
            ports[i].discard_completion = ports[i].armed;
            disconnect_port(&ports[i]);
        }
        clear_commands();
        update_status();
    }
    k_mutex_unlock(&ports_lock);
    k_sem_give(&worker_wake);
    return 0;
}

static void worker(void *p1, void *p2, void *p3) {
    ARG_UNUSED(p1);
    ARG_UNUSED(p2);
    ARG_UNUSED(p3);
    while (true) {
        service_ports(k_uptime_get());
        k_sem_take(&worker_wake, K_MSEC(SILENCE_CHECK_MS));
    }
}

static int set_enabled(bool value) {
    atomic_set(&enabled, value);
    if (!value) {
        clear_commands();
    }
    k_sem_give(&worker_wake);
    return 0;
}

static int set_status_callback(zmk_split_transport_central_status_changed_cb_t callback) {
    status_callback = callback;
    return 0;
}

static const struct zmk_split_transport_central_api transport_api = {
    .set_enabled = set_enabled,
    .get_status = get_status,
    .set_status_callback = set_status_callback,
};

const struct zmk_split_transport_central defy_wired_central = {.api = &transport_api};

static int configure_ports(void) {
    for (size_t i = 0; i < ARRAY_SIZE(ports); i++) {
        struct wired_port *port = &ports[i];
        if (!device_is_ready(port->device)) {
            return -ENODEV;
        }
        port->tx_buf = (struct spi_buf){.buf = port->tx, .len = sizeof(port->tx)};
        port->rx_buf = (struct spi_buf){.buf = port->rx, .len = sizeof(port->rx)};
        port->tx_set = (struct spi_buf_set){.buffers = &port->tx_buf, .count = 1};
        port->rx_set = (struct spi_buf_set){.buffers = &port->rx_buf, .count = 1};
    }
    return 0;
}

static int wired_init(void) {
    int err = configure_ports();
    if (err < 0) {
        return err;
    }
    k_thread_create(&worker_thread, worker_stack, K_THREAD_STACK_SIZEOF(worker_stack), worker,
                    NULL, NULL, NULL, K_PRIO_PREEMPT(CONFIG_DEFY_WIRED_CENTRAL_THREAD_PRIORITY), 0,
                    K_NO_WAIT);
    k_thread_name_set(&worker_thread, "defy_wired");
    return 0;
}

SYS_INIT(wired_init, APPLICATION, CONFIG_APPLICATION_INIT_PRIORITY);
