/* SPDX-License-Identifier: MIT */

/*
 * A half's end of the cable: the SPI master that polls the Neuron. Each
 * transfer carries the oldest key snapshot the Neuron has not acknowledged,
 * and the reply acknowledges it. Once the Neuron has every snapshot, a
 * transfer can carry a lighting acknowledgement, battery telemetry or the
 * half's version report instead, and a reply can carry one of the Neuron's
 * control commands: the battery display, the lighting state, a group the state names or the
 * host display.
 */

#include <errno.h>
#include <string.h>

#include <zephyr/drivers/spi.h>
#include <zephyr/init.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/util.h>

#include <zmk/split/transport/peripheral.h>

#include <dygma/defy_battery.h>
#include <dygma/defy_battery_hci.h>
#include <dygma/defy_control.h>
#include <dygma/defy_feedback.h>
#include <dygma/defy_half.h>
#include <dygma/defy_half_leds.h>
#include <dygma/defy_half_version.h>
#include <dygma/defy_lighting.h>
#include <dygma/defy_wired_protocol.h>

#include "defy_split_internal.h"

#define DT_DRV_COMPAT dygma_defy_wired_peripheral

LOG_MODULE_REGISTER(defy_wired_peripheral, CONFIG_ZMK_LOG_LEVEL);

BUILD_ASSERT(DT_NUM_INST_STATUS_OKAY(DT_DRV_COMPAT) == 1);
BUILD_ASSERT(DEFY_HALF_ID <= DEFY_WIRED_RIGHT);

/*
 * After ZMK's peripheral_init, which installs ZMK's status callback on every
 * registered transport, and before ZMK's physical layout init starts the key
 * scan. On a hybrid half, init() reporting the cable ready through that
 * callback is what makes ZMK select the hybrid transport this early, before
 * settings_load() lets Bluetooth become available, so that keys held at start
 * reach a transport. That is why the halves register this transport with ZMK.
 */
#define CABLE_INIT_PRIORITY 80

BUILD_ASSERT(CONFIG_KERNEL_INIT_PRIORITY_DEFAULT < CABLE_INIT_PRIORITY &&
             CABLE_INIT_PRIORITY < CONFIG_APPLICATION_INIT_PRIORITY);

/* How often battery telemetry may take an idle frame's place. */
#define BATTERY_INTERVAL_MS 5000
/* How often the version report takes an idle frame's place, after the first
 * idle frame once the link comes up. */
#define VERSION_INTERVAL_MS 60000
#define CABLE_CS_DELAY_US 10

static const struct spi_dt_spec cable = SPI_DT_SPEC_INST_GET(
    0, SPI_WORD_SET(8) | SPI_TRANSFER_MSB | SPI_MODE_CPHA, CABLE_CS_DELAY_US);

static struct defy_wired_tx transmitter;
static struct k_spinlock state_lock;
static K_MUTEX_DEFINE(io_lock);
static bool ready;
static bool enabled;
static bool connected;
static bool quiescing;
/* Starts in the past, so that no reply counts as recent before the first. */
static int64_t last_reply_ms = -CONFIG_DEFY_WIRED_PERIPHERAL_TIMEOUT_MS;
/* The last key change, which sets how often an idle hybrid half polls. */
static int64_t last_activity_ms;
static int64_t last_battery_ms = -BATTERY_INTERVAL_MS;
/* The version report's 'DV' frame, made once at start, as it does not change. */
static uint8_t version_frame[DEFY_WIRED_FRAME_SIZE];
/* The link came up, and the version report waits for the next idle frame. */
static bool version_pending;
static int64_t last_version_ms;
/* The Neuron's latest commands, until the system work queue takes them. */
static struct defy_wired_battery_show_command battery_show_command;
static int64_t battery_show_expires;
static bool battery_show_pending;
static struct defy_wired_lighting_command lighting_command;
static int64_t lighting_expires;
static bool lighting_pending;
static struct defy_wired_host_feedback feedback_command;
static int64_t feedback_expires;
static bool feedback_pending;
/* The applied lighting state, to acknowledge in the next idle frame. */
static struct defy_lighting_state lighting_ack;
static uint32_t lighting_ack_session;
static bool lighting_ack_pending;
static zmk_split_transport_peripheral_status_changed_cb_t status_callback;

