/* SPDX-License-Identifier: MIT */

#include <errno.h>
#include <string.h>

#include <zephyr/device.h>
#include <zephyr/drivers/spi.h>
#include <zephyr/init.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/atomic.h>
#include <zephyr/ztest.h>

#include <dygma/defy_wired_protocol.h>
#include <dygma/defy_battery_central.h>
#include <dygma/defy_half_version.h>
#include <dygma/defy_lighting.h>
#include <zmk/split/transport/central.h>

#include "fake_spi.h"

static bool lighting_ready;
static bool global_maintenance;
static struct defy_lighting_state desired_lighting;

bool defy_hybrid_central_is_maintenance(void) { return global_maintenance; }

bool defy_lighting_central_get(struct defy_lighting_state *state) {
    *state = desired_lighting;
    return lighting_ready;
}

/* The groups the cable asked for, and an error to give instead. */
static unsigned int group_calls;
static int group_error;

int defy_lighting_central_group(uint8_t side, uint8_t index, struct defy_lighting_group *group) {
    group_calls++;
    if (group_error) {
        return group_error;
    }
    *group = (struct defy_lighting_group){
        .boot = desired_lighting.boot, .index = index, .colour = {index, side, 0x80},
        .leds = BIT64(index) | BIT64(34 - side),
    };
    return 0;
}

#undef SYS_INIT
#define SYS_INIT(fn, level, priority)
#include "../../../../drivers/split/defy_wired_central.c"

struct queued_snapshot {
    uint8_t side;
    uint32_t session;
    bool connected;
    uint8_t bitmap[DEFY_WIRED_BITMAP_SIZE];
};

static struct queued_snapshot snapshots[32];
static size_t snapshot_count;
static uint32_t trace_clock;
static struct {
    uint8_t kind;
    uint8_t flags;
    uint16_t value;
    uint32_t extra;
    size_t preceding_snapshots;
} traces[16];
static size_t trace_count;

uint32_t defy_key_trace_now(void) { return trace_clock; }

void defy_key_trace_add(uint8_t kind, uint8_t flags, uint16_t value, uint32_t extra) {
    zassert_true(trace_count < ARRAY_SIZE(traces));
    traces[trace_count].kind = kind;
    traces[trace_count].flags = flags;
    traces[trace_count].value = value;
    traces[trace_count].extra = extra;
    traces[trace_count++].preceding_snapshots = snapshot_count;
}
static size_t attempts;
static int failure_at;
static int failures_left;
static struct {
    bool live;
    uint8_t side;
    uint32_t epoch;
    size_t connect_count;
    size_t disconnect_count;
    size_t accept_attempts;
    size_t accepted;
    struct defy_battery_frame frame;
} battery_routes[2];

int defy_battery_central_route_live(enum defy_link route, uint8_t source,
                                   uint32_t epoch, uint8_t side) {
    zassert_equal(route, DEFY_LINK_WIRED);
    zassert_true(source < ARRAY_SIZE(battery_routes));
    zassert_not_equal(epoch, 0);
    zassert_true(side <= DEFY_WIRED_RIGHT);
    battery_routes[source].live = true;
    battery_routes[source].side = side;
    battery_routes[source].epoch = epoch;
    battery_routes[source].connect_count++;
    return 0;
}

void defy_battery_central_disconnect(enum defy_link route, uint8_t source,
                                     uint32_t epoch) {
    zassert_equal(route, DEFY_LINK_WIRED);
    zassert_true(source < ARRAY_SIZE(battery_routes));
    zassert_true(battery_routes[source].live);
    zassert_equal(battery_routes[source].epoch, epoch);
    battery_routes[source].live = false;
    battery_routes[source].disconnect_count++;
}

int defy_battery_central_accept(enum defy_link route, uint8_t source, uint32_t epoch,
                               const uint8_t *data, size_t length, uint32_t extra_age_ms) {
    struct defy_battery_frame frame;

    zassert_equal(route, DEFY_LINK_WIRED);
    zassert_equal(extra_age_ms, 0);
    zassert_true(source < ARRAY_SIZE(battery_routes));
    zassert_true(battery_routes[source].live);
    zassert_equal(battery_routes[source].epoch, epoch);
    battery_routes[source].accept_attempts++;
    int err = defy_battery_frame_decode(data, length, &frame);

    if (err < 0) {
        return err;
    }
    if (frame.side != battery_routes[source].side) {
        return -EINVAL;
    }
    battery_routes[source].frame = frame;
    battery_routes[source].accepted++;
    return 0;
}

/* The version reports the cable passed on, and the sides whose cable went down. */
static struct {
    size_t received;
    enum defy_link link;
    uint8_t slot;
    struct defy_half_version version;
    size_t down[2];
    /* The number of key snapshots when each side last went down. */
    size_t down_after[2];
} versions;

void defy_half_version_received(enum defy_link link, uint8_t slot,
                                const struct defy_half_version *version) {
    versions.received++;
    versions.link = link;
    versions.slot = slot;
    versions.version = *version;
}

void defy_half_version_wired_down(uint8_t side) {
    zassert_true(side <= DEFY_WIRED_RIGHT);
    versions.down[side]++;
    versions.down_after[side] = snapshot_count;
}

int defy_hybrid_central_wired_update(uint8_t side, uint32_t session, bool connected,
                                     const uint8_t bitmap[DEFY_WIRED_BITMAP_SIZE]) {
    zassert_true(side <= DEFY_WIRED_RIGHT);
    zassert_not_equal(session, 0);
    zassert_equal(bitmap != NULL, connected);
    if (global_maintenance) {
        return 0;
    }
    attempts++;
    if (failure_at == snapshot_count && failures_left > 0) {
        failures_left--;
        return -ENOMSG;
    }
    zassert_true(snapshot_count < ARRAY_SIZE(snapshots));
    struct queued_snapshot *snapshot = &snapshots[snapshot_count++];
    snapshot->side = side;
    snapshot->session = session;
    snapshot->connected = connected;
    if (connected) {
        zassert_true(defy_wired_bitmap_valid(side, bitmap));
        memcpy(snapshot->bitmap, bitmap, sizeof(snapshot->bitmap));
    }
    return 0;
}

static void reset(void *fixture) {
    ARG_UNUSED(fixture);
    ARG_UNUSED(wired_init);
    global_maintenance = false;
    atomic_clear(&maintenance);
    for (size_t i = 0; i < ARRAY_SIZE(ports); i++) {
        const struct device *device = ports[i].device;
        memset(&ports[i], 0, sizeof(ports[i]));
        ports[i].device = device;
        memset(device->data, 0, sizeof(struct fake_spi));
    }
    memset(snapshots, 0, sizeof(snapshots));
    snapshot_count = 0;
    trace_clock = 0;
    trace_count = 0;
    attempts = 0;
    failure_at = -1;
    failures_left = 0;
    memset(battery_routes, 0, sizeof(battery_routes));
    memset(&versions, 0, sizeof(versions));
    memset(side_routes, 0, sizeof(side_routes));
    lighting_ready = false;
    desired_lighting = (struct defy_lighting_state){.boot = 0x12345678, .revision = 1,
                                                   .enabled = true};
    group_calls = 0;
    group_error = 0;
    status_callback = NULL;
    atomic_set(&connected_sides, 0);
    zassert_ok(configure_ports());
    zassert_ok(set_enabled(true));
    service_ports(0);
}

static struct defy_wired_packet data_packet(uint8_t side, uint32_t session, uint32_t sequence,
                                           int position) {
    struct defy_wired_packet packet = {
        .kind = DEFY_WIRED_DATA,
        .side = side,
        .session = session,
        .sequence = sequence,
        .flags = sequence == 0 ? DEFY_WIRED_FLAG_SYNC : 0,
    };
    if (position >= 0) {
        packet.bitmap[position / 8] |= BIT(position % 8);
    }
    return packet;
}

