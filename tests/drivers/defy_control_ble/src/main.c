/*
 * The Neuron's client of the halves' control service, for the version
 * report: its discovery, the read once per connection, the retry and the
 * records it reports. tests/drivers/defy_battery_central covers the battery
 * and lighting requests, without the version report.
 *
 * SPDX-License-Identifier: MIT
 */

#include <errno.h>
#include <string.h>

#include <zephyr/bluetooth/conn.h>
#include <zephyr/bluetooth/gatt.h>
#include <zephyr/bluetooth/uuid.h>
#include <zephyr/kernel.h>
#include <zephyr/ztest.h>

#include <dygma/defy_battery.h>
#include <dygma/defy_battery_central.h>
#include <dygma/defy_half_version.h>
#include <dygma/defy_hybrid.h>
#include <dygma/defy_lighting.h>

#include "fake_conn.h"
#include "fake_uptime.h"

static bool maintenance;
bool defy_hybrid_central_is_maintenance(void) { return maintenance; }
void defy_hybrid_central_ble_reject(uint8_t slot, uint32_t epoch) {
    ARG_UNUSED(slot);
    ARG_UNUSED(epoch);
}

int defy_battery_central_route_live(enum defy_link route, uint8_t source, uint32_t epoch,
                                    uint8_t side) {
    zassert_equal(route, DEFY_LINK_BLE);
    ARG_UNUSED(source);
    ARG_UNUSED(epoch);
    ARG_UNUSED(side);
    return 0;
}

int defy_battery_central_route_side(enum defy_link route, uint8_t source, uint32_t epoch,
                                    uint8_t *side) {
    ARG_UNUSED(route);
    ARG_UNUSED(source);
    ARG_UNUSED(epoch);
    ARG_UNUSED(side);
    return -ENOENT;
}

void defy_battery_central_disconnect(enum defy_link route, uint8_t source, uint32_t epoch) {
    zassert_equal(route, DEFY_LINK_BLE);
    ARG_UNUSED(source);
    ARG_UNUSED(epoch);
}

static unsigned int battery_frames;
int defy_battery_central_accept(enum defy_link route, uint8_t source, uint32_t epoch,
                                const uint8_t *data, size_t length, uint32_t extra_age_ms) {
    zassert_equal(route, DEFY_LINK_BLE);
    ARG_UNUSED(source);
    ARG_UNUSED(epoch);
    ARG_UNUSED(data);
    ARG_UNUSED(extra_age_ms);
    zassert_equal(length, DEFY_BATTERY_FRAME_SIZE);
    battery_frames++;
    return 0;
}

bool defy_lighting_central_get(struct defy_lighting_state *state) {
    ARG_UNUSED(state);
    return false;
}

int defy_lighting_central_group(uint8_t side, uint8_t index, struct defy_lighting_group *group) {
    ARG_UNUSED(side);
    ARG_UNUSED(index);
    ARG_UNUSED(group);
    return -ENOENT;
}

void defy_lighting_central_wake(const struct defy_lighting_wake *wake) { ARG_UNUSED(wake); }

/* What the client reported to the records. */
static struct {
    unsigned int received;
    uint8_t link;
    uint8_t slot;
    struct defy_half_version version;
    unsigned int down;
    uint8_t down_slot;
} records;

void defy_half_version_received(enum defy_link link, uint8_t slot,
                                const struct defy_half_version *version) {
    records.received++;
    records.link = link;
    records.slot = slot;
    records.version = *version;
}

void defy_half_version_ble_down(uint8_t slot) {
    records.down++;
    records.down_slot = slot;
}

static int test_submit(struct k_work_q *queue, struct k_work *work) {
    ARG_UNUSED(queue);
    ARG_UNUSED(work);
    return 1;
}
static int64_t scheduled_delay;
static int test_reschedule(struct k_work_q *queue, struct k_work_delayable *work,
                           k_timeout_t timeout) {
    ARG_UNUSED(queue);
    ARG_UNUSED(work);
    scheduled_delay = k_ticks_to_ms_ceil64(timeout.ticks);
    return 1;
}

#define k_work_submit_to_queue test_submit
#define k_work_reschedule_for_queue test_reschedule
#include "../../../../battery/defy_control_ble.c"
#undef k_uptime_get
#undef k_work_submit_to_queue
#undef k_work_reschedule_for_queue

#define BATTERY_HANDLE 42
#define COMMAND_HANDLE 45
#define VERSION_HANDLE 48

static struct bt_gatt_discover_params *last_discovery;
static struct bt_gatt_read_params *last_read;
static unsigned int discovery_calls;
static unsigned int version_reads;
static int version_read_result;

