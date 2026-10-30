/* SPDX-License-Identifier: MIT */

#include <errno.h>
#include <string.h>

#include <zephyr/drivers/spi.h>
#include <zephyr/kernel.h>
#include <zephyr/ztest.h>

#include <zmk/events/split_peripheral_status_changed.h>
#include <zmk/split/transport/peripheral.h>

#include <dygma/defy_wired_protocol.h>
#include <dygma/defy_feedback.h>
#include <dygma/defy_lighting.h>
#include <dygma/defy_half_version.h>
#include <dygma/defy_battery.h>
#include <dygma/defy_half_leds.h>

static const struct defy_half_version local_version = {
    .side = DEFY_WIRED_LEFT, .program_crc = 0x0badcafe, .seal_version = 3,
    .module = "v0.1.0-dirty",
};
void defy_half_version_local(struct defy_half_version *version) { *version = local_version; }

static atomic_t battery_refresh_requests;
int defy_battery_hci_refresh(void) {
    atomic_inc(&battery_refresh_requests);
    return 0;
}

#include "defy_split_internal.h"

#define transport (defy_wired_peripheral.api)
static K_MUTEX_DEFINE(lock);
static struct {
    struct defy_wired_rx receiver;
    uint8_t reply[DEFY_WIRED_FRAME_SIZE];
    uint8_t received[128][DEFY_WIRED_BITMAP_SIZE];
    uint32_t sessions[128];
    size_t count;
    size_t exchanges;
    unsigned int dropped_replies;
    unsigned int status_changes;
    unsigned int connection_events;
    bool online;
    bool corrupt_reply;
    bool fail_transfer;
    bool resync_reply;
    bool display_once;
    bool display_repeat;
    uint8_t display_reply[DEFY_WIRED_FRAME_SIZE];
    uint32_t display_delay_ms;
} central;

static struct {
    struct defy_host_feedback state;
    size_t displays;
} feedback;

int defy_half_leds_host_show(const struct defy_host_feedback *state) {
    zassert_equal(k_current_get(), &k_sys_work_q.thread);
    zassert_true(transport->get_status().available);
    k_mutex_lock(&lock, K_FOREVER);
    feedback.state = *state;
    feedback.displays++;
    k_mutex_unlock(&lock);
    return 0;
}

/* The number of groups the cable passed on. */
static size_t groups_kept;

int defy_lighting_peripheral_group(const struct defy_lighting_group *group) {
    ARG_UNUSED(group);
    k_mutex_lock(&lock, K_FOREVER);
    groups_kept++;
    k_mutex_unlock(&lock);
    return 0;
}

static struct {
    struct defy_lighting_state applied;
    struct defy_wired_lighting_command ack;
    size_t applies;
    size_t acks;
    size_t key_snapshots;
    /* The groups kept when the last state was applied. */
    size_t groups_at_apply;
    uint32_t epoch;
    uint32_t key_sequence;
    int apply_error;
} lighting;

int defy_lighting_peripheral_apply(const struct defy_lighting_state *state, enum defy_link route,
                                   uint32_t route_epoch) {
    zassert_equal(k_current_get(), &k_sys_work_q.thread);
    zassert_equal(route, DEFY_LINK_WIRED);
    zassert_equal(route_epoch, state->boot);
    zassert_true(transport->get_status().available);
    k_mutex_lock(&lock, K_FOREVER);
    lighting.applies++;
    lighting.groups_at_apply = groups_kept;
    int err = lighting.apply_error;
    if (!err) {
        lighting.applied = *state;
        lighting.epoch = route_epoch;
    }
    k_mutex_unlock(&lock);
    return err;
}

bool defy_lighting_peripheral_get(struct defy_lighting_state *state) {
    k_mutex_lock(&lock, K_FOREVER);
    *state = lighting.applied;
    bool available = state->boot != 0;
    k_mutex_unlock(&lock);
    return available;
}

/* The 'DV' frames the Neuron received, and the keys at the last. */
static struct {
    size_t count;
    int64_t received_ms;
    size_t key_snapshots;
    struct defy_half_version version;
} reports;

static struct {
    bool available;
    size_t count;
    int64_t received_ms;
    uint32_t key_sequence;
    size_t key_snapshots;
    size_t displays;
    struct defy_battery_frame frame;
} battery;

int defy_half_leds_battery_show(void) {
    zassert_equal(k_current_get(), &k_sys_work_q.thread);
    zassert_true(transport->get_status().available);
    k_mutex_lock(&lock, K_FOREVER);
    battery.displays++;
    k_mutex_unlock(&lock);
    return 0;
}

int defy_battery_local_frame(struct defy_battery_frame *frame) {
    k_mutex_lock(&lock, K_FOREVER);
    bool available = battery.available;

    if (available) {
        *frame = (struct defy_battery_frame){
            .side = DEFY_WIRED_LEFT,
            .session = 0x12345678,
            .sample = {.sequence = 8, .words = {0x4031, 2, 0, 0, 0x5080, 0xc800, 0xff80,
                                                0x1284}},
        };
    }
    k_mutex_unlock(&lock);
    return available ? 0 : -ENODATA;
}