static void transfer(unsigned int index, const struct defy_wired_packet *packet, int result,
                     bool corrupt) {
    struct wired_port *port = &ports[index];
    struct fake_spi *state = port->device->data;
    uint8_t frame[DEFY_WIRED_FRAME_SIZE];
    defy_wired_encode(packet, frame);
    if (corrupt) {
        frame[20] ^= 0x80;
    }
    zassert_true(state->active);
    zassert_mem_equal(state->armed_tx, state->tx->buffers[0].buf, DEFY_WIRED_FRAME_SIZE);
    if (result > 0) {
        memcpy(state->rx->buffers[0].buf, frame, MIN(result, sizeof(frame)));
    }
    state->active = false;
    state->callback(port->device, result, state->userdata);
}

static void assert_ack(unsigned int index, uint32_t session, uint32_t sequence, uint8_t flags) {
    struct defy_wired_packet packet;
    zassert_ok(defy_wired_decode(ports[index].tx, DEFY_WIRED_FRAME_SIZE, &packet));
    zassert_equal(packet.kind, DEFY_WIRED_ACK);
    zassert_equal(packet.session, session);
    zassert_equal(packet.sequence, sequence);
    zassert_equal(packet.flags, flags);
}

static void exchange(unsigned int index, const struct defy_wired_packet *packet, int64_t now) {
    transfer(index, packet, DEFY_WIRED_FRAME_SIZE, false);
    service_ports(now);
}

static void battery_exchange(unsigned int index, uint8_t side, uint32_t session,
                              int64_t now, bool corrupt) {
    struct defy_battery_frame frame = {
        .side = side,
        .session = session,
        .sample = {.sequence = 8, .words = {0x4031, 2, 0, 0, 0x5080, 0xc800, 0xff80,
                                            0x1284}},
    };
    struct wired_port *port = &ports[index];
    struct fake_spi *state = port->device->data;

    zassert_true(state->active);
    zassert_ok(defy_battery_frame_encode(&frame, state->rx->buffers[0].buf));
    if (corrupt) {
        ((uint8_t *)state->rx->buffers[0].buf)[20] ^= 0x80;
    }
    state->active = false;
    state->callback(port->device, DEFY_WIRED_FRAME_SIZE, state->userdata);
    service_ports(now);
}

static void assert_snapshot(size_t index, uint8_t side, uint32_t session, bool connected,
                            const uint8_t *bitmap) {
    zassert_true(index < snapshot_count);
    zassert_equal(snapshots[index].side, side);
    zassert_equal(snapshots[index].session, session);
    zassert_equal(snapshots[index].connected, connected);
    if (connected) {
        zassert_mem_equal(snapshots[index].bitmap, bitmap, DEFY_WIRED_BITMAP_SIZE);
    }
}

static void reject_next(size_t count) {
    failure_at = snapshot_count;
    failures_left = count;
}

static void assert_no_ack(unsigned int index) {
    struct defy_wired_packet packet;
    zassert_false(ports[index].reply_valid);
    zassert_not_equal(defy_wired_decode(ports[index].tx, DEFY_WIRED_FRAME_SIZE, &packet), 0);
}

ZTEST(defy_wired_central, test_empty_sync_establishes_connection_before_any_key) {
    struct defy_wired_packet packet = data_packet(DEFY_WIRED_LEFT, 10, 0, -1);
    exchange(1, &packet, 1);
    zassert_equal(snapshot_count, 1);
    assert_snapshot(0, DEFY_WIRED_LEFT, 10, true, packet.bitmap);
    zassert_true(ports[1].receiver.connected);
    assert_ack(1, 10, 0, 0);
    zassert_equal(get_status().connections, ZMK_SPLIT_TRANSPORT_CONNECTIONS_STATUS_SOME_CONNECTED);
}

ZTEST(defy_wired_central, test_queue_full_withholds_initial_ack_until_snapshot_accepted) {
    struct defy_wired_packet packet = data_packet(DEFY_WIRED_RIGHT, 10, 0, 7);
    reject_next(2);
    exchange(0, &packet, 1);
    zassert_false(ports[0].receiver.connected);
    assert_no_ack(0);
    exchange(0, &packet, 2);
    zassert_equal(snapshot_count, 0);
    assert_no_ack(0);
    exchange(0, &packet, 3);
    zassert_equal(attempts, 3);
    zassert_equal(snapshot_count, 1);
    assert_snapshot(0, DEFY_WIRED_RIGHT, 10, true, packet.bitmap);
    assert_ack(0, 10, 0, 0);
}

ZTEST(defy_wired_central, test_lost_ack_and_identical_heartbeats_do_not_duplicate_snapshots) {
    struct defy_wired_packet packet = data_packet(DEFY_WIRED_LEFT, 10, 0, 0);
    exchange(0, &packet, 1);
    exchange(0, &packet, 201);
    zassert_equal(snapshot_count, 1);
    assert_ack(0, 10, 0, 0);
    packet.sequence = 1;
    packet.flags = 0;
    exchange(0, &packet, 401);
    zassert_equal(snapshot_count, 1);
    assert_ack(0, 10, 1, 0);
    service_ports(650);
    zassert_true(ports[0].receiver.connected);
    zassert_equal(snapshot_count, 1);
    service_ports(651);
    zassert_equal(snapshot_count, 2);
    assert_snapshot(1, DEFY_WIRED_LEFT, 10, false, NULL);
}

ZTEST(defy_wired_central, test_press_and_release_preserve_order_across_queue_backpressure) {
    struct defy_wired_packet packet = data_packet(DEFY_WIRED_LEFT, 10, 0, -1);
    exchange(0, &packet, 1);
    packet = data_packet(DEFY_WIRED_LEFT, 10, 1, 14);
    reject_next(1);
    exchange(0, &packet, 2);
    assert_no_ack(0);
    zassert_equal(ports[0].receiver.sequence, 0);
    exchange(0, &packet, 3);
    assert_snapshot(1, DEFY_WIRED_LEFT, 10, true, packet.bitmap);
    assert_ack(0, 10, 1, 0);
    packet = data_packet(DEFY_WIRED_LEFT, 10, 2, -1);
    reject_next(1);
    exchange(0, &packet, 4);
    assert_no_ack(0);
    zassert_equal(ports[0].receiver.sequence, 1);
    exchange(0, &packet, 5);
    assert_snapshot(2, DEFY_WIRED_LEFT, 10, true, packet.bitmap);
    assert_ack(0, 10, 2, 0);
    zassert_equal(snapshot_count, 3);
}

static void assert_poll(size_t index, uint8_t flags, uint16_t sequence, uint32_t completed) {
    zassert_true(index < trace_count);
    zassert_equal(traces[index].kind, DEFY_KEY_TRACE_WIRED_POLL);
    zassert_equal(traces[index].flags, flags | DEFY_KEY_TRACE_ACTIVE);
    zassert_equal(traces[index].value, sequence);
    zassert_equal(traces[index].extra, completed);
    zassert_equal(traces[index].preceding_snapshots, index);
}

ZTEST(defy_wired_central, test_key_trace_records_changed_frames_with_completion_time) {
    struct defy_wired_packet right = data_packet(DEFY_WIRED_RIGHT, 10, 0, 7);
    trace_clock = 1000;
    transfer(1, &right, DEFY_WIRED_FRAME_SIZE, false);
    trace_clock = 1500;
    service_ports(1);
    zassert_equal(snapshot_count, 1);
    zassert_equal(trace_count, 1);
    assert_poll(0, DEFY_WIRED_RIGHT | DEFY_KEY_TRACE_SOURCE, 0, 1000);
    trace_clock = 2000;
    exchange(1, &right, 2);
    zassert_equal(trace_count, 1);
    right = data_packet(DEFY_WIRED_RIGHT, 10, 1, -1);
    exchange(1, &right, 3);
    struct defy_wired_packet left = data_packet(DEFY_WIRED_LEFT, 11, 0, 17);
    trace_clock = 2500;
    exchange(0, &left, 4);
    zassert_equal(snapshot_count, 3);
    zassert_equal(trace_count, 3);
    assert_poll(1, DEFY_WIRED_RIGHT | DEFY_KEY_TRACE_SOURCE, 1, 2000);
    assert_poll(2, DEFY_WIRED_LEFT, 0, 2500);
}