static struct k_thread thread;
static K_THREAD_STACK_DEFINE(thread_stack, CONFIG_DEFY_WIRED_PERIPHERAL_STACK_SIZE);
static K_SEM_DEFINE(wake, 0, 1);

/* Whether the half may follow a command for this side and session now: the
 * cable is up and the Neuron replied recently. Called with state_lock held. */
static bool command_allowed(uint8_t side, uint32_t session, int64_t now) {
    return enabled && !quiescing && connected && side == DEFY_HALF_ID &&
           session == transmitter.session &&
           now - last_reply_ms < MIN(CONFIG_DEFY_WIRED_PERIPHERAL_TIMEOUT_MS,
                                    DEFY_CONTROL_TIMEOUT_MS);
}

#if IS_ENABLED(CONFIG_DEFY_BATTERY) || IS_ENABLED(CONFIG_DEFY_LIGHTING)
/* Clears *pending and returns whether the command it marked may still be
 * followed. Called with state_lock held. */
static bool take_command(bool *pending, int64_t expires, uint8_t side, uint32_t session,
                         int64_t now) {
    bool take = *pending && now < expires && command_allowed(side, session, now);

    *pending = false;
    return take;
}
#endif

#if IS_ENABLED(CONFIG_DEFY_BATTERY)
static void show_battery(struct k_work *work) {
    ARG_UNUSED(work);
    int64_t now = k_uptime_get();
    k_spinlock_key_t key = k_spin_lock(&state_lock);
    bool show = take_command(&battery_show_pending, battery_show_expires,
                             battery_show_command.side, battery_show_command.session, now);
    k_spin_unlock(&state_lock, key);
    if (show) {
        defy_half_leds_battery_show();
    }
}

static K_WORK_DEFINE(battery_show_work, show_battery);
#endif

#if IS_ENABLED(CONFIG_DEFY_BATTERY_HCI)
static void refresh_battery(struct k_work *work) {
    ARG_UNUSED(work);
    defy_battery_hci_refresh();
}

static K_WORK_DEFINE(refresh_work, refresh_battery);
#endif

#if IS_ENABLED(CONFIG_DEFY_LIGHTING)
static void apply_lighting(struct k_work *work) {
    ARG_UNUSED(work);
    int64_t now = k_uptime_get();
    k_spinlock_key_t key = k_spin_lock(&state_lock);
    struct defy_wired_lighting_command command = lighting_command;
    bool apply = take_command(&lighting_pending, lighting_expires, command.side,
                              command.session, now);
    k_spin_unlock(&state_lock, key);
    if (!apply) {
        return;
    }
    struct defy_lighting_state state = command.state;
    if (defy_lighting_peripheral_apply(&state, DEFY_LINK_WIRED, state.boot) < 0 ||
        !defy_lighting_peripheral_get(&state)) {
        return;
    }
    key = k_spin_lock(&state_lock);
    bool acknowledge = command_allowed(command.side, command.session, k_uptime_get());
    if (acknowledge) {
        lighting_ack = state;
        lighting_ack_session = command.session;
        lighting_ack_pending = true;
    }
    k_spin_unlock(&state_lock, key);
    if (acknowledge) {
        k_sem_give(&wake);
    }
}

static K_WORK_DEFINE(lighting_work, apply_lighting);

static void show_feedback(struct k_work *work) {
    ARG_UNUSED(work);
    int64_t now = k_uptime_get();
    k_spinlock_key_t key = k_spin_lock(&state_lock);
    int64_t remaining = feedback_expires - now;
    struct defy_host_feedback state = feedback_command.state;
    bool show = take_command(&feedback_pending, feedback_expires, feedback_command.side,
                             feedback_command.session, now);
    k_spin_unlock(&state_lock, key);
    if (show) {
        state.remaining_ms = MIN(remaining, DEFY_HOST_FEEDBACK_DURATION_MS);
        defy_half_leds_host_show(&state);
    }
}

static K_WORK_DEFINE(feedback_work, show_feedback);
#endif

static bool polling_enabled(void) {
    k_spinlock_key_t key = k_spin_lock(&state_lock);
    bool value = enabled && !quiescing;

    k_spin_unlock(&state_lock, key);
    return value;
}