static int transfer(const struct device *dev, const struct spi_config *config,
                    const struct spi_buf_set *tx, const struct spi_buf_set *rx) {
    ARG_UNUSED(dev);
    zassert_not_null(tx);
    zassert_not_null(rx);
    zassert_equal(tx->count, 1);
    zassert_equal(rx->count, 1);
    zassert_equal(tx->buffers[0].len, DEFY_WIRED_FRAME_SIZE);
    zassert_equal(rx->buffers[0].len, DEFY_WIRED_FRAME_SIZE);
    zassert_equal(config->slave, 2);
    zassert_equal(config->operation, SPI_WORD_SET(8) | SPI_TRANSFER_MSB | SPI_MODE_CPHA);
    zassert_equal(config->frequency, 1000000);
    zassert_equal(config->cs.gpio.pin, 21);
    zassert_equal(config->cs.delay, 10);

    k_mutex_lock(&lock, K_FOREVER);
    central.exchanges++;
    if (central.fail_transfer) {
        k_mutex_unlock(&lock);
        return -EIO;
    }
    memset(rx->buffers[0].buf, 0xff, DEFY_WIRED_FRAME_SIZE);
    if (central.online) {
        memcpy(rx->buffers[0].buf, central.reply, DEFY_WIRED_FRAME_SIZE);
        if (central.display_once || central.display_repeat) {
            memcpy(rx->buffers[0].buf, central.display_reply, DEFY_WIRED_FRAME_SIZE);
            central.display_once = false;
            if (central.display_repeat) {
                k_sleep(K_MSEC(1));
            }
            if (central.display_delay_ms != 0) {
                uint32_t delay = central.display_delay_ms;
                central.display_delay_ms = 0;
                k_sleep(K_MSEC(delay));
            }
        }
        if (central.dropped_replies > 0) {
            central.dropped_replies--;
            memset(rx->buffers[0].buf, 0, DEFY_WIRED_FRAME_SIZE);
        } else if (central.corrupt_reply) {
            ((uint8_t *)rx->buffers[0].buf)[0] ^= 1;
        }
        const uint8_t *frame = tx->buffers[0].buf;

        if (frame[0] == 'D' && frame[1] == 'B') {
            zassert_ok(defy_battery_frame_decode(frame, DEFY_BATTERY_FRAME_SIZE,
                                                 &battery.frame));
            battery.count++;
            battery.received_ms = k_uptime_get();
            battery.key_sequence = central.receiver.sequence;
            battery.key_snapshots = central.count;
            k_mutex_unlock(&lock);
            return 0;
        }
        if (frame[0] == 'D' && frame[1] == 'V') {
            uint8_t record[DEFY_HALF_VERSION_SIZE];

            zassert_ok(defy_wired_half_version_decode(frame, DEFY_WIRED_FRAME_SIZE, record));
            zassert_true(defy_half_version_decode(record, &reports.version));
            reports.count++;
            reports.received_ms = k_uptime_get();
            reports.key_snapshots = central.count;
            k_mutex_unlock(&lock);
            return 0;
        }
        if (frame[0] == 'D' && frame[1] == 'L') {
            struct defy_wired_control control;
            zassert_ok(defy_wired_control_check(frame, DEFY_WIRED_FRAME_SIZE, &control));
            zassert_ok(defy_wired_lighting_decode(frame, &control, &lighting.ack));
            zassert_equal(lighting.ack.opcode, DEFY_CONTROL_LIGHTING_ACK);
            zassert_equal(lighting.ack.side, DEFY_WIRED_LEFT);
            zassert_equal(lighting.ack.session, central.receiver.session);
            lighting.acks++;
            lighting.key_snapshots = central.count;
            lighting.key_sequence = central.receiver.sequence;
            k_mutex_unlock(&lock);
            return 0;
        }
        struct defy_wired_packet packet;
        struct defy_wired_packet ack;

        zassert_ok(defy_wired_decode(tx->buffers[0].buf, DEFY_WIRED_FRAME_SIZE, &packet));
        zassert_equal(packet.side, DEFY_WIRED_LEFT);
        int result = defy_wired_rx_accept(&central.receiver, &packet, &ack);

        zassert_true(result >= 0);
        if (result == DEFY_WIRED_UPDATED) {
            zassert_true(central.count < ARRAY_SIZE(central.received));
            memcpy(central.received[central.count], packet.bitmap, DEFY_WIRED_BITMAP_SIZE);
            central.sessions[central.count++] = packet.session;
        }
        defy_wired_encode(&ack, central.reply);
        if (central.resync_reply) {
            ack.flags = DEFY_WIRED_FLAG_RESYNC;
            defy_wired_encode(&ack, rx->buffers[0].buf);
        }
    }
    k_mutex_unlock(&lock);
    return 0;
}

static DEVICE_API(spi, spi_api) = {.transceive = transfer};
SPI_DEVICE_DT_DEFINE(DT_NODELABEL(spi0), NULL, NULL, NULL, NULL, POST_KERNEL,
                     CONFIG_SPI_INIT_PRIORITY, &spi_api);

static int status_changed(const struct zmk_split_transport_peripheral *peripheral,
                          struct zmk_split_transport_status status) {
    zassert_equal(peripheral, &defy_wired_peripheral);
    ARG_UNUSED(status);
    zassert_true(transport->get_status().available);
    k_mutex_lock(&lock, K_FOREVER);
    central.status_changes++;
    k_mutex_unlock(&lock);
    return 0;
}

int raise_zmk_split_peripheral_status_changed(struct zmk_split_peripheral_status_changed event) {
    ARG_UNUSED(event);
    zassert_true(transport->get_status().available);
    k_mutex_lock(&lock, K_FOREVER);
    central.connection_events++;
    k_mutex_unlock(&lock);
    return 0;
}

static bool is_connected(void) {
    return transport->get_status().connections ==
           ZMK_SPLIT_TRANSPORT_CONNECTIONS_STATUS_ALL_CONNECTED;
}

static void await_connection(bool expected) {
    int64_t deadline = k_uptime_get() + 300;

    while (is_connected() != expected && k_uptime_get() < deadline) {
        k_sleep(K_MSEC(1));
    }
    zassert_equal(is_connected(), expected);
}

static size_t received_count(void) {
    k_mutex_lock(&lock, K_FOREVER);
    size_t count = central.count;

    k_mutex_unlock(&lock);
    return count;
}

static void await_received(size_t expected) {
    int64_t deadline = k_uptime_get() + 300;

    while (received_count() < expected && k_uptime_get() < deadline) {
        k_sleep(K_MSEC(1));
    }
    zassert_equal(received_count(), expected);
}