ZTEST(defy_wired_central, test_timeout_disconnect_retries_without_mutating_armed_dma) {
    struct defy_wired_packet packet = data_packet(DEFY_WIRED_RIGHT, 10, 0, 21);
    exchange(0, &packet, 1);
    uint8_t armed_tx[DEFY_WIRED_FRAME_SIZE];
    memcpy(armed_tx, ports[0].tx, sizeof(armed_tx));
    reject_next(2);
    service_ports(251);
    zassert_true(ports[0].receiver.connected);
    zassert_false(ports[0].reply_valid);
    service_ports(261);
    zassert_equal(snapshot_count, 1);
    service_ports(271);
    zassert_equal(snapshot_count, 2);
    assert_snapshot(1, DEFY_WIRED_RIGHT, 10, false, NULL);
    zassert_false(ports[0].receiver.connected);
    zassert_mem_equal(armed_tx, ports[0].tx, sizeof(armed_tx));
    service_ports(281);
    zassert_equal(snapshot_count, 2);
}

ZTEST(defy_wired_central, test_new_session_forwards_unchanged_snapshot_without_disconnect) {
    struct defy_wired_packet packet = data_packet(DEFY_WIRED_LEFT, 10, 0, 0);
    exchange(0, &packet, 1);
    packet.session = 11;
    reject_next(2);
    exchange(0, &packet, 2);
    zassert_equal(snapshot_count, 1);
    zassert_equal(ports[0].receiver.session, 10);
    assert_no_ack(0);
    exchange(0, &packet, 3);
    zassert_equal(snapshot_count, 1);
    assert_no_ack(0);
    exchange(0, &packet, 4);
    zassert_equal(snapshot_count, 2);
    assert_snapshot(1, DEFY_WIRED_LEFT, 11, true, packet.bitmap);
    zassert_equal(ports[0].receiver.session, 11);
    assert_ack(0, 11, 0, 0);
    exchange(0, &packet, 5);
    zassert_equal(snapshot_count, 2);
    assert_ack(0, 11, 0, 0);
}

ZTEST(defy_wired_central, test_timeout_after_session_change_disconnects_current_epoch) {
    struct defy_wired_packet packet = data_packet(DEFY_WIRED_LEFT, 10, 0, 0);
    exchange(0, &packet, 1);
    packet.session = 11;
    exchange(0, &packet, 2);
    service_ports(252);
    zassert_equal(snapshot_count, 3);
    assert_snapshot(2, DEFY_WIRED_LEFT, 11, false, NULL);
    zassert_false(ports[0].receiver.connected);
    exchange(0, &packet, 253);
    zassert_equal(snapshot_count, 4);
    assert_snapshot(3, DEFY_WIRED_LEFT, 11, true, packet.bitmap);
    assert_ack(0, 11, 0, 0);
}

ZTEST(defy_wired_central, test_physical_side_change_waits_for_old_side_disconnect) {
    struct defy_wired_packet packet = data_packet(DEFY_WIRED_LEFT, 10, 0, 0);
    exchange(0, &packet, 1);
    packet = data_packet(DEFY_WIRED_RIGHT, 11, 0, 7);
    reject_next(1);
    exchange(0, &packet, 2);
    zassert_equal(snapshot_count, 1);
    zassert_equal(ports[0].receiver.side, DEFY_WIRED_LEFT);
    assert_no_ack(0);
    failure_at = 2;
    failures_left = 1;
    exchange(0, &packet, 3);
    zassert_equal(snapshot_count, 2);
    assert_snapshot(1, DEFY_WIRED_LEFT, 10, false, NULL);
    zassert_false(ports[0].receiver.connected);
    assert_no_ack(0);
    exchange(0, &packet, 4);
    zassert_equal(snapshot_count, 3);
    assert_snapshot(2, DEFY_WIRED_RIGHT, 11, true, packet.bitmap);
    assert_ack(0, 11, 0, 0);
}

ZTEST(defy_wired_central, test_socket_move_retains_physical_side_and_orders_disconnect) {
    struct defy_wired_packet old = data_packet(DEFY_WIRED_LEFT, 10, 0, 0);
    struct defy_wired_packet next = data_packet(DEFY_WIRED_LEFT, 11, 0, 1);
    exchange(0, &old, 1);
    exchange(1, &next, 2);
    zassert_equal(snapshot_count, 1);
    assert_no_ack(1);
    reject_next(2);
    exchange(1, &next, 251);
    zassert_equal(snapshot_count, 1);
    assert_no_ack(1);
    exchange(1, &next, 261);
    zassert_equal(snapshot_count, 3);
    assert_snapshot(1, DEFY_WIRED_LEFT, 10, false, NULL);
    assert_snapshot(2, DEFY_WIRED_LEFT, 11, true, next.bitmap);
    zassert_false(ports[0].receiver.connected);
    zassert_true(ports[1].receiver.connected);
    assert_ack(1, 11, 0, 0);
}

ZTEST(defy_wired_central, test_swapped_ports_release_old_owners_before_publishing_new_sides) {
    struct defy_wired_packet left = data_packet(DEFY_WIRED_LEFT, 10, 0, 0);
    struct defy_wired_packet right = data_packet(DEFY_WIRED_RIGHT, 20, 0, 7);
    exchange(0, &left, 1);
    exchange(1, &right, 2);
    left.session = 11;
    right.session = 21;
    transfer(0, &right, DEFY_WIRED_FRAME_SIZE, false);
    transfer(1, &left, DEFY_WIRED_FRAME_SIZE, false);
    service_ports(252);
    zassert_equal(snapshot_count, 6);
    assert_snapshot(2, DEFY_WIRED_RIGHT, 20, false, NULL);
    assert_snapshot(3, DEFY_WIRED_LEFT, 10, false, NULL);
    assert_snapshot(4, DEFY_WIRED_RIGHT, 21, true, right.bitmap);
    assert_snapshot(5, DEFY_WIRED_LEFT, 11, true, left.bitmap);
    zassert_equal(ports[0].receiver.side, DEFY_WIRED_RIGHT);
    zassert_equal(ports[1].receiver.side, DEFY_WIRED_LEFT);
    assert_ack(0, 21, 0, 0);
    assert_ack(1, 11, 0, 0);
}

ZTEST(defy_wired_central, test_invalid_transfers_cannot_reach_mux_or_extend_liveness) {
    struct defy_wired_packet packet = data_packet(DEFY_WIRED_LEFT, 10, 0, 0);
    exchange(0, &packet, 1);
    transfer(0, &packet, DEFY_WIRED_FRAME_SIZE, true);
    service_ports(101);
    transfer(0, &packet, 8, false);
    service_ports(151);
    transfer(0, &packet, -EIO, false);
    service_ports(201);
    zassert_equal(snapshot_count, 1);
    service_ports(251);
    zassert_equal(snapshot_count, 2);
    assert_snapshot(1, DEFY_WIRED_LEFT, 10, false, NULL);
}