/* How the read in progress ends. */
static read_done_t read_done(struct bt_gatt_read_params *params) {
    return CONTAINER_OF(params, struct control_peer, read)->read_done;
}

bt_security_t bt_conn_get_security(const struct bt_conn *conn) {
    ARG_UNUSED(conn);
    return BT_SECURITY_L2;
}

int bt_gatt_discover(struct bt_conn *conn, struct bt_gatt_discover_params *params) {
    ARG_UNUSED(conn);
    discovery_calls++;
    last_discovery = params;
    return 0;
}

int bt_gatt_read(struct bt_conn *conn, struct bt_gatt_read_params *params) {
    ARG_UNUSED(conn);
    last_read = params;
    if (params->single.handle == VERSION_HANDLE) {
        version_reads++;
        return version_read_result;
    }
    return 0;
}

int bt_gatt_write_without_response_cb(struct bt_conn *conn, uint16_t handle, const void *data,
                                       uint16_t length, bool sign, bt_gatt_complete_func_t func,
                                       void *user_data) {
    ARG_UNUSED(conn);
    ARG_UNUSED(handle);
    ARG_UNUSED(data);
    ARG_UNUSED(length);
    ARG_UNUSED(sign);
    ARG_UNUSED(func);
    ARG_UNUSED(user_data);
    return 0;
}

void bt_gatt_cancel(struct bt_conn *conn, void *params) {
    for (size_t i = 0; i < ARRAY_SIZE(control_peers); i++) {
        struct control_peer *peer = &control_peers[i];
        if (params == &peer->discovery) {
            peer->discovery.func(conn, NULL, &peer->discovery);
            return;
        }
        if (params == &peer->read) {
            peer->read.func(conn, BT_ATT_ERR_UNLIKELY, &peer->read, NULL, 0);
            return;
        }
    }
    zassert_unreachable();
}

static const struct defy_half_version left_version = {
    .side = DEFY_WIRED_LEFT, .program_crc = 0x2468ace0, .seal_version = 6,
    .module = "v0.1.0-7-g0123abc",
};

static void reset(void *fixture) {
    ARG_UNUSED(fixture);
    memset(control_peers, 0, sizeof(control_peers));
    memset(refs, 0, sizeof(refs));
    memset(&records, 0, sizeof(records));
    maintenance = false;
    now = 0;
    scheduled_delay = 0;
    last_discovery = NULL;
    last_read = NULL;
    discovery_calls = version_reads = battery_frames = 0;
    version_read_result = 0;
}

static void poll(void) { control_poll_work(NULL); }

/* Hands the discovery one characteristic: what its callback returns. */
static uint8_t discover(uint8_t index, const struct bt_uuid *uuid, uint16_t handle) {
    struct bt_gatt_chrc characteristic = {.uuid = uuid, .value_handle = handle};
    struct bt_gatt_attr attr = {.user_data = &characteristic};
    return last_discovery->func(connection(index), &attr, last_discovery);
}

/* Connects the half on index and runs its discovery up to the command's characteristic. */
static void discover_to_command(uint8_t index, uint8_t slot, uint32_t epoch) {
    defy_control_ble_connected(connection(index), slot, epoch);
    now += 1000;
    poll();
    zassert_not_null(last_discovery);
    zassert_equal(discover(index, &battery_uuid.uuid, BATTERY_HANDLE), BT_GATT_ITER_CONTINUE);
    zassert_equal(discover(index, &command_uuid.uuid, COMMAND_HANDLE), BT_GATT_ITER_CONTINUE);
}

/* Ends the wake report's read before the first lighting state, then polls. */
static void finish_preflight(uint8_t index) {
    now++;
    poll();
    zassert_equal(read_done(last_read), wake_read_done);
    zassert_equal(last_read->func(connection(index), 0, last_read, NULL, 0), BT_GATT_ITER_STOP);
    poll();
}

/* Delivers the battery frame read in progress, then polls. */
static void finish_battery(uint8_t index) {
    struct defy_battery_frame frame = {
        .side = DEFY_WIRED_LEFT, .session = 10,
        .sample = {.sequence = 1, .words = {0x4031, 0, 0, 0, 0x5080, 0xc800, 0xff80, 0x1284}},
    };
    uint8_t data[DEFY_BATTERY_FRAME_SIZE];
    zassert_equal(read_done(last_read), battery_read_done);
    zassert_equal(last_read->single.handle, BATTERY_HANDLE);
    zassert_ok(defy_battery_frame_encode(&frame, data));
    zassert_equal(last_read->func(connection(index), 0, last_read, data, sizeof(data)),
                  BT_GATT_ITER_CONTINUE);
    zassert_equal(last_read->func(connection(index), 0, last_read, NULL, 0), BT_GATT_ITER_STOP);
    poll();
}