static int key_event(uint8_t position, bool pressed) {
    struct zmk_split_transport_peripheral_event event = {
        .type = ZMK_SPLIT_TRANSPORT_PERIPHERAL_EVENT_TYPE_KEY_POSITION_EVENT,
        .data.key_position_event = {.position = position, .pressed = pressed},
    };

    return transport->report_event(&event);
}

static void before(void *fixture) {
    ARG_UNUSED(fixture);
    defy_wired_peripheral_cancel_sleep();
    zassert_ok(transport->set_enabled(false));
    k_sleep(K_MSEC(10));
    for (uint8_t position = 0; position < DEFY_WIRED_KEY_COUNT; position++) {
        if (defy_wired_position_owned(DEFY_WIRED_LEFT, position)) {
            zassert_ok(key_event(position, false));
        }
    }
    k_mutex_lock(&lock, K_FOREVER);
    memset(&central, 0, sizeof(central));
    memset(&feedback, 0, sizeof(feedback));
    groups_kept = 0;
    memset(&lighting, 0, sizeof(lighting));
    memset(&battery, 0, sizeof(battery));
    memset(&reports, 0, sizeof(reports));
    central.online = true;
    k_mutex_unlock(&lock);
    zassert_ok(transport->set_status_callback(status_changed));
    zassert_ok(transport->set_enabled(true));
    await_connection(true);
    await_received(1);
}

ZTEST(defy_wired_peripheral, test_pipelined_heartbeat_preserves_connection) {
    k_sleep(K_MSEC(CONFIG_DEFY_WIRED_PERIPHERAL_TIMEOUT_MS * 2));
    zassert_true(is_connected());
    zassert_equal(received_count(), 1);
    k_mutex_lock(&lock, K_FOREVER);
    zassert_equal(central.connection_events, 0);
    zassert_true(central.exchanges > 2);
    zassert_true(central.status_changes > 0);
    k_mutex_unlock(&lock);
}

ZTEST(defy_wired_peripheral, test_short_tap_survives_lost_ack) {
    k_mutex_lock(&lock, K_FOREVER);
    central.dropped_replies = 8;
    k_mutex_unlock(&lock);
    zassert_ok(key_event(14, true));
    zassert_ok(key_event(14, false));
    await_received(3);
    k_sleep(K_MSEC(20));
    zassert_equal(received_count(), 3);
    k_mutex_lock(&lock, K_FOREVER);
    zassert_equal(central.received[1][1], BIT(6));
    zassert_equal(central.received[2][1], 0);
    k_mutex_unlock(&lock);
    zassert_true(is_connected());
}

ZTEST(defy_wired_peripheral, test_foreign_positions_and_non_key_events_rejected) {
    zassert_equal(key_event(7, true), -EINVAL);
    zassert_equal(key_event(70, true), -EINVAL);
    struct zmk_split_transport_peripheral_event event = {
        .type = ZMK_SPLIT_TRANSPORT_PERIPHERAL_EVENT_TYPE_BATTERY_EVENT,
        .data.battery_event = {.level = 99},
    };

    zassert_equal(transport->report_event(&event), -ENOTSUP);
    k_sleep(K_MSEC(10));
    zassert_equal(received_count(), 1);
}

ZTEST(defy_wired_peripheral, test_corrupt_replies_timeout_and_recover_current_keys) {
    k_mutex_lock(&lock, K_FOREVER);
    central.corrupt_reply = true;
    k_mutex_unlock(&lock);
    await_connection(false);
    zassert_ok(key_event(14, true));
    zassert_ok(key_event(14, false));
    zassert_ok(key_event(28, true));
    k_mutex_lock(&lock, K_FOREVER);
    central.corrupt_reply = false;
    defy_wired_rx_init(&central.receiver);
    memset(central.reply, 0, sizeof(central.reply));
    central.count = 0;
    k_mutex_unlock(&lock);
    await_connection(true);
    await_received(1);
    k_mutex_lock(&lock, K_FOREVER);
    zassert_equal(central.received[0][1], 0);
    zassert_equal(central.received[0][3], BIT(4));
    k_mutex_unlock(&lock);
}

ZTEST(defy_wired_peripheral, test_transfer_error_timeout_and_reconnect) {
    k_mutex_lock(&lock, K_FOREVER);
    central.fail_transfer = true;
    k_mutex_unlock(&lock);
    await_connection(false);
    k_mutex_lock(&lock, K_FOREVER);
    central.fail_transfer = false;
    k_mutex_unlock(&lock);
    await_connection(true);
}

ZTEST(defy_wired_peripheral, test_central_restart_resynchronizes_held_key) {
    zassert_ok(key_event(0, true));
    await_received(2);
    k_sleep(K_MSEC(10));
    k_mutex_lock(&lock, K_FOREVER);
    uint32_t old_session = central.receiver.session;

    defy_wired_rx_init(&central.receiver);
    memset(central.reply, 0, sizeof(central.reply));
    k_mutex_unlock(&lock);
    await_received(3);
    await_connection(true);
    k_mutex_lock(&lock, K_FOREVER);
    zassert_not_equal(central.sessions[2], old_session);
    zassert_equal(central.received[2][0], BIT(0));
    k_mutex_unlock(&lock);
}

ZTEST(defy_wired_peripheral, test_queue_overflow_resynchronizes_without_stuck_key) {
    unsigned int overflows = 0;

    k_mutex_lock(&lock, K_FOREVER);
    central.dropped_replies = 8;
    k_mutex_unlock(&lock);
    for (size_t i = 0; i < DEFY_WIRED_QUEUE_CAPACITY + 2; i++) {
        int err = key_event(14, (i % 2) == 0);

        zassert_true(err == 0 || err == -ENOSPC);
        overflows += err == -ENOSPC;
    }
    zassert_equal(overflows, 1);
    k_sleep(K_MSEC(80));
    zassert_true(is_connected());
    k_mutex_lock(&lock, K_FOREVER);
    zassert_equal(central.receiver.bitmap[1], 0);
    k_mutex_unlock(&lock);
}