void defy_wired_peripheral_cancel_sleep(void) {
    k_spinlock_key_t key = k_spin_lock(&state_lock);
    bool changed = quiescing;

    quiescing = false;
    k_spin_unlock(&state_lock, key);
    if (changed) {
        k_sem_give(&wake);
    }
}

int defy_wired_peripheral_prepare_sleep(void) {
    k_spinlock_key_t key = k_spin_lock(&state_lock);

    quiescing = true;
    k_spin_unlock(&state_lock, key);
    int err = k_mutex_lock(&io_lock, K_MSEC(100));

    if (err < 0) {
        defy_wired_peripheral_cancel_sleep();
        return -ETIMEDOUT;
    }
    key = k_spin_lock(&state_lock);
    bool busy = connected ||
                k_uptime_get() - last_reply_ms < CONFIG_DEFY_WIRED_PERIPHERAL_TIMEOUT_MS ||
                !defy_wired_bitmap_empty(transmitter.bitmap);

    k_spin_unlock(&state_lock, key);
    k_mutex_unlock(&io_lock);
    if (busy) {
        defy_wired_peripheral_cancel_sleep();
        return -EBUSY;
    }
    return 0;
}

static struct zmk_split_transport_status transport_status(void) {
    k_spinlock_key_t key = k_spin_lock(&state_lock);
    struct zmk_split_transport_status status = {
        .available = ready,
        .enabled = enabled,
        .connections = connected ? ZMK_SPLIT_TRANSPORT_CONNECTIONS_STATUS_ALL_CONNECTED
                                 : ZMK_SPLIT_TRANSPORT_CONNECTIONS_STATUS_DISCONNECTED,
    };

    k_spin_unlock(&state_lock, key);
    return status;
}

static void publish_status(struct zmk_split_transport_status *previous) {
    struct zmk_split_transport_status current = transport_status();

    if (current.available == previous->available && current.enabled == previous->enabled &&
        current.connections == previous->connections) {
        return;
    }

    k_spinlock_key_t key = k_spin_lock(&state_lock);
    zmk_split_transport_peripheral_status_changed_cb_t callback = status_callback;

    k_spin_unlock(&state_lock, key);
    if (callback != NULL) {
        callback(&defy_wired_peripheral, current);
    }
    if (current.connections != previous->connections) {
        bool now_connected =
            current.connections == ZMK_SPLIT_TRANSPORT_CONNECTIONS_STATUS_ALL_CONNECTED;

        LOG_INF("Cable %s", now_connected ? "connected" : "disconnected");
#if IS_ENABLED(CONFIG_DEFY_BATTERY_HCI)
        k_work_submit(&refresh_work);
#endif
    }
    *previous = current;
}

static void prepare_version(void) {
    struct defy_half_version version;
    uint8_t record[DEFY_HALF_VERSION_SIZE];

    defy_half_version_local(&version);
    defy_half_version_encode(&version, record);
    defy_wired_half_version_encode(record, version_frame);
}

/* Puts the version report in an idle frame once the link has come up, then
 * every VERSION_INTERVAL_MS. Called with state_lock held. */
static void fill_version(uint8_t tx[DEFY_WIRED_FRAME_SIZE], int64_t now) {
    if (version_pending || now - last_version_ms >= VERSION_INTERVAL_MS) {
        memcpy(tx, version_frame, DEFY_WIRED_FRAME_SIZE);
        version_pending = false;
        last_version_ms = now;
    }
}

/*
 * Fills the next frame with the oldest key snapshot the Neuron has not
 * acknowledged. Once the Neuron has them all, a pending lighting
 * acknowledgement or, at most every BATTERY_INTERVAL_MS, battery telemetry
 * goes in its place, or else the version report when it is due. Called with
 * state_lock held.
 */