ZTEST(defy_wired_central, test_battery_requires_key_route_and_uses_physical_side_epoch) {
    battery_exchange(1, DEFY_WIRED_LEFT, 777, 1, false);
    zassert_equal(battery_routes[1].accept_attempts, 0);
    struct defy_wired_packet packet = data_packet(DEFY_WIRED_LEFT, 10, 0, 14);

    exchange(1, &packet, 2);
    zassert_equal(battery_routes[1].connect_count, 1);
    zassert_equal(battery_routes[1].side, DEFY_WIRED_LEFT);
    zassert_equal(battery_routes[1].epoch, 10);
    battery_exchange(1, DEFY_WIRED_LEFT, 777, 3, false);
    zassert_equal(battery_routes[1].accepted, 1);
    zassert_equal(battery_routes[1].frame.session, 777);
    zassert_equal(battery_routes[1].frame.sample.sequence, 8);
    zassert_equal(ports[1].receiver.session, 10);
    zassert_equal(ports[1].receiver.sequence, 0);
    zassert_equal(ports[1].last_seen, 2);
    zassert_equal(snapshot_count, 1);
    assert_ack(1, 10, 0, 0);
    exchange(1, &packet, 4);
    zassert_equal(battery_routes[1].connect_count, 1);
    battery_exchange(1, DEFY_WIRED_RIGHT, 777, 5, false);
    zassert_equal(battery_routes[1].accept_attempts, 2);
    zassert_equal(battery_routes[1].accepted, 1);
    assert_ack(1, 10, 0, 0);
}

ZTEST(defy_wired_central, test_bad_battery_keeps_key_ack_and_cannot_extend_link_liveness) {
    struct defy_wired_packet packet = data_packet(DEFY_WIRED_RIGHT, 20, 0, 21);

    exchange(0, &packet, 1);
    battery_exchange(0, DEFY_WIRED_RIGHT, 888, 101, true);
    zassert_equal(battery_routes[0].accept_attempts, 1);
    zassert_equal(battery_routes[0].accepted, 0);
    zassert_equal(ports[0].last_seen, 1);
    zassert_equal(snapshot_count, 1);
    assert_ack(0, 20, 0, 0);
    battery_exchange(0, DEFY_WIRED_RIGHT, 888, 201, false);
    zassert_equal(battery_routes[0].accepted, 1);
    zassert_equal(ports[0].last_seen, 1);
    service_ports(251);
    zassert_false(battery_routes[0].live);
    zassert_equal(battery_routes[0].disconnect_count, 1);
    zassert_false(ports[0].receiver.connected);
    zassert_equal(snapshot_count, 2);
    battery_exchange(0, DEFY_WIRED_RIGHT, 888, 252, false);
    zassert_equal(battery_routes[0].accept_attempts, 2);
}

ZTEST(defy_wired_central, test_battery_does_not_acknowledge_backpressured_key_transition) {
    struct defy_wired_packet packet = data_packet(DEFY_WIRED_LEFT, 10, 0, -1);

    exchange(0, &packet, 1);
    packet = data_packet(DEFY_WIRED_LEFT, 10, 1, 14);
    reject_next(1);
    exchange(0, &packet, 2);
    assert_no_ack(0);
    battery_exchange(0, DEFY_WIRED_LEFT, 777, 3, false);
    assert_no_ack(0);
    zassert_equal(ports[0].receiver.sequence, 0);
    zassert_equal(snapshot_count, 1);
    exchange(0, &packet, 4);
    assert_ack(0, 10, 1, 0);
    zassert_equal(snapshot_count, 2);
}

ZTEST(defy_wired_central, test_battery_route_changes_epoch_without_disconnect_with_key_session) {
    struct defy_wired_packet packet = data_packet(DEFY_WIRED_LEFT, 10, 0, -1);

    exchange(0, &packet, 1);
    battery_exchange(0, DEFY_WIRED_LEFT, 777, 2, false);
    packet.session = 11;
    exchange(0, &packet, 3);
    zassert_equal(battery_routes[0].disconnect_count, 0);
    zassert_equal(battery_routes[0].connect_count, 2);
    zassert_equal(battery_routes[0].epoch, 11);
    battery_exchange(0, DEFY_WIRED_LEFT, 777, 4, false);
    zassert_equal(battery_routes[0].accepted, 2);
    zassert_equal(battery_routes[0].frame.session, 777);
    assert_ack(0, 11, 0, 0);
}

static void assert_battery_show(unsigned int index, uint8_t side, uint32_t session) {
    struct defy_wired_control control;
    struct defy_wired_battery_show_command command;
    zassert_ok(defy_wired_control_check(ports[index].tx, DEFY_WIRED_FRAME_SIZE, &control));
    zassert_ok(defy_wired_battery_show_decode(ports[index].tx, &control, &command));
    zassert_equal(command.side, side);
    zassert_equal(command.session, session);
}

ZTEST(defy_wired_central, test_battery_show_requires_live_cable_and_valid_side) {
    zassert_equal(defy_wired_battery_show(0), -ENOTCONN);
    zassert_equal(defy_wired_battery_show(2), -EINVAL);
    struct defy_wired_packet packet = data_packet(DEFY_WIRED_LEFT, 10, 0, -1);
    exchange(0, &packet, k_uptime_get());
    zassert_equal(defy_wired_battery_show(1), -ENOTCONN);
    zassert_ok(defy_wired_battery_show(0));
    zassert_ok(set_enabled(false));
    zassert_equal(defy_wired_battery_show(0), -ENOTCONN);
    zassert_false(side_routes[0].battery_show_pending);
}

ZTEST(defy_wired_central, test_battery_show_targets_physical_side_on_swapped_socket_once) {
    struct defy_wired_packet packet = data_packet(DEFY_WIRED_RIGHT, 20, 0, -1);
    int64_t now = k_uptime_get();
    exchange(0, &packet, now);
    uint8_t armed_ack[DEFY_WIRED_FRAME_SIZE];
    memcpy(armed_ack, ports[0].tx, sizeof(armed_ack));
    zassert_ok(defy_wired_battery_show(DEFY_WIRED_RIGHT));
    zassert_mem_equal(ports[0].tx, armed_ack, sizeof(armed_ack));
    service_ports(now + 1);
    zassert_mem_equal(ports[0].tx, armed_ack, sizeof(armed_ack));
    exchange(0, &packet, now + 2);
    assert_battery_show(0, DEFY_WIRED_RIGHT, 20);
    zassert_equal(snapshot_count, 1);
    exchange(0, &packet, now + 3);
    assert_ack(0, 20, 0, 0);
    exchange(0, &packet, now + 4);
    assert_ack(0, 20, 0, 0);
}

ZTEST(defy_wired_central, test_battery_show_queue_expiry_drops_request) {
    struct defy_wired_packet packet = data_packet(DEFY_WIRED_LEFT, 10, 0, -1);
    int64_t now = k_uptime_get();
    exchange(0, &packet, now);
    zassert_ok(defy_wired_battery_show(0));
    exchange(0, &packet, now + DEFY_CONTROL_TIMEOUT_MS);
    assert_ack(0, 10, 0, 0);
    zassert_false(side_routes[0].battery_show_pending);
}

ZTEST(defy_wired_central, test_battery_show_arm_failure_does_not_retry_or_lose_ack) {
    struct defy_wired_packet packet = data_packet(DEFY_WIRED_LEFT, 10, 0, -1);
    int64_t now = k_uptime_get();
    exchange(0, &packet, now);
    zassert_ok(defy_wired_battery_show(0));
    fake_spi0.arm_error = -EBUSY;
    exchange(0, &packet, now + 1);
    zassert_false(ports[0].armed);
    zassert_false(side_routes[0].battery_show_pending);
    service_ports(now + 2);
    assert_ack(0, 10, 0, 0);
}

static void assert_lighting(unsigned int index, uint8_t side, uint32_t session) {
    struct defy_wired_control control;
    struct defy_wired_lighting_command command;
    zassert_ok(defy_wired_control_check(ports[index].tx, DEFY_WIRED_FRAME_SIZE, &control));
    zassert_ok(defy_wired_lighting_decode(ports[index].tx, &control, &command));
    zassert_equal(command.opcode, DEFY_CONTROL_LIGHTING_SET);
    zassert_equal(command.side, side);
    zassert_equal(command.session, session);
    zassert_equal(command.state.boot, desired_lighting.boot);
    zassert_equal(command.state.revision, desired_lighting.revision);
    zassert_equal(command.state.enabled, desired_lighting.enabled);
    zassert_equal(command.state.groups, desired_lighting.groups);
    zassert_equal(command.state.level, desired_lighting.level);
}