ZTEST(defy_wired_peripheral, test_disable_stops_polling_and_reenable_synchronizes) {
    zassert_ok(transport->set_enabled(false));
    k_sleep(K_MSEC(10));
    zassert_false(is_connected());
    k_mutex_lock(&lock, K_FOREVER);
    size_t exchanges = central.exchanges;

    k_mutex_unlock(&lock);
    zassert_ok(key_event(28, true));
    k_sleep(K_MSEC(20));
    k_mutex_lock(&lock, K_FOREVER);
    zassert_equal(central.exchanges, exchanges);
    k_mutex_unlock(&lock);
    zassert_ok(transport->set_enabled(true));
    await_connection(true);
    k_mutex_lock(&lock, K_FOREVER);
    zassert_equal(central.receiver.bitmap[3], BIT(4));
    k_mutex_unlock(&lock);
}

ZTEST(defy_wired_peripheral, test_recent_resync_ack_vetoes_until_reply_expires) {
    k_mutex_lock(&lock, K_FOREVER);
    central.resync_reply = true;
    k_mutex_unlock(&lock);
    await_connection(false);
    k_mutex_lock(&lock, K_FOREVER);
    central.online = false;
    k_mutex_unlock(&lock);
    zassert_equal(defy_wired_peripheral_prepare_sleep(), -EBUSY);
    zassert_false(is_connected());
    k_sleep(K_MSEC(CONFIG_DEFY_WIRED_PERIPHERAL_TIMEOUT_MS + 5));
    zassert_ok(defy_wired_peripheral_prepare_sleep());
    defy_wired_peripheral_cancel_sleep();
}

/* The Neuron's CONFIG_DEFY_WIRED_CENTRAL_TIMEOUT_MS default: a half notices a lost cable
 * before the Neuron gives up on it. */
#define NEURON_CABLE_TIMEOUT_MS 250

static size_t exchange_count(void) {
    k_mutex_lock(&lock, K_FOREVER);
    size_t count = central.exchanges;

    k_mutex_unlock(&lock);
    return count;
}

static void await_next_exchange(void) {
    size_t before = exchange_count();
    int64_t deadline = k_uptime_get() + CONFIG_DEFY_HYBRID_IDLE_CABLE_PROBE_MS + 20;

    while (exchange_count() == before && k_uptime_get() < deadline) {
        k_sleep(K_MSEC(1));
    }
    zassert_true(exchange_count() > before);
}

static void enter_idle(void) {
    k_sleep(K_MSEC(CONFIG_DEFY_HYBRID_IDLE_TIMEOUT_MS +
                   CONFIG_DEFY_HYBRID_IDLE_CABLE_POLL_MS + 5));
    await_next_exchange();
}

static void set_online(bool online) {
    k_mutex_lock(&lock, K_FOREVER);
    central.online = online;
    k_mutex_unlock(&lock);
}

ZTEST(defy_wired_peripheral, test_idle_connected_polling_keeps_ack_heartbeat) {
    enter_idle();
    size_t before = exchange_count();

    k_sleep(K_MSEC(80));
    size_t polls = exchange_count() - before;

    zassert_true(polls >= 3 && polls <= 5, "Idle polls: %zu", polls);
    zassert_true(is_connected());
    zassert_equal(received_count(), 1);
}

ZTEST(defy_wired_peripheral, test_first_idle_tap_interrupts_wait_and_keeps_both_edges) {
    enter_idle();
    int64_t started = k_uptime_get();

    zassert_ok(key_event(14, true));
    zassert_ok(key_event(14, false));
    await_received(3);
    zassert_true(k_uptime_get() - started < CONFIG_DEFY_HYBRID_IDLE_CABLE_POLL_MS);
    k_mutex_lock(&lock, K_FOREVER);
    zassert_equal(central.received[1][1], BIT(6));
    zassert_equal(central.received[2][1], 0);
    k_mutex_unlock(&lock);
    size_t before = exchange_count();

    k_sleep(K_MSEC(20));
    zassert_true(exchange_count() - before >= 7);
}

ZTEST(defy_wired_peripheral, test_held_key_prevents_idle_until_release_timeout) {
    zassert_ok(key_event(28, true));
    await_received(2);
    enter_idle();
    size_t before = exchange_count();

    k_sleep(K_MSEC(20));
    zassert_true(exchange_count() - before >= 7);
    zassert_ok(key_event(28, false));
    await_received(3);
    k_sleep(K_MSEC(CONFIG_DEFY_HYBRID_IDLE_TIMEOUT_MS / 2));
    before = exchange_count();
    k_sleep(K_MSEC(20));
    zassert_true(exchange_count() - before >= 7);
    enter_idle();
    before = exchange_count();
    k_sleep(K_MSEC(60));
    zassert_true(exchange_count() - before <= 4);
}

ZTEST(defy_wired_peripheral, test_noop_and_invalid_events_do_not_restart_idle) {
    enter_idle();
    size_t before = exchange_count();

    for (int i = 0; i < 60; i++) {
        zassert_ok(key_event(14, false));
        zassert_equal(key_event(7, true), -EINVAL);
        zassert_equal(key_event(70, true), -EINVAL);
        zassert_ok(transport->set_enabled(true));
        k_sleep(K_MSEC(1));
    }
    zassert_true(exchange_count() - before <= 4);
    zassert_equal(received_count(), 1);
}

ZTEST(defy_wired_peripheral, test_idle_cable_loss_and_background_reconnection) {
    enter_idle();
    int64_t started = k_uptime_get();

    set_online(false);
    await_connection(false);
    zassert_true(k_uptime_get() - started <= CONFIG_DEFY_WIRED_PERIPHERAL_TIMEOUT_MS +
                                              CONFIG_DEFY_HYBRID_IDLE_CABLE_POLL_MS + 5);
    zassert_true(k_uptime_get() - started < NEURON_CABLE_TIMEOUT_MS);
    await_next_exchange();
    size_t before = exchange_count();

    k_sleep(K_MSEC(250));
    size_t probes = exchange_count() - before;

    zassert_true(probes >= 2 && probes <= 3, "Idle probes: %zu", probes);
    started = k_uptime_get();
    set_online(true);
    await_connection(true);
    zassert_true(k_uptime_get() - started <= 2 * CONFIG_DEFY_HYBRID_IDLE_CABLE_PROBE_MS + 5);
}