static void fill_tx(uint8_t tx[DEFY_WIRED_FRAME_SIZE], const struct defy_battery_frame *battery) {
    defy_wired_tx_frame(&transmitter, tx);
    bool idle = connected && transmitter.count == 0 && transmitter.has_acknowledged;

    if (IS_ENABLED(CONFIG_DEFY_LIGHTING) && idle && lighting_ack_pending &&
        lighting_ack_session == transmitter.session) {
        struct defy_wired_lighting_command command = {
            .opcode = DEFY_CONTROL_LIGHTING_ACK, .side = DEFY_HALF_ID,
            .session = transmitter.session, .state = lighting_ack,
        };
        bool sent = defy_wired_lighting_encode(&command, tx) == 0;

        lighting_ack_pending = false;
        if (sent) {
            return;
        }
    }
    if (!idle) {
        return;
    }
    int64_t now = k_uptime_get();

    if (IS_ENABLED(CONFIG_DEFY_BATTERY) && battery != NULL &&
        now - last_battery_ms >= BATTERY_INTERVAL_MS &&
        defy_battery_frame_encode(battery, tx) == 0) {
        last_battery_ms = now;
        return;
    }
    if (IS_ENABLED(CONFIG_DEFY_HALF_VERSION)) {
        fill_version(tx, now);
    }
}

/* The command a reply from the Neuron carried, if any. */
struct cable_commands {
    struct defy_wired_battery_show_command battery_show;
    struct defy_wired_lighting_command lighting;
    struct defy_wired_lighting_group_command group;
    struct defy_wired_host_feedback feedback;
    bool battery_show_received;
    bool lighting_received;
    bool group_received;
    bool feedback_received;
};

/* Decodes the command in a 'DL' reply, when it is of a kind the half follows. */
static void decode_command(const uint8_t rx[DEFY_WIRED_FRAME_SIZE],
                           struct cable_commands *commands) {
    struct defy_wired_control control;

    if (defy_wired_control_check(rx, DEFY_WIRED_FRAME_SIZE, &control) < 0) {
        return;
    }
    switch (control.opcode) {
    case DEFY_CONTROL_BATTERY_SHOW:
        commands->battery_show_received =
            IS_ENABLED(CONFIG_DEFY_BATTERY) &&
            defy_wired_battery_show_decode(rx, &control, &commands->battery_show) == 0;
        break;
    case DEFY_CONTROL_LIGHTING_SET:
        commands->lighting_received =
            IS_ENABLED(CONFIG_DEFY_LIGHTING) &&
            defy_wired_lighting_decode(rx, &control, &commands->lighting) == 0;
        break;
    case DEFY_CONTROL_LIGHTING_GROUP:
        commands->group_received =
            IS_ENABLED(CONFIG_DEFY_LIGHTING) &&
            defy_wired_lighting_group_decode(rx, &control, &commands->group) == 0;
        break;
    case DEFY_CONTROL_HOST_FEEDBACK:
        commands->feedback_received =
            IS_ENABLED(CONFIG_DEFY_LIGHTING) &&
            defy_wired_host_feedback_decode(rx, &control, &commands->feedback) == 0;
        break;
    default:
        break;
    }
}

/*
 * Keeps the commands the half may follow and drops the rest. A host display
 * or lighting state no newer than the pending one is dropped, and the pending
 * one keeps its expiry; Bluetooth replaces an equal lighting revision instead.
 * Called with state_lock held, before the reply updates the link.
 */
static void receive_commands(struct cable_commands *commands, int64_t now) {
    const struct defy_wired_host_feedback *feedback = &commands->feedback;

    commands->feedback_received = commands->feedback_received &&
                                  command_allowed(feedback->side, feedback->session, now);
    if (commands->feedback_received && feedback_pending &&
        feedback->state.boot == feedback_command.state.boot &&
        (int32_t)(feedback->state.sequence - feedback_command.state.sequence) <= 0) {
        commands->feedback_received = false;
    }
    if (commands->feedback_received) {
        feedback_command = *feedback;
        feedback_expires = now + feedback->state.remaining_ms;
        feedback_pending = true;
    }

    const struct defy_wired_lighting_command *light = &commands->lighting;

    commands->lighting_received = commands->lighting_received &&
                                  command_allowed(light->side, light->session, now);
    if (commands->lighting_received && lighting_pending &&
        light->state.boot == lighting_command.state.boot &&
        (int32_t)(light->state.revision - lighting_command.state.revision) <= 0) {
        commands->lighting_received = false;
    }
    if (commands->lighting_received) {
        lighting_command = *light;
        lighting_expires = now + DEFY_CONTROL_TIMEOUT_MS;
        lighting_pending = true;
    }

    const struct defy_wired_lighting_group_command *group = &commands->group;

    commands->group_received = commands->group_received &&
                               command_allowed(group->side, group->session, now);

    const struct defy_wired_battery_show_command *show = &commands->battery_show;

    commands->battery_show_received = commands->battery_show_received &&
                                      command_allowed(show->side, show->session, now);
    if (commands->battery_show_received) {
        battery_show_command = *show;
        battery_show_expires = now + DEFY_CONTROL_TIMEOUT_MS;
        battery_show_pending = true;
    }
}