/* The port's next transfer carries the group with index for side, in session. */
static void assert_group(unsigned int index, uint8_t side, uint32_t session, uint8_t group) {
    struct defy_wired_control control;
    struct defy_wired_lighting_group_command command;
    zassert_ok(defy_wired_control_check(ports[index].tx, DEFY_WIRED_FRAME_SIZE, &control));
    zassert_equal(control.opcode, DEFY_CONTROL_LIGHTING_GROUP);
    zassert_ok(defy_wired_lighting_group_decode(ports[index].tx, &control, &command));
    zassert_equal(command.side, side);
    zassert_equal(command.session, session);
    zassert_equal(command.group.boot, desired_lighting.boot);
    zassert_equal(command.group.index, group);
    zassert_equal(command.group.leds, BIT64(group) | BIT64(34 - side));
}

static void lighting_exchange(unsigned int index,
                               const struct defy_wired_lighting_command *command,
                               int64_t now, bool corrupt) {
    struct wired_port *port = &ports[index];
    struct fake_spi *state = port->device->data;
    zassert_true(state->active);
    zassert_mem_equal(state->armed_tx, state->tx->buffers[0].buf, DEFY_WIRED_FRAME_SIZE);
    zassert_ok(defy_wired_lighting_encode(command, state->rx->buffers[0].buf));
    if (corrupt) {
        ((uint8_t *)state->rx->buffers[0].buf)[20] ^= 1;
    }
    state->active = false;
    state->callback(port->device, DEFY_WIRED_FRAME_SIZE, state->userdata);
    service_ports(now);
}

static struct defy_wired_lighting_command lighting_reply(uint8_t side, uint32_t session) {
    return (struct defy_wired_lighting_command){
        .opcode = DEFY_CONTROL_LIGHTING_ACK, .side = side, .session = session,
        .state = desired_lighting,
    };
}

ZTEST(defy_wired_central, test_lighting_waits_for_sent_key_ack_and_targets_physical_side) {
    lighting_ready = true;
    struct defy_wired_packet packet = data_packet(DEFY_WIRED_RIGHT, 20, 0, -1);
    exchange(0, &packet, 1);
    assert_ack(0, 20, 0, 0);
    zassert_equal(ports[0].lighting.attempts, 0);
    exchange(0, &packet, 2);
    assert_lighting(0, DEFY_WIRED_RIGHT, 20);
    packet = data_packet(DEFY_WIRED_RIGHT, 20, 1, 21);
    desired_lighting.revision++;
    desired_lighting.enabled = false;
    exchange(0, &packet, 3);
    assert_ack(0, 20, 1, 0);
    zassert_equal(ports[0].lighting.attempts, 0);
    exchange(0, &packet, 4);
    assert_lighting(0, DEFY_WIRED_RIGHT, 20);
    zassert_equal(snapshot_count, 2);
    zassert_equal(group_calls, 0);
}

ZTEST(defy_wired_central, test_lighting_confirmation_requires_complete_matching_tuple) {
    lighting_ready = true;
    struct defy_wired_packet packet = data_packet(DEFY_WIRED_LEFT, 10, 0, -1);
    exchange(0, &packet, 1);
    exchange(0, &packet, 2);
    struct defy_wired_lighting_command valid = lighting_reply(0, 10);
    for (size_t field = 0; field < 9; field++) {
        struct defy_wired_lighting_command bad = valid;
        switch (field) {
        case 0: bad.opcode = DEFY_CONTROL_LIGHTING_SET; break;
        case 1: bad.side = 1; break;
        case 2: bad.session++; break;
        case 3: bad.state.boot++; break;
        case 4: bad.state.revision++; break;
        case 5: bad.state.enabled = !bad.state.enabled; break;
        case 6: bad.state.groups = BIT(1); bad.state.level = 1; break;
        case 7: bad.state.level = 5; break;
        default: break;
        }
        lighting_exchange(0, &bad, 3 + field, field == 8);
        zassert_false(ports[0].lighting.confirmed);
        zassert_equal(ports[0].lighting_groups, 0);
    }
    lighting_exchange(0, &valid, 12, false);
    zassert_true(ports[0].lighting.confirmed);
    zassert_equal(ports[0].last_seen, 2);
    for (int64_t now = 100; now <= 5000; now += 100) {
        exchange(0, &packet, now);
        assert_ack(0, 10, 0, 0);
    }
    zassert_equal(ports[0].lighting.attempts, 1);
}

ZTEST(defy_wired_central, test_lighting_retries_back_off_and_stop_after_six_attempts) {
    lighting_ready = true;
    struct defy_wired_packet packet = data_packet(DEFY_WIRED_LEFT, 10, 0, -1);
    exchange(0, &packet, 1);
    exchange(0, &packet, 2);
    static const int64_t due[] = {252, 752, 1752, 3752, 7752};
    for (size_t i = 0; i < ARRAY_SIZE(due); i++) {
        exchange(0, &packet, due[i] - 1);
        assert_ack(0, 10, 0, 0);
        zassert_equal(ports[0].lighting.attempts, i + 1);
        exchange(0, &packet, due[i]);
        assert_lighting(0, 0, 10);
        zassert_equal(ports[0].lighting.attempts, i + 2);
    }
    exchange(0, &packet, 8000);
    exchange(0, &packet, 30000);
    assert_ack(0, 10, 0, 0);
    zassert_equal(ports[0].lighting.attempts, 6);
    desired_lighting.enabled = false;
    desired_lighting.revision++;
    defy_wired_lighting_changed();
    exchange(0, &packet, 30001);
    assert_lighting(0, 0, 10);
    zassert_equal(ports[0].lighting.attempts, 1);
}

ZTEST(defy_wired_central, test_lighting_reconnect_replays_and_rejects_old_session_ack) {
    lighting_ready = true;
    struct defy_wired_packet packet = data_packet(DEFY_WIRED_LEFT, 10, 0, -1);
    exchange(1, &packet, 1);
    exchange(1, &packet, 2);
    struct defy_wired_lighting_command old = lighting_reply(0, 10);
    lighting_exchange(1, &old, 3, false);
    zassert_true(ports[1].lighting.confirmed);
    service_ports(300);
    zassert_false(ports[1].receiver.connected);
    packet.session = 11;
    exchange(1, &packet, 301);
    assert_ack(1, 11, 0, 0);
    exchange(1, &packet, 302);
    assert_lighting(1, 0, 11);
    lighting_exchange(1, &old, 303, false);
    zassert_false(ports[1].lighting.confirmed);
    struct defy_wired_lighting_command current = lighting_reply(0, 11);
    lighting_exchange(1, &current, 304, false);
    zassert_true(ports[1].lighting.confirmed);
}

ZTEST(defy_wired_central, test_lighting_ack_cannot_extend_key_route_liveness) {
    lighting_ready = true;
    struct defy_wired_packet packet = data_packet(DEFY_WIRED_LEFT, 10, 0, -1);
    exchange(0, &packet, 1);
    exchange(0, &packet, 2);
    struct defy_wired_lighting_command ack = lighting_reply(0, 10);
    lighting_exchange(0, &ack, 200, false);
    zassert_true(ports[0].receiver.connected);
    lighting_exchange(0, &ack, 252, false);
    zassert_false(ports[0].receiver.connected);
}