ZTEST(defy_wired_peripheral, test_key_wakes_idle_absent_probe_immediately) {
    enter_idle();
    set_online(false);
    await_connection(false);
    await_next_exchange();
    size_t before = exchange_count();
    int64_t started = k_uptime_get();

    set_online(true);
    zassert_ok(key_event(28, true));
    while (exchange_count() == before && k_uptime_get() - started < 10) {
        k_sleep(K_MSEC(1));
    }
    zassert_true(exchange_count() > before);
    zassert_true(k_uptime_get() - started < 10);
    await_connection(true);
    zassert_true(k_uptime_get() - started <= 2 * CONFIG_DEFY_HYBRID_CABLE_PROBE_MS + 5);
    k_mutex_lock(&lock, K_FOREVER);
    zassert_equal(central.receiver.bitmap[3], BIT(4));
    k_mutex_unlock(&lock);
}

ZTEST(defy_wired_peripheral, test_idle_disable_and_reenable_reset_activity) {
    enter_idle();
    zassert_ok(transport->set_enabled(false));
    k_sleep(K_MSEC(5));
    size_t before = exchange_count();

    zassert_ok(key_event(28, true));
    zassert_ok(key_event(28, false));
    k_sleep(K_MSEC(30));
    zassert_equal(exchange_count(), before);
    int64_t started = k_uptime_get();

    zassert_ok(transport->set_enabled(true));
    await_connection(true);
    zassert_true(k_uptime_get() - started <= 2 * CONFIG_DEFY_HYBRID_CABLE_PROBE_MS + 5);
    before = exchange_count();
    k_sleep(K_MSEC(20));
    zassert_true(exchange_count() - before >= 7);
}

static void await_battery(size_t count) {
    int64_t deadline = k_uptime_get() + 5100;

    while (k_uptime_get() < deadline) {
        k_mutex_lock(&lock, K_FOREVER);
        bool received = battery.count >= count;
        k_mutex_unlock(&lock);
        if (received) {
            return;
        }
        k_sleep(K_MSEC(1));
    }
    zassert_unreachable("Battery frame did not arrive");
}

ZTEST(defy_wired_peripheral, test_cable_changes_refresh_battery_but_heartbeats_do_not) {
    k_sleep(K_MSEC(10));
    atomic_val_t baseline = atomic_get(&battery_refresh_requests);
    k_sleep(K_MSEC(80));
    zassert_equal(atomic_get(&battery_refresh_requests), baseline);
    set_online(false);
    await_connection(false);
    k_sleep(K_MSEC(10));
    zassert_equal(atomic_get(&battery_refresh_requests), baseline + 1);
    set_online(true);
    await_connection(true);
    k_sleep(K_MSEC(10));
    zassert_equal(atomic_get(&battery_refresh_requests), baseline + 2);
}

ZTEST(defy_wired_peripheral, test_battery_is_periodic_without_key_or_activity_changes) {
    k_mutex_lock(&lock, K_FOREVER);
    battery.available = true;
    k_mutex_unlock(&lock);
    await_battery(1);
    k_mutex_lock(&lock, K_FOREVER);
    int64_t first = battery.received_ms;

    zassert_equal(battery.key_sequence, 0);
    zassert_equal(battery.key_snapshots, 1);
    zassert_equal(battery.frame.session, 0x12345678);
    zassert_equal(battery.frame.sample.sequence, 8);
    k_mutex_unlock(&lock);
    k_sleep(K_MSEC(4900));
    k_mutex_lock(&lock, K_FOREVER);
    zassert_equal(battery.count, 1);
    k_mutex_unlock(&lock);
    await_battery(2);
    k_mutex_lock(&lock, K_FOREVER);
    zassert_between_inclusive(battery.received_ms - first, 5000, 5025);
    zassert_equal(central.count, 1);
    zassert_equal(central.receiver.sequence, 0);
    k_mutex_unlock(&lock);
    zassert_true(is_connected());
}

ZTEST(defy_wired_peripheral, test_battery_waits_for_key_queue_and_acknowledgement) {
    k_sleep(K_MSEC(5010));
    k_mutex_lock(&lock, K_FOREVER);
    central.dropped_replies = 8;
    battery.available = true;
    zassert_ok(key_event(14, true));
    zassert_ok(key_event(14, false));
    k_mutex_unlock(&lock);
    await_battery(1);
    k_mutex_lock(&lock, K_FOREVER);
    zassert_equal(battery.key_snapshots, 3);
    zassert_equal(battery.key_sequence, 2);
    zassert_equal(central.received[1][1], BIT(6));
    zassert_equal(central.received[2][1], 0);
    zassert_equal(central.receiver.sequence, 2);
    k_mutex_unlock(&lock);
    zassert_ok(key_event(28, true));
    await_received(4);
    k_mutex_lock(&lock, K_FOREVER);
    zassert_equal(central.receiver.sequence, 3);
    zassert_equal(central.received[3][3], BIT(4));
    k_mutex_unlock(&lock);
}

/* The Neuron's next reply asks this half to show its battery. */
static void queue_display(void) {
    k_mutex_lock(&lock, K_FOREVER);
    struct defy_wired_battery_show_command command = {
        .side = DEFY_WIRED_LEFT,
        .session = central.receiver.session,
    };
    zassert_ok(defy_wired_battery_show_encode(&command, central.display_reply));
    central.display_once = true;
    k_mutex_unlock(&lock);
}

static size_t display_count(void) {
    k_mutex_lock(&lock, K_FOREVER);
    size_t count = battery.displays;
    k_mutex_unlock(&lock);
    return count;
}