/* A connection whose discovery finds the version report, up to the report's read. */
static void reach_version_read(uint8_t index, uint8_t slot, uint32_t epoch) {
    discover_to_command(index, slot, epoch);
    zassert_equal(discover(index, &version_uuid.uuid, VERSION_HANDLE), BT_GATT_ITER_STOP);
    finish_preflight(index);
    /* The battery frame goes first. */
    finish_battery(index);
    zassert_equal(read_done(last_read), version_read_done);
    zassert_equal(last_read->single.handle, VERSION_HANDLE);
}

/* Delivers record as a read in two parts, as a 23-byte MTU splits it, then polls. */
static void deliver_version(uint8_t index, const uint8_t *record, size_t length) {
    size_t first = MIN(length, 22);
    zassert_equal(last_read->func(connection(index), 0, last_read, record, first),
                  first <= DEFY_HALF_VERSION_SIZE ? BT_GATT_ITER_CONTINUE : BT_GATT_ITER_STOP);
    if (length > first) {
        last_read->func(connection(index), 0, last_read, record + first, length - first);
    }
    last_read->func(connection(index), 0, last_read, NULL, 0);
    poll();
}

static void deliver_left(uint8_t index) {
    uint8_t record[DEFY_HALF_VERSION_SIZE];
    defy_half_version_encode(&left_version, record);
    deliver_version(index, record, sizeof(record));
}

/* Runs the polls of the next periods, finishing each battery read they start. */
static void run_periods(uint8_t index, unsigned int count) {
    for (unsigned int i = 0; i < count; i++) {
        now += BATTERY_POLL_MS;
        poll();
        finish_battery(index);
    }
}

ZTEST(defy_control_ble, test_version_is_read_once_after_discovery) {
    reach_version_read(0, 1, 5);
    deliver_left(0);
    zassert_equal(records.received, 1);
    zassert_equal(records.link, DEFY_LINK_BLE);
    zassert_equal(records.slot, 1);
    zassert_equal(records.version.side, DEFY_WIRED_LEFT);
    zassert_equal(records.version.program_crc, 0x2468ace0);
    zassert_equal(records.version.seal_version, 6);
    zassert_str_equal(records.version.module, "v0.1.0-7-g0123abc");
    run_periods(0, 3);
    zassert_equal(version_reads, 1);
    zassert_equal(discovery_calls, 1);
    zassert_equal(records.received, 1);
    zassert_equal(battery_frames, 4);
}

ZTEST(defy_control_ble, test_failed_read_is_retried_after_a_second) {
    reach_version_read(0, 0, 1);
    zassert_equal(last_read->func(connection(0), BT_ATT_ERR_INSUFFICIENT_ENCRYPTION, last_read,
                                  NULL, 0), BT_GATT_ITER_STOP);
    poll();
    zassert_equal(records.received, 0);
    zassert_equal(scheduled_delay, RETRY_MS);
    now += RETRY_MS - 1;
    poll();
    zassert_equal(version_reads, 1);
    now++;
    poll();
    zassert_equal(version_reads, 2);
    zassert_equal(read_done(last_read), version_read_done);
    deliver_left(0);
    zassert_equal(records.received, 1);
    run_periods(0, 2);
    zassert_equal(version_reads, 2);
}

ZTEST(defy_control_ble, test_failed_request_is_retried_after_a_second) {
    version_read_result = -ENOMEM;
    reach_version_read(0, 0, 1);
    zassert_false(control_peers[0].busy);
    version_read_result = 0;
    now += RETRY_MS - 1;
    poll();
    zassert_equal(version_reads, 1);
    now++;
    poll();
    zassert_equal(version_reads, 2);
    deliver_left(0);
    zassert_equal(records.received, 1);
    /* The battery frame keeps its own schedule. */
    zassert_equal(battery_frames, 1);
    run_periods(0, 1);
    zassert_equal(battery_frames, 2);
}

ZTEST(defy_control_ble, test_half_without_the_report_is_served_as_before) {
    /* Another service's characteristic follows the command's: the discovery stops there. */
    discover_to_command(0, 0, 1);
    zassert_equal(discover(0, BT_UUID_DECLARE_16(0x2a19), 50), BT_GATT_ITER_STOP);
    finish_preflight(0);
    finish_battery(0);
    run_periods(0, 2);
    zassert_equal(version_reads, 0);
    zassert_equal(discovery_calls, 1);
    defy_control_ble_disconnected(connection(0), 0, 1);
    poll();
    zassert_equal(refs[0], 0);

    /* Or the discovery ends with the command's. */
    discover_to_command(1, 1, 2);
    zassert_equal(last_discovery->func(connection(1), NULL, last_discovery), BT_GATT_ITER_STOP);
    finish_preflight(1);
    finish_battery(1);
    run_periods(1, 2);
    zassert_equal(version_reads, 0);
    zassert_equal(discovery_calls, 2);
    zassert_equal(records.received, 0);
    zassert_equal(battery_frames, 6);
}