ZTEST(defy_wired_central, test_lighting_groups_go_one_per_transfer_before_the_state) {
    lighting_ready = true;
    desired_lighting.groups = BIT(2) | BIT(5);
    desired_lighting.level = 99;
    struct defy_wired_packet packet = data_packet(DEFY_WIRED_RIGHT, 20, 0, -1);
    exchange(0, &packet, 1);
    assert_ack(0, 20, 0, 0);
    exchange(0, &packet, 2);
    assert_group(0, DEFY_WIRED_RIGHT, 20, 2);
    exchange(0, &packet, 3);
    assert_ack(0, 20, 0, 0);
    exchange(0, &packet, 4);
    assert_group(0, DEFY_WIRED_RIGHT, 20, 5);
    zassert_equal(ports[0].lighting.attempts, 0);
    exchange(0, &packet, 5);
    assert_ack(0, 20, 0, 0);
    exchange(0, &packet, 6);
    assert_lighting(0, DEFY_WIRED_RIGHT, 20);
    zassert_equal(ports[0].lighting.attempts, 1);

    /* Acknowledged: a state that names one more group sends that one alone. */
    struct defy_wired_lighting_command ack = lighting_reply(DEFY_WIRED_RIGHT, 20);
    lighting_exchange(0, &ack, 7, false);
    zassert_true(ports[0].lighting.confirmed);
    zassert_equal(ports[0].lighting_groups, BIT(2) | BIT(5));
    assert_ack(0, 20, 0, 0);
    desired_lighting.revision++;
    desired_lighting.groups = BIT(5) | BIT(7);
    exchange(0, &packet, 8);
    assert_group(0, DEFY_WIRED_RIGHT, 20, 7);
    exchange(0, &packet, 9);
    assert_ack(0, 20, 0, 0);
    exchange(0, &packet, 10);
    assert_lighting(0, DEFY_WIRED_RIGHT, 20);
    ack = lighting_reply(DEFY_WIRED_RIGHT, 20);
    lighting_exchange(0, &ack, 11, false);
    zassert_equal(ports[0].lighting_groups, BIT(2) | BIT(5) | BIT(7));
    desired_lighting.revision++;
    desired_lighting.groups = BIT(2) | BIT(7);
    exchange(0, &packet, 12);
    assert_lighting(0, DEFY_WIRED_RIGHT, 20);
    zassert_equal(group_calls, 3);
}

ZTEST(defy_wired_central, test_lighting_unacknowledged_state_sends_its_groups_again) {
    lighting_ready = true;
    desired_lighting.groups = BIT(3);
    desired_lighting.level = 7;
    group_error = -EAGAIN;
    struct defy_wired_packet packet = data_packet(DEFY_WIRED_LEFT, 10, 0, -1);
    exchange(0, &packet, 1);
    exchange(0, &packet, 2);
    /* A group that cannot go costs the attempt, and the state waits. */
    assert_ack(0, 10, 0, 0);
    zassert_equal(ports[0].lighting.attempts, 1);
    group_error = 0;
    exchange(0, &packet, 251);
    assert_ack(0, 10, 0, 0);
    exchange(0, &packet, 252);
    assert_group(0, DEFY_WIRED_LEFT, 10, 3);
    exchange(0, &packet, 253);
    assert_ack(0, 10, 0, 0);
    exchange(0, &packet, 254);
    assert_lighting(0, DEFY_WIRED_LEFT, 10);
    zassert_equal(ports[0].lighting.attempts, 2);
    exchange(0, &packet, 753);
    assert_ack(0, 10, 0, 0);
    exchange(0, &packet, 754);
    assert_group(0, DEFY_WIRED_LEFT, 10, 3);
    exchange(0, &packet, 755);
    exchange(0, &packet, 756);
    assert_lighting(0, DEFY_WIRED_LEFT, 10);
    zassert_equal(ports[0].lighting.attempts, 3);
}

ZTEST(defy_wired_central, test_lighting_link_that_comes_back_gets_its_groups_again) {
    lighting_ready = true;
    desired_lighting.groups = BIT(0);
    desired_lighting.level = 1;
    struct defy_wired_packet packet = data_packet(DEFY_WIRED_LEFT, 10, 0, -1);
    exchange(1, &packet, 1);
    exchange(1, &packet, 2);
    assert_group(1, DEFY_WIRED_LEFT, 10, 0);
    exchange(1, &packet, 3);
    exchange(1, &packet, 4);
    assert_lighting(1, DEFY_WIRED_LEFT, 10);
    struct defy_wired_lighting_command ack = lighting_reply(DEFY_WIRED_LEFT, 10);
    lighting_exchange(1, &ack, 5, false);
    zassert_equal(ports[1].lighting_groups, BIT(0));

    /* The cable times out and the half comes back in the same session, whose
     * acknowledgement the last transfer carried whole. */
    service_ports(300);
    zassert_false(ports[1].receiver.connected);
    exchange(1, &packet, 301);
    zassert_equal(ports[1].lighting_groups, 0);
    assert_group(1, DEFY_WIRED_LEFT, 10, 0);
    exchange(1, &packet, 302);
    assert_ack(1, 10, 0, 0);
    exchange(1, &packet, 303);
    assert_lighting(1, DEFY_WIRED_LEFT, 10);
    ack = lighting_reply(DEFY_WIRED_LEFT, 10);
    lighting_exchange(1, &ack, 304, false);
    zassert_equal(ports[1].lighting_groups, BIT(0));

    /* A new session, as after a resync, starts with none confirmed too. */
    packet = data_packet(DEFY_WIRED_LEFT, 11, 0, -1);
    exchange(1, &packet, 305);
    assert_ack(1, 11, 0, 0);
    zassert_equal(ports[1].lighting_groups, 0);
    exchange(1, &packet, 306);
    assert_group(1, DEFY_WIRED_LEFT, 11, 0);
}

ZTEST(defy_wired_central, test_maintenance_preserves_dma_and_stops_rearming_both_ports) {
    struct defy_wired_packet left = data_packet(DEFY_WIRED_LEFT, 10, 0, 0);
    struct defy_wired_packet right = data_packet(DEFY_WIRED_RIGHT, 20, 0, 7);
    exchange(0, &left, 1);
    exchange(1, &right, 2);
    uint8_t tx[2][DEFY_WIRED_FRAME_SIZE];
    memcpy(tx[0], ports[0].tx, sizeof(tx[0]));
    memcpy(tx[1], ports[1].tx, sizeof(tx[1]));
    global_maintenance = true;
    zassert_ok(defy_wired_central_set_maintenance(true));
    for (size_t i = 0; i < ARRAY_SIZE(ports); i++) {
        zassert_mem_equal(tx[i], ports[i].tx, sizeof(tx[i]));
        zassert_true(ports[i].armed);
        zassert_false(ports[i].receiver.connected);
        zassert_false(battery_routes[i].live);
    }
    zassert_equal(defy_wired_battery_show(0), -ENOTCONN);
    transfer(0, &left, DEFY_WIRED_FRAME_SIZE, false);
    transfer(1, &right, DEFY_WIRED_FRAME_SIZE, false);
    service_ports(3);
    zassert_false(fake_spi0.active);
    zassert_false(fake_spi1.active);
    zassert_equal(snapshot_count, 2);
    zassert_equal(atomic_get(&connected_sides), 0);
    zassert_ok(defy_wired_central_set_maintenance(false));
    service_ports(4);
    zassert_false(fake_spi0.active);
    zassert_false(fake_spi1.active);
    global_maintenance = false;
    service_ports(5);
    zassert_true(fake_spi0.active);
    zassert_true(fake_spi1.active);
    assert_no_ack(0);
    assert_no_ack(1);
    exchange(0, &left, 6);
    exchange(1, &right, 7);
    zassert_equal(snapshot_count, 4);
}

ZTEST(defy_wired_central, test_maintenance_discards_transfer_that_finishes_after_resume) {
    struct defy_wired_packet packet = data_packet(DEFY_WIRED_LEFT, 10, 0, 0);
    exchange(0, &packet, 1);
    global_maintenance = true;
    zassert_ok(defy_wired_central_set_maintenance(true));
    zassert_ok(defy_wired_central_set_maintenance(false));
    global_maintenance = false;
    packet = data_packet(DEFY_WIRED_LEFT, 10, 1, 14);
    exchange(0, &packet, 2);
    zassert_equal(snapshot_count, 1);
    zassert_false(ports[0].receiver.connected);
    zassert_false(ports[0].sent_ack_valid);
    assert_no_ack(0);
    exchange(0, &packet, 3);
    zassert_equal(snapshot_count, 1);
    assert_ack(0, 10, 1, DEFY_WIRED_FLAG_RESYNC);
    packet = data_packet(DEFY_WIRED_LEFT, 11, 0, 14);
    exchange(0, &packet, 4);
    zassert_equal(snapshot_count, 2);
    assert_snapshot(1, DEFY_WIRED_LEFT, 11, true, packet.bitmap);
}