static void await_display(size_t count) {
    int64_t deadline = k_uptime_get() + 200;
    while (display_count() < count && k_uptime_get() < deadline) {
        k_sleep(K_MSEC(1));
    }
    zassert_equal(display_count(), count);
}

ZTEST(defy_wired_peripheral, test_display_runs_once_on_system_work) {
    queue_display();
    await_display(1);
    k_sleep(K_MSEC(30));
    zassert_equal(display_count(), 1);
    zassert_true(is_connected());
}

ZTEST(defy_wired_peripheral, test_repeated_display_frames_cannot_renew_ack_liveness) {
    queue_display();
    k_mutex_lock(&lock, K_FOREVER);
    central.display_repeat = true;
    k_mutex_unlock(&lock);
    await_connection(false);
    size_t count = display_count();
    k_sleep(K_MSEC(50));
    zassert_equal(display_count(), count);
    zassert_false(is_connected());
    k_mutex_lock(&lock, K_FOREVER);
    central.display_repeat = false;
    k_mutex_unlock(&lock);
    await_connection(true);
}

ZTEST(defy_wired_peripheral, test_stale_armed_display_is_rejected_after_long_transfer_gap) {
    k_mutex_lock(&lock, K_FOREVER);
    central.display_delay_ms = DEFY_CONTROL_TIMEOUT_MS + 50;
    k_mutex_unlock(&lock);
    queue_display();
    k_sleep(K_MSEC(DEFY_CONTROL_TIMEOUT_MS + 100));
    zassert_equal(display_count(), 0);
    await_connection(true);
    zassert_ok(key_event(28, true));
    k_sleep(K_MSEC(30));
    k_mutex_lock(&lock, K_FOREVER);
    zassert_true((central.receiver.bitmap[3] & BIT(4)) != 0);
    k_mutex_unlock(&lock);
}

static K_SEM_DEFINE(display_block_started, 0, 1);
static K_SEM_DEFINE(display_block_release, 0, 1);

static void block_display_workqueue(struct k_work *work) {
    ARG_UNUSED(work);
    k_sem_give(&display_block_started);
    k_sem_take(&display_block_release, K_FOREVER);
}

static K_WORK_DEFINE(display_block_work, block_display_workqueue);

static void queue_lighting(uint8_t opcode, uint8_t side, int session_delta,
                            uint32_t boot, uint32_t revision, bool enabled, bool corrupt) {
    k_mutex_lock(&lock, K_FOREVER);
    struct defy_wired_lighting_command command = {
        .opcode = opcode, .side = side, .session = central.receiver.session + session_delta,
        .state = {.boot = boot, .revision = revision, .enabled = enabled},
    };
    zassert_ok(defy_wired_lighting_encode(&command, central.display_reply));
    if (corrupt) {
        central.display_reply[20] ^= 1;
    }
    central.display_once = true;
    k_mutex_unlock(&lock);
}

static void await_lighting(size_t applies, size_t acks) {
    int64_t deadline = k_uptime_get() + 300;
    while (k_uptime_get() < deadline) {
        k_mutex_lock(&lock, K_FOREVER);
        bool done = lighting.applies == applies && lighting.acks == acks;
        k_mutex_unlock(&lock);
        if (done) {
            return;
        }
        k_sleep(K_MSEC(1));
    }
    zassert_equal(lighting.applies, applies);
    zassert_equal(lighting.acks, acks);
}

static void block_lighting(void) {
    k_sem_reset(&display_block_started);
    k_sem_reset(&display_block_release);
    k_work_submit(&display_block_work);
    zassert_ok(k_sem_take(&display_block_started, K_MSEC(100)));
}

/* The Neuron's next reply carries the host display with sequence, shown for duration ms. */
static void queue_feedback(uint32_t sequence, uint16_t duration) {
    k_mutex_lock(&lock, K_FOREVER);
    struct defy_wired_host_feedback command = {
        .side = DEFY_WIRED_LEFT, .session = central.receiver.session,
        .state = {.boot = 321, .sequence = sequence, .remaining_ms = duration,
                   .profile = 2, .flags = DEFY_HOST_SELECTED_USB | DEFY_HOST_USB_READY},
    };
    zassert_ok(defy_wired_host_feedback_encode(&command, central.display_reply));
    central.display_once = true;
    k_mutex_unlock(&lock);
}

static size_t feedback_count(void) {
    k_mutex_lock(&lock, K_FOREVER);
    size_t result = feedback.displays;
    k_mutex_unlock(&lock);
    return result;
}

static void await_feedback(size_t count) {
    int64_t deadline = k_uptime_get() + 300;
    while (feedback_count() < count && k_uptime_get() < deadline) {
        k_sleep(K_MSEC(1));
    }
    zassert_equal(feedback_count(), count);
}

ZTEST(defy_wired_peripheral, test_feedback_runs_outside_transfer) {
    queue_feedback(1, 2000);
    await_feedback(1);
    k_mutex_lock(&lock, K_FOREVER);
    zassert_equal(feedback.state.boot, 321);
    zassert_equal(feedback.state.sequence, 1);
    zassert_equal(feedback.state.profile, 2);
    zassert_equal(feedback.state.flags, DEFY_HOST_SELECTED_USB | DEFY_HOST_USB_READY);
    zassert_between_inclusive(feedback.state.remaining_ms, 1900, 2000);
    k_mutex_unlock(&lock);
    k_sleep(K_MSEC(30));
    zassert_equal(feedback_count(), 1);
    zassert_true(is_connected());
}

ZTEST(defy_wired_peripheral, test_feedback_pending_mailbox_keeps_newest_and_remaining_age) {
    block_lighting();
    queue_feedback(2, 1000);
    k_sleep(K_MSEC(30));
    queue_feedback(1, 2000);
    k_sleep(K_MSEC(30));
    queue_feedback(2, 2000);
    k_sleep(K_MSEC(30));
    k_sem_give(&display_block_release);
    await_feedback(1);
    k_mutex_lock(&lock, K_FOREVER);
    zassert_equal(feedback.state.sequence, 2);
    zassert_between_inclusive(feedback.state.remaining_ms, 900, 950);
    k_mutex_unlock(&lock);
}