ZTEST(defy_control_ble, test_report_that_does_not_decode_is_not_read_again) {
    uint8_t record[DEFY_HALF_VERSION_SIZE + 1];
    defy_half_version_encode(&left_version, record);
    record[DEFY_HALF_VERSION_SIZE] = 0;
    /* A byte short or a byte long. */
    const size_t lengths[] = {DEFY_HALF_VERSION_SIZE - 1, DEFY_HALF_VERSION_SIZE + 1};
    for (uint8_t i = 0; i < ARRAY_SIZE(lengths); i++) {
        reach_version_read(0, 0, i + 1);
        deliver_version(0, record, lengths[i]);
        now += 3 * RETRY_MS;
        poll();
        zassert_equal(version_reads, i + 1);
        defy_control_ble_disconnected(connection(0), 0, i + 1);
        poll();
        zassert_equal(refs[0], 0);
    }
    zassert_equal(records.received, 0);
}

ZTEST(defy_control_ble, test_disconnection_drops_the_slots_records) {
    reach_version_read(0, 1, 5);
    deliver_left(0);
    zassert_equal(records.received, 1);
    defy_control_ble_disconnected(connection(0), 1, 5);
    zassert_equal(records.down, 1);
    zassert_equal(records.down_slot, 1);
    poll();
    zassert_equal(refs[0], 0);

    /* A read that ends after its link reports nothing. */
    reach_version_read(1, 0, 7);
    defy_control_ble_disconnected(connection(1), 0, 7);
    zassert_equal(records.down, 2);
    zassert_equal(records.down_slot, 0);
    uint8_t record[DEFY_HALF_VERSION_SIZE];
    defy_half_version_encode(&left_version, record);
    zassert_equal(last_read->func(connection(1), 0, last_read, record, sizeof(record)),
                  BT_GATT_ITER_STOP);
    zassert_equal(last_read->func(connection(1), 0, last_read, NULL, 0), BT_GATT_ITER_STOP);
    zassert_equal(records.received, 1);
    poll();
    zassert_equal(refs[1], 0);
    zassert_equal(version_reads, 2);
}

ZTEST(defy_control_ble, test_maintenance_holds_the_report_until_it_ends) {
    reach_version_read(0, 0, 1);
    maintenance = true;
    deliver_left(0);
    zassert_equal(records.received, 0);
    now += RETRY_MS;
    poll();
    zassert_equal(version_reads, 1);
    maintenance = false;
    poll();
    zassert_equal(version_reads, 2);
    deliver_left(0);
    zassert_equal(records.received, 1);
}

ZTEST(defy_control_ble, test_discovery_again_for_a_missing_battery_reads_the_report_once) {
    defy_control_ble_connected(connection(0), 0, 1);
    now += 1000;
    poll();
    zassert_equal(discover(0, &command_uuid.uuid, COMMAND_HANDLE), BT_GATT_ITER_CONTINUE);
    zassert_equal(discover(0, &version_uuid.uuid, VERSION_HANDLE), BT_GATT_ITER_CONTINUE);
    zassert_equal(last_discovery->func(connection(0), NULL, last_discovery), BT_GATT_ITER_STOP);
    finish_preflight(0);
    zassert_equal(read_done(last_read), version_read_done);
    deliver_left(0);
    zassert_equal(records.received, 1);
    for (unsigned int i = 0; i < 2; i++) {
        now += BATTERY_POLL_MS;
        poll();
        zassert_equal(discovery_calls, 2 + i);
        zassert_equal(discover(0, &command_uuid.uuid, COMMAND_HANDLE), BT_GATT_ITER_CONTINUE);
        zassert_equal(discover(0, &version_uuid.uuid, VERSION_HANDLE), BT_GATT_ITER_CONTINUE);
        zassert_equal(last_discovery->func(connection(0), NULL, last_discovery),
                      BT_GATT_ITER_STOP);
        poll();
    }
    zassert_equal(version_reads, 1);
    zassert_equal(records.received, 1);
}

ZTEST_SUITE(defy_control_ble, NULL, NULL, reset, NULL, NULL);