ZTEST(defy_wired_central, test_maintenance_discards_completion_already_waiting_for_worker) {
    struct defy_wired_packet packet = data_packet(DEFY_WIRED_LEFT, 10, 0, 0);
    transfer(0, &packet, DEFY_WIRED_FRAME_SIZE, false);
    global_maintenance = true;
    zassert_ok(defy_wired_central_set_maintenance(true));
    service_ports(1);
    zassert_equal(snapshot_count, 0);
    zassert_false(ports[0].receiver.connected);
    zassert_false(fake_spi0.active);
}

ZTEST(defy_wired_central, test_maintenance_clears_commands_and_preserves_disable_request) {
    struct defy_wired_packet packet = data_packet(DEFY_WIRED_LEFT, 10, 0, -1);
    exchange(0, &packet, k_uptime_get());
    zassert_ok(defy_wired_battery_show(0));
    zassert_true(side_routes[0].battery_show_pending);
    global_maintenance = true;
    zassert_ok(defy_wired_central_set_maintenance(true));
    zassert_false(side_routes[0].battery_show_pending);
    zassert_ok(set_enabled(false));
    zassert_ok(defy_wired_central_set_maintenance(false));
    global_maintenance = false;
    exchange(0, &packet, k_uptime_get());
    exchange(0, &packet, k_uptime_get());
    zassert_false(get_status().enabled);
    zassert_false(ports[0].receiver.connected);
    zassert_equal(snapshot_count, 1);
}

static struct defy_host_feedback feedback_request(uint32_t sequence) {
    return (struct defy_host_feedback){
        .boot = 123, .sequence = sequence, .remaining_ms = 2000, .profile = 2,
        .flags = DEFY_HOST_PREFERRED_BLE | DEFY_HOST_SELECTED_BLE | DEFY_HOST_PROFILE_CONNECTED,
    };
}

static void assert_feedback(unsigned int index, uint8_t side, uint32_t session,
                            uint32_t sequence, uint16_t remaining) {
    struct defy_wired_control control;
    struct defy_wired_host_feedback command;
    zassert_ok(defy_wired_control_check(ports[index].tx, DEFY_WIRED_FRAME_SIZE, &control));
    zassert_ok(defy_wired_host_feedback_decode(ports[index].tx, &control, &command));
    zassert_equal(command.side, side);
    zassert_equal(command.session, session);
    zassert_equal(command.state.boot, 123);
    zassert_equal(command.state.sequence, sequence);
    zassert_equal(command.state.remaining_ms, remaining);
    zassert_equal(command.state.profile, 2);
    zassert_equal(command.state.flags, DEFY_HOST_PREFERRED_BLE | DEFY_HOST_SELECTED_BLE |
                                       DEFY_HOST_PROFILE_CONNECTED);
}

ZTEST(defy_wired_central, test_feedback_targets_live_sides_and_reduces_remaining_time) {
    struct defy_host_feedback request = feedback_request(1);
    defy_wired_host_feedback_show(&request);
    zassert_false(side_routes[0].feedback_pending);
    zassert_false(side_routes[1].feedback_pending);
    struct defy_wired_packet right = data_packet(1, 20, 0, -1);
    struct defy_wired_packet left = data_packet(0, 10, 0, -1);
    int64_t now = k_uptime_get();
    exchange(0, &right, now);
    exchange(1, &left, now);
    defy_wired_host_feedback_show(&request);
    int64_t expiry = side_routes[0].feedback_expires;
    uint8_t armed[DEFY_WIRED_FRAME_SIZE];
    memcpy(armed, ports[0].tx, sizeof(armed));
    service_ports(now + 1);
    zassert_mem_equal(armed, ports[0].tx, sizeof(armed));
    exchange(0, &right, expiry - 1850);
    exchange(1, &left, expiry - 1850);
    assert_feedback(0, 1, 20, 1, 1850);
    assert_feedback(1, 0, 10, 1, 1850);
    zassert_equal(snapshot_count, 2);
    exchange(0, &right, expiry - 1849);
    assert_ack(0, 20, 0, 0);
    exchange(0, &right, expiry - 1848);
    assert_ack(0, 20, 0, 0);
}

ZTEST(defy_wired_central, test_feedback_coalesces_and_never_precedes_current_key_ack) {
    struct defy_wired_packet packet = data_packet(0, 10, 0, -1);
    int64_t now = k_uptime_get();
    exchange(0, &packet, now);
    struct defy_host_feedback request = feedback_request(1);
    defy_wired_host_feedback_show(&request);
    request.sequence = 2;
    defy_wired_host_feedback_show(&request);
    int64_t expiry = side_routes[0].feedback_expires;
    packet = data_packet(0, 10, 1, 14);
    exchange(0, &packet, now + 1);
    assert_ack(0, 10, 1, 0);
    exchange(0, &packet, now + 2);
    assert_feedback(0, 0, 10, 2, expiry - (now + 2));
    request.sequence = 3;
    defy_wired_host_feedback_show(&request);
    packet = data_packet(0, 10, 2, -1);
    exchange(0, &packet, now + 3);
    assert_ack(0, 10, 2, 0);
    zassert_equal(snapshot_count, 3);
    zassert_equal(snapshots[1].bitmap[1], BIT(6));
    zassert_equal(snapshots[2].bitmap[1], 0);
}

ZTEST(defy_wired_central, test_feedback_expires_without_delayed_replay) {
    struct defy_wired_packet packet = data_packet(0, 10, 0, -1);
    int64_t now = k_uptime_get();
    exchange(0, &packet, now);
    struct defy_host_feedback request = feedback_request(1);
    defy_wired_host_feedback_show(&request);
    int64_t expiry = side_routes[0].feedback_expires;
    exchange(0, &packet, expiry);
    assert_ack(0, 10, 0, 0);
    zassert_false(side_routes[0].feedback_pending);
    exchange(0, &packet, expiry + 1);
    assert_ack(0, 10, 0, 0);
}

ZTEST(defy_wired_central, test_feedback_session_change_and_disconnect_discard_pending) {
    struct defy_wired_packet packet = data_packet(0, 10, 0, -1);
    int64_t now = k_uptime_get();
    exchange(0, &packet, now);
    struct defy_host_feedback request = feedback_request(1);
    defy_wired_host_feedback_show(&request);
    packet.session = 11;
    exchange(0, &packet, now + 1);
    assert_ack(0, 11, 0, 0);
    zassert_false(side_routes[0].feedback_pending);
    defy_wired_host_feedback_show(&request);
    service_ports(now + CONFIG_DEFY_WIRED_CENTRAL_TIMEOUT_MS + 2);
    zassert_false(side_routes[0].feedback_pending);
    zassert_false(side_routes[0].connected);
    defy_wired_host_feedback_show(&request);
    zassert_false(side_routes[0].feedback_pending);
}

ZTEST(defy_wired_central, test_feedback_disable_discards_pending) {
    struct defy_wired_packet packet = data_packet(0, 10, 0, -1);
    exchange(0, &packet, k_uptime_get());
    struct defy_host_feedback request = feedback_request(1);
    defy_wired_host_feedback_show(&request);
    zassert_true(side_routes[0].feedback_pending);
    zassert_ok(set_enabled(false));
    zassert_false(side_routes[0].feedback_pending);
    defy_wired_host_feedback_show(&request);
    zassert_false(side_routes[0].feedback_pending);
}