/* Takes the Neuron's acknowledgement, or drops the link when none came in
 * time. Called with state_lock held. */
static void receive_ack(const struct defy_wired_packet *reply, int64_t now) {
    if (enabled && reply != NULL && defy_wired_tx_ack(&transmitter, reply)) {
        bool was_connected = connected;

        connected = !(reply->flags & DEFY_WIRED_FLAG_RESYNC);
        if (!connected) {
            feedback_pending = false;
        } else if (!was_connected) {
            version_pending = true;
        }
        last_reply_ms = now;
    } else if (connected && now - last_reply_ms >= CONFIG_DEFY_WIRED_PERIPHERAL_TIMEOUT_MS) {
        connected = false;
        feedback_pending = false;
        defy_wired_tx_resync(&transmitter);
    }
}

/*
 * Keeps a group at once, before any state that a later reply carries, and
 * hands the other kept commands to the system work queue.
 */
static void queue_commands(const struct cable_commands *commands) {
#if IS_ENABLED(CONFIG_DEFY_LIGHTING)
    if (commands->group_received) {
        defy_lighting_peripheral_group(&commands->group.group);
    }
    if (commands->feedback_received) {
        k_work_submit(&feedback_work);
    }
    if (commands->lighting_received) {
        k_work_submit(&lighting_work);
    }
#endif
#if IS_ENABLED(CONFIG_DEFY_BATTERY)
    if (commands->battery_show_received) {
        k_work_submit(&battery_show_work);
    }
#endif
    ARG_UNUSED(commands);
}

static void poll_cable(void) {
    uint8_t tx[DEFY_WIRED_FRAME_SIZE];
    uint8_t rx[DEFY_WIRED_FRAME_SIZE] = {0};
    struct spi_buf tx_buffer = {.buf = tx, .len = sizeof(tx)};
    struct spi_buf rx_buffer = {.buf = rx, .len = sizeof(rx)};
    const struct spi_buf_set tx_buffers = {.buffers = &tx_buffer, .count = 1};
    const struct spi_buf_set rx_buffers = {.buffers = &rx_buffer, .count = 1};
    struct defy_battery_frame battery;
    bool battery_available =
        IS_ENABLED(CONFIG_DEFY_BATTERY) && defy_battery_local_frame(&battery) == 0;

    k_mutex_lock(&io_lock, K_FOREVER);
    k_spinlock_key_t key = k_spin_lock(&state_lock);

    if (!enabled || quiescing) {
        k_spin_unlock(&state_lock, key);
        k_mutex_unlock(&io_lock);
        return;
    }
    fill_tx(tx, battery_available ? &battery : NULL);
    k_spin_unlock(&state_lock, key);

    int err = spi_transceive_dt(&cable, &tx_buffers, &rx_buffers);
    int64_t now = k_uptime_get();
    struct defy_wired_packet reply;
    bool decoded = false;
    struct cable_commands commands = {0};

    /* A reply carries the Neuron's acknowledgement, 'DW', or a command, 'DL'. */
    if (err == 0 && defy_wired_frame_is(rx, 'L')) {
        decode_command(rx, &commands);
    } else if (err == 0) {
        decoded = defy_wired_decode(rx, sizeof(rx), &reply) == 0;
    }
    key = k_spin_lock(&state_lock);
    receive_commands(&commands, now);
    receive_ack(decoded ? &reply : NULL, now);
    k_spin_unlock(&state_lock, key);
    k_mutex_unlock(&io_lock);
    queue_commands(&commands);
}

/* Slower once no key has changed for a while; see connected idle in
 * docs/power-and-wake.md. */