ZTEST(defy_wired_peripheral, test_feedback_expires_while_queued_without_extending_led_time) {
    block_lighting();
    queue_feedback(1, 50);
    k_sleep(K_MSEC(100));
    k_sem_give(&display_block_release);
    k_sleep(K_MSEC(10));
    zassert_equal(feedback_count(), 0);
    zassert_true(is_connected());
}

ZTEST(defy_wired_peripheral, test_feedback_pending_work_rejects_disconnect_and_new_session) {
    block_lighting();
    queue_feedback(1, 2000);
    k_sleep(K_MSEC(30));
    zassert_ok(transport->set_enabled(false));
    zassert_ok(transport->set_enabled(true));
    await_connection(true);
    k_sem_give(&display_block_release);
    k_sleep(K_MSEC(10));
    zassert_equal(feedback_count(), 0);
    block_lighting();
    queue_feedback(2, 2000);
    k_sleep(K_MSEC(30));
    k_mutex_lock(&lock, K_FOREVER);
    uint32_t old_session = central.receiver.session;
    central.resync_reply = true;
    k_mutex_unlock(&lock);
    k_sleep(K_MSEC(30));
    k_mutex_lock(&lock, K_FOREVER);
    central.resync_reply = false;
    zassert_not_equal(central.receiver.session, old_session);
    k_mutex_unlock(&lock);
    k_sem_give(&display_block_release);
    k_sleep(K_MSEC(10));
    zassert_equal(feedback_count(), 0);
    await_connection(true);
}

ZTEST(defy_wired_peripheral, test_lighting_applies_on_system_work_after_short_tap_is_retained) {
    queue_lighting(DEFY_CONTROL_LIGHTING_SET, 0, 0, 101, 1, false, false);
    zassert_ok(key_event(14, true));
    zassert_ok(key_event(14, false));
    await_lighting(1, 1);
    zassert_equal(lighting.ack.state.boot, 101);
    zassert_equal(lighting.ack.state.revision, 1);
    zassert_false(lighting.ack.state.enabled);
    zassert_equal(lighting.key_snapshots, 3);
    zassert_equal(lighting.key_sequence, 2);
    zassert_equal(received_count(), 3);
    zassert_true(is_connected());
    k_sleep(K_MSEC(60));
    await_lighting(1, 1);
    queue_lighting(DEFY_CONTROL_LIGHTING_SET, 0, 0, 101, 1, false, false);
    await_lighting(2, 2);
}

ZTEST(defy_wired_peripheral, test_lighting_rejects_wrong_kind_side_session_and_corruption) {
    queue_lighting(DEFY_CONTROL_LIGHTING_ACK, 0, 0, 101, 1, false, false);
    k_sleep(K_MSEC(30));
    queue_lighting(DEFY_CONTROL_LIGHTING_SET, 1, 0, 101, 1, false, false);
    k_sleep(K_MSEC(30));
    queue_lighting(DEFY_CONTROL_LIGHTING_SET, 0, 1, 101, 1, false, false);
    k_sleep(K_MSEC(30));
    queue_lighting(DEFY_CONTROL_LIGHTING_SET, 0, 0, 101, 1, false, true);
    k_sleep(K_MSEC(30));
    await_lighting(0, 0);
    zassert_true(is_connected());
}

ZTEST(defy_wired_peripheral, test_lighting_mailbox_keeps_newest_revision_until_worker_runs) {
    block_lighting();
    queue_lighting(DEFY_CONTROL_LIGHTING_SET, 0, 0, 101, 2, false, false);
    k_sleep(K_MSEC(30));
    queue_lighting(DEFY_CONTROL_LIGHTING_SET, 0, 0, 101, 1, true, false);
    k_sleep(K_MSEC(30));
    queue_lighting(DEFY_CONTROL_LIGHTING_SET, 0, 0, 101, 2, true, false);
    k_sleep(K_MSEC(30));
    k_sem_give(&display_block_release);
    await_lighting(1, 1);
    zassert_equal(lighting.applied.revision, 2);
    zassert_false(lighting.applied.enabled);
    zassert_equal(lighting.ack.state.revision, 2);
}

ZTEST(defy_wired_peripheral, test_lighting_apply_failure_is_not_acknowledged_and_can_retry) {
    k_mutex_lock(&lock, K_FOREVER);
    lighting.apply_error = -EAGAIN;
    k_mutex_unlock(&lock);
    queue_lighting(DEFY_CONTROL_LIGHTING_SET, 0, 0, 101, 1, false, false);
    await_lighting(1, 0);
    k_sleep(K_MSEC(30));
    await_lighting(1, 0);
    k_mutex_lock(&lock, K_FOREVER);
    lighting.apply_error = 0;
    k_mutex_unlock(&lock);
    queue_lighting(DEFY_CONTROL_LIGHTING_SET, 0, 0, 101, 1, false, false);
    await_lighting(2, 1);
}

ZTEST(defy_wired_peripheral, test_new_central_boot_is_control_epoch_with_same_key_session) {
    queue_lighting(DEFY_CONTROL_LIGHTING_SET, 0, 0, 101, 4, false, false);
    await_lighting(1, 1);
    uint32_t session = lighting.ack.session;
    queue_lighting(DEFY_CONTROL_LIGHTING_SET, 0, 0, 202, 1, true, false);
    await_lighting(2, 2);
    zassert_equal(lighting.ack.session, session);
    zassert_equal(lighting.epoch, 202);
    zassert_true(lighting.ack.state.enabled);
}