ZTEST(defy_wired_central, test_feedback_yields_between_lighting_and_battery_commands) {
    lighting_ready = true;
    struct defy_wired_packet packet = data_packet(0, 10, 0, -1);
    int64_t now = k_uptime_get();
    exchange(0, &packet, now);
    struct defy_host_feedback request = feedback_request(1);
    defy_wired_host_feedback_show(&request);
    exchange(0, &packet, now + 1);
    assert_lighting(0, 0, 10);
    exchange(0, &packet, now + 2);
    assert_ack(0, 10, 0, 0);
    zassert_ok(defy_wired_battery_show(0));
    exchange(0, &packet, now + 3);
    assert_battery_show(0, 0, 10);
    exchange(0, &packet, now + 4);
    assert_ack(0, 10, 0, 0);
    exchange(0, &packet, now + 5);
    assert_feedback(0, 0, 10, 1, side_routes[0].feedback_expires - (now + 5));
    exchange(0, &packet, now + 6);
    assert_ack(0, 10, 0, 0);
}

static const struct defy_half_version left_version = {
    .side = DEFY_WIRED_LEFT, .program_crc = 0x13572468, .seal_version = 4, .module = "v0.1.0",
};

/* The half on index sends a 'DV' frame with record, then the worker runs. */
static void version_exchange(unsigned int index, const uint8_t record[DEFY_HALF_VERSION_SIZE],
                             int64_t now, bool corrupt) {
    struct wired_port *port = &ports[index];
    struct fake_spi *state = port->device->data;

    zassert_true(state->active);
    defy_wired_half_version_encode(record, state->rx->buffers[0].buf);
    if (corrupt) {
        ((uint8_t *)state->rx->buffers[0].buf)[20] ^= 0x80;
    }
    state->active = false;
    state->callback(port->device, DEFY_WIRED_FRAME_SIZE, state->userdata);
    service_ports(now);
}

ZTEST(defy_wired_central, test_version_requires_key_route_and_its_side) {
    uint8_t record[DEFY_HALF_VERSION_SIZE];
    defy_half_version_encode(&left_version, record);
    version_exchange(1, record, 1, false);
    zassert_equal(versions.received, 0);
    struct defy_wired_packet packet = data_packet(DEFY_WIRED_LEFT, 10, 0, 14);

    exchange(1, &packet, 2);
    version_exchange(1, record, 3, false);
    zassert_equal(versions.received, 1);
    zassert_equal(versions.link, DEFY_LINK_WIRED);
    zassert_equal(versions.slot, 1);
    zassert_equal(versions.version.side, DEFY_WIRED_LEFT);
    zassert_equal(versions.version.program_crc, 0x13572468);
    zassert_equal(versions.version.seal_version, 4);
    zassert_str_equal(versions.version.module, "v0.1.0");
    /* The frame neither acknowledges keys nor keeps the cable alive. */
    zassert_equal(ports[1].last_seen, 2);
    zassert_equal(snapshot_count, 1);
    assert_ack(1, 10, 0, 0);

    /* The other side's report or a corrupt frame is dropped. */
    struct defy_half_version right_version = left_version;
    right_version.side = DEFY_WIRED_RIGHT;
    defy_half_version_encode(&right_version, record);
    version_exchange(1, record, 4, false);
    defy_half_version_encode(&left_version, record);
    version_exchange(1, record, 5, true);
    zassert_equal(versions.received, 1);
    assert_ack(1, 10, 0, 0);
    service_ports(252);
    zassert_false(ports[1].receiver.connected);
    defy_half_version_encode(&left_version, record);
    version_exchange(1, record, 253, false);
    zassert_equal(versions.received, 1);
}

ZTEST(defy_wired_central, test_version_dropped_when_the_cable_goes_down) {
    uint8_t record[DEFY_HALF_VERSION_SIZE];
    defy_half_version_encode(&left_version, record);
    struct defy_wired_packet left = data_packet(DEFY_WIRED_LEFT, 10, 0, -1);
    struct defy_wired_packet right = data_packet(DEFY_WIRED_RIGHT, 20, 0, -1);

    /* A timeout, after the key route's disconnection. */
    exchange(0, &left, 1);
    version_exchange(0, record, 2, false);
    zassert_equal(versions.received, 1);
    service_ports(250);
    zassert_equal(versions.down[DEFY_WIRED_LEFT], 0);
    service_ports(251);
    zassert_equal(versions.down[DEFY_WIRED_LEFT], 1);
    zassert_equal(versions.down_after[DEFY_WIRED_LEFT], 2);
    assert_snapshot(1, DEFY_WIRED_LEFT, 10, false, NULL);
    service_ports(260);
    zassert_equal(versions.down[DEFY_WIRED_LEFT], 1);

    /* The other half on the same socket. */
    left.session = 11;
    exchange(0, &left, 300);
    right.session = 21;
    exchange(0, &right, 301);
    zassert_equal(versions.down[DEFY_WIRED_LEFT], 2);
    zassert_equal(versions.down[DEFY_WIRED_RIGHT], 0);

    /* A new key session on the same socket keeps the record. */
    right.session = 22;
    exchange(0, &right, 302);
    zassert_true(ports[0].receiver.connected);
    zassert_equal(versions.down[DEFY_WIRED_RIGHT], 0);

    /* Turning the transport off. */
    zassert_ok(set_enabled(false));
    service_ports(303);
    zassert_equal(versions.down[DEFY_WIRED_RIGHT], 1);
    zassert_ok(set_enabled(true));
    service_ports(304);

    /* Maintenance, for both sides at once. */
    left.session = 12;
    right.session = 23;
    exchange(0, &left, 305);
    exchange(1, &right, 306);
    global_maintenance = true;
    zassert_ok(defy_wired_central_set_maintenance(true));
    zassert_equal(versions.down[DEFY_WIRED_LEFT], 3);
    zassert_equal(versions.down[DEFY_WIRED_RIGHT], 2);
    version_exchange(0, record, 307, false);
    zassert_equal(versions.received, 1);
    zassert_ok(defy_wired_central_set_maintenance(false));
    global_maintenance = false;
}

ZTEST(defy_wired_central, test_version_cable_stays_up_while_a_disconnect_is_refused) {
    struct defy_wired_packet left = data_packet(DEFY_WIRED_LEFT, 10, 0, 0);
    struct defy_wired_packet right = data_packet(DEFY_WIRED_RIGHT, 11, 0, 7);
    exchange(0, &left, 1);
    /* The router refuses the left side's disconnection, so the socket keeps it. */
    reject_next(1);
    exchange(0, &right, 2);
    zassert_true(ports[0].receiver.connected);
    zassert_equal(ports[0].receiver.side, DEFY_WIRED_LEFT);
    zassert_equal(versions.down[DEFY_WIRED_LEFT], 0);
    exchange(0, &right, 3);
    zassert_equal(versions.down[DEFY_WIRED_LEFT], 1);
    zassert_equal(versions.down_after[DEFY_WIRED_LEFT], 2);
}

ZTEST(defy_wired_central, test_version_follows_the_half_to_another_socket) {
    uint8_t record[DEFY_HALF_VERSION_SIZE];
    defy_half_version_encode(&left_version, record);
    struct defy_wired_packet old = data_packet(DEFY_WIRED_LEFT, 10, 0, -1);
    struct defy_wired_packet next = data_packet(DEFY_WIRED_LEFT, 11, 0, -1);

    exchange(0, &old, 1);
    version_exchange(0, record, 2, false);
    zassert_equal(versions.slot, 0);
    exchange(1, &next, 3);
    version_exchange(1, record, 4, false);
    zassert_equal(versions.received, 1);
    exchange(1, &next, 251);
    zassert_equal(versions.down[DEFY_WIRED_LEFT], 1);
    version_exchange(1, record, 252, false);
    zassert_equal(versions.received, 2);
    zassert_equal(versions.slot, 1);
}

ZTEST_SUITE(defy_wired_central, NULL, NULL, reset, NULL, NULL);