static int poll_delay_ms(void) {
    k_spinlock_key_t key = k_spin_lock(&state_lock);
    bool idle = defy_wired_bitmap_empty(transmitter.bitmap) &&
                k_uptime_get() - last_activity_ms >= CONFIG_DEFY_HYBRID_IDLE_TIMEOUT_MS;
    int delay = connected ? (idle ? CONFIG_DEFY_HYBRID_IDLE_CABLE_POLL_MS
                                  : CONFIG_DEFY_WIRED_POLL_INTERVAL_MS)
                          : (idle ? CONFIG_DEFY_HYBRID_IDLE_CABLE_PROBE_MS
                                  : CONFIG_DEFY_HYBRID_CABLE_PROBE_MS);

    k_spin_unlock(&state_lock, key);
    return delay;
}

static void run(void *a, void *b, void *c) {
    ARG_UNUSED(a);
    ARG_UNUSED(b);
    ARG_UNUSED(c);
    struct zmk_split_transport_status previous = {0};

    while (true) {
        publish_status(&previous);
        if (!polling_enabled()) {
            k_sem_take(&wake, K_FOREVER);
            continue;
        }
        poll_cable();
        publish_status(&previous);
        k_sem_take(&wake, K_MSEC(poll_delay_ms()));
    }
}

static int report_event(const struct zmk_split_transport_peripheral_event *event) {
    if (event->type != ZMK_SPLIT_TRANSPORT_PERIPHERAL_EVENT_TYPE_KEY_POSITION_EVENT) {
        return -ENOTSUP;
    }

    uint16_t position = event->data.key_position_event.position;
    bool pressed = event->data.key_position_event.pressed != 0;
    k_spinlock_key_t key = k_spin_lock(&state_lock);
    /* A real change ends the half's idle polling. */
    bool changed = ready &&
                   defy_wired_position_owned(DEFY_HALF_ID, position) &&
                   ((transmitter.bitmap[position / 8] & BIT(position % 8)) != 0) != pressed;
    int err = ready ? defy_wired_tx_set_key(&transmitter, position, pressed) : -ENODEV;
    bool queued = err == 0 || err == -ENOSPC;

    if (queued && !connected) {
        defy_wired_tx_resync(&transmitter);
    }
    if (changed && queued) {
        last_activity_ms = k_uptime_get();
    }
    k_spin_unlock(&state_lock, key);
    if (changed && queued) {
        k_sem_give(&wake);
    }
    if (err == -ENOSPC) {
        LOG_WRN("Cable key queue full; resending current keys");
    }
    return err;
}

static int set_enabled(bool value) {
    k_spinlock_key_t key = k_spin_lock(&state_lock);
    bool changed = enabled != value;

    if (value && !ready) {
        k_spin_unlock(&state_lock, key);
        return -ENODEV;
    }
    if (changed) {
        enabled = value;
        connected = false;
        feedback_pending = false;
        lighting_pending = false;
        lighting_ack_pending = false;
        battery_show_pending = false;
        defy_wired_tx_resync(&transmitter);
        last_activity_ms = k_uptime_get();
    }
    k_spin_unlock(&state_lock, key);
    if (changed) {
        k_sem_give(&wake);
    }
    return 0;
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

ZMK_SPLIT_TRANSPORT_PERIPHERAL_REGISTER(defy_wired_peripheral, &api, CONFIG_DEFY_WIRED_PRIORITY);

static int init(void) {
    if (!spi_is_ready_dt(&cable)) {
        return -ENODEV;
    }

    if (IS_ENABLED(CONFIG_DEFY_HALF_VERSION)) {
        prepare_version();
    }
    defy_wired_tx_init(&transmitter, DEFY_HALF_ID, k_cycle_get_32());
    k_spinlock_key_t key = k_spin_lock(&state_lock);

    ready = true;
    last_activity_ms = k_uptime_get();
    zmk_split_transport_peripheral_status_changed_cb_t callback = status_callback;

    k_spin_unlock(&state_lock, key);
    if (callback != NULL) {
        callback(&defy_wired_peripheral, transport_status());
    }
    k_thread_create(&thread, thread_stack, K_THREAD_STACK_SIZEOF(thread_stack), run, NULL, NULL,
                    NULL, K_PRIO_PREEMPT(CONFIG_DEFY_WIRED_PERIPHERAL_THREAD_PRIORITY), 0,
                    K_NO_WAIT);
    k_thread_name_set(&thread, "defy_cable");
    return 0;
}

SYS_INIT(init, APPLICATION, CABLE_INIT_PRIORITY);