/* The Neuron's next reply carries the group with index, for side in the session plus delta. */
static void queue_group(uint8_t side, int session_delta, uint8_t index, bool corrupt) {
    k_mutex_lock(&lock, K_FOREVER);
    struct defy_wired_lighting_group_command command = {
        .side = side, .session = central.receiver.session + session_delta,
        .group = {.boot = 101, .index = index, .colour = {index, 2, 3}, .leds = BIT64(index)},
    };
    zassert_ok(defy_wired_lighting_group_encode(&command, central.display_reply));
    if (corrupt) {
        central.display_reply[20] ^= 1;
    }
    central.display_once = true;
    k_mutex_unlock(&lock);
}

static size_t group_count(void) {
    k_mutex_lock(&lock, K_FOREVER);
    size_t count = groups_kept;
    k_mutex_unlock(&lock);
    return count;
}

static void await_groups(size_t count) {
    int64_t deadline = k_uptime_get() + 300;
    while (group_count() < count && k_uptime_get() < deadline) {
        k_sleep(K_MSEC(1));
    }
    zassert_equal(group_count(), count);
}

/* Waits until the half has the queued reply. */
static void await_reply_taken(void) {
    int64_t deadline = k_uptime_get() + 300;
    bool queued = true;
    while (queued && k_uptime_get() < deadline) {
        k_mutex_lock(&lock, K_FOREVER);
        queued = central.display_once;
        k_mutex_unlock(&lock);
        k_sleep(K_MSEC(1));
    }
    zassert_false(queued);
}

ZTEST(defy_wired_peripheral, test_lighting_group_rejects_other_side_session_and_corruption) {
    queue_group(DEFY_WIRED_RIGHT, 0, 4, false);
    await_reply_taken();
    queue_group(DEFY_WIRED_LEFT, 1, 4, false);
    await_reply_taken();
    queue_group(DEFY_WIRED_LEFT, 0, 4, true);
    await_reply_taken();
    k_sleep(K_MSEC(30));
    zassert_equal(group_count(), 0);
    zassert_true(is_connected());
}

ZTEST(defy_wired_peripheral, test_lighting_group_is_kept_before_the_state_that_follows) {
    /* The work queue is busy: the group is kept anyway, the state waits. */
    block_lighting();
    queue_group(DEFY_WIRED_LEFT, 0, 3, false);
    await_groups(1);
    k_mutex_lock(&lock, K_FOREVER);
    struct defy_wired_lighting_command command = {
        .opcode = DEFY_CONTROL_LIGHTING_SET, .side = DEFY_WIRED_LEFT,
        .session = central.receiver.session,
        .state = {.boot = 101, .revision = 1, .enabled = true, .groups = BIT(3), .level = 40},
    };
    zassert_ok(defy_wired_lighting_encode(&command, central.display_reply));
    central.display_once = true;
    k_mutex_unlock(&lock);
    await_reply_taken();
    await_lighting(0, 0);
    k_sem_give(&display_block_release);
    await_lighting(1, 1);
    zassert_equal(lighting.groups_at_apply, 1);
    zassert_equal(lighting.ack.state.groups, BIT(3));
    zassert_equal(lighting.ack.state.level, 40);
}

static size_t report_count(void) {
    k_mutex_lock(&lock, K_FOREVER);
    size_t count = reports.count;

    k_mutex_unlock(&lock);
    return count;
}

static void await_report(size_t count) {
    int64_t deadline = k_uptime_get() + 300;

    while (report_count() < count && k_uptime_get() < deadline) {
        k_sleep(K_MSEC(1));
    }
    zassert_equal(report_count(), count);
}

/* Sleeps until ms after the last report arrived. */
static void sleep_after_report(int64_t ms) {
    k_mutex_lock(&lock, K_FOREVER);
    int64_t wake = reports.received_ms + ms;

    k_mutex_unlock(&lock);
    k_sleep(K_MSEC(MAX(0, wake - k_uptime_get())));
}

ZTEST(defy_wired_peripheral, test_version_goes_in_first_idle_frame_then_every_minute) {
    await_report(1);
    k_mutex_lock(&lock, K_FOREVER);
    zassert_equal(reports.version.side, DEFY_WIRED_LEFT);
    zassert_equal(reports.version.program_crc, 0x0badcafe);
    zassert_equal(reports.version.seal_version, 3);
    zassert_str_equal(reports.version.module, "v0.1.0-dirty");
    zassert_equal(reports.key_snapshots, 1);
    int64_t first = reports.received_ms;
    k_mutex_unlock(&lock);
    sleep_after_report(59900);
    zassert_equal(report_count(), 1);
    zassert_true(is_connected());
    await_report(2);
    k_mutex_lock(&lock, K_FOREVER);
    zassert_between_inclusive(reports.received_ms - first, 60000,
                              60000 + CONFIG_DEFY_HYBRID_IDLE_CABLE_POLL_MS + 5);
    zassert_equal(central.count, 1);
    k_mutex_unlock(&lock);
    k_sleep(K_MSEC(1000));
    zassert_equal(report_count(), 2);
    zassert_true(is_connected());
}

ZTEST(defy_wired_peripheral, test_version_goes_again_when_the_link_comes_back) {
    await_report(1);
    set_online(false);
    await_connection(false);
    k_sleep(K_MSEC(50));
    zassert_equal(report_count(), 1);
    int64_t started = k_uptime_get();

    set_online(true);
    await_connection(true);
    await_report(2);
    /* Within the probe that finds the cable and two polls, not a minute. */
    k_mutex_lock(&lock, K_FOREVER);
    zassert_true(reports.received_ms - started <= 2 * CONFIG_DEFY_HYBRID_IDLE_CABLE_PROBE_MS +
                                                  2 * CONFIG_DEFY_HYBRID_IDLE_CABLE_POLL_MS + 5);
    k_mutex_unlock(&lock);
    k_sleep(K_MSEC(1000));
    zassert_equal(report_count(), 2);
}

ZTEST_SUITE(defy_wired_peripheral, NULL, NULL, before, NULL, NULL);
