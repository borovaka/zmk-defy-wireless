/* SPDX-License-Identifier: MIT */

#include <errno.h>
#include <string.h>

#include <zephyr/bluetooth/conn.h>
#include <zephyr/bluetooth/gatt.h>
#include <zephyr/bluetooth/uuid.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/byteorder.h>
#include <zephyr/ztest.h>

#include <dygma/defy_battery_central.h>
#include <dygma/defy_lighting.h>
#include <dygma/defy_feedback.h>

#include "fake_conn.h"
#include "fake_uptime.h"

static bool maintenance_active_test;
bool defy_hybrid_central_is_maintenance(void) { return maintenance_active_test; }
static unsigned int rejected_peers;
static uint8_t rejected_slot;
static uint32_t rejected_epoch;
void defy_hybrid_central_ble_reject(uint8_t slot, uint32_t epoch) {
    rejected_peers++;
    rejected_slot = slot;
    rejected_epoch = epoch;
}


static bool lighting_available;
static struct defy_lighting_state desired_lighting;
static struct defy_battery_frame analog_sample(uint8_t side, uint32_t sequence,
                                               uint16_t threshold, uint16_t reference);
static unsigned int wake_calls;
static struct defy_lighting_wake observed_wake;
static void (*wake_hook)(void);
bool defy_lighting_central_get(struct defy_lighting_state *state) {
    *state = desired_lighting;
    return lighting_available;
}
void defy_lighting_central_wake(const struct defy_lighting_wake *wake) {
    wake_calls++;
    observed_wake = *wake;
    if (wake_hook) {
        wake_hook();
    }
}
/* The groups the client asked for, the side of the last, and an error to give instead. */
static unsigned int group_calls;
static uint8_t group_side;
static int group_error;
int defy_lighting_central_group(uint8_t side, uint8_t index, struct defy_lighting_group *group) {
    group_calls++;
    group_side = side;
    if (group_error) {
        return group_error;
    }
    *group = (struct defy_lighting_group){
        .boot = desired_lighting.boot, .index = index, .colour = {index, side, 0x80},
        .leds = BIT64(index) | BIT64(34 - side),
    };
    return 0;
}

static struct k_work_delayable *host_scheduled_work;
static int64_t host_work_due;
static int test_host_reschedule(struct k_work_delayable *work, k_timeout_t timeout) {
    host_scheduled_work = work;
    host_work_due = now + k_ticks_to_ms_ceil64(timeout.ticks);
    return 1;
}
static int test_host_schedule(struct k_work_delayable *work, k_timeout_t timeout) {
    if (host_work_due != INT64_MAX) {
        return 0;
    }
    return test_host_reschedule(work, timeout);
}
/* The work last submitted, and its queue. */
static struct k_work *last_submitted;
static struct k_work_q *submitted_queue;
static int test_submit(struct k_work_q *queue, struct k_work *work) {
    submitted_queue = queue;
    last_submitted = work;
    return 1;
}
static struct k_work_q *scheduled_queue;
static int64_t scheduled_delay;
static int test_reschedule(struct k_work_q *queue, struct k_work_delayable *work,
                           k_timeout_t timeout) {
    ARG_UNUSED(work);
    scheduled_queue = queue;
    scheduled_delay = k_ticks_to_ms_ceil64(timeout.ticks);
    return 1;
}

#define k_work_submit_to_queue test_submit
#define k_work_reschedule test_host_reschedule
#define k_work_schedule test_host_schedule
#define k_work_reschedule_for_queue test_reschedule
#include "../../../../battery/defy_battery_central.c"
#include "../../../../battery/defy_battery_host.c"
/* One translation unit here, whose log module defy_battery_central.c registered. */
#undef LOG_MODULE_REGISTER
#define LOG_MODULE_REGISTER(...)
#include "../../../../battery/defy_control_ble.c"
#undef k_uptime_get
#undef k_work_submit_to_queue
#undef k_work_reschedule
#undef k_work_schedule
#undef k_work_reschedule_for_queue

static int discovery_result;
static int read_result;
static int discovery_calls;
static int read_calls;
static int cancel_calls;
static int notify_calls;
static uint8_t notified_level;
static bool host_live[6];
static bool host_subscribed[6];
static unsigned int host_notify_calls[6];
static int host_notify_result[6];
static void (*notify_hook)(uint8_t index);
static struct bt_gatt_discover_params *last_discovery;
static struct bt_gatt_read_params *last_read;
static bool disconnect_during_read_start;
static int write_result;
static unsigned int write_calls;
static uint8_t written_peer;
static uint16_t written_handle;
static struct defy_lighting_state written_lighting;
static struct defy_lighting_group written_group;
static struct defy_host_feedback written_feedback;
static bt_security_t security_level;
/* The first writes in order, and the one, counted from 1, that fails for want of buffers. */
static struct {
    uint8_t peer;
    uint16_t length;
    uint8_t data[DEFY_LIGHTING_CONTROL_SIZE];
} write_log[32];
static unsigned int failing_write;

static void run_bas_work(void) {
    zassert_equal(host_scheduled_work, &battery_notify);
    zassert_not_equal(host_work_due, INT64_MAX);
    now = MAX(now, host_work_due);
    host_work_due = INT64_MAX;
    host_scheduled_work->work.handler(&host_scheduled_work->work);
}

/* How the read in progress ends. */
static read_done_t read_done(struct bt_gatt_read_params *params) {
    return CONTAINER_OF(params, struct control_peer, read)->read_done;
}
void bt_conn_foreach(enum bt_conn_type type, void (*func)(struct bt_conn *, void *), void *data) {
    zassert_equal(type, BT_CONN_TYPE_LE);
    for (size_t i = 0; i < ARRAY_SIZE(tokens); i++) {
        if (host_live[i]) {
            func(connection(i), data);
        }
    }
}
bool bt_gatt_is_subscribed(struct bt_conn *conn, const struct bt_gatt_attr *attr,
                           uint16_t ccc_type) {
    zassert_true(attr == &defy_battery_bas.attrs[1] || attr == &defy_battery_bas.attrs[3]);
    zassert_equal(ccc_type, BT_GATT_CCC_NOTIFY);
    uint8_t index = bt_conn_index(conn);
    return host_live[index] && host_subscribed[index];
}
bt_security_t bt_conn_get_security(const struct bt_conn *conn) {
    ARG_UNUSED(conn);
    return security_level;
}
int bt_gatt_discover(struct bt_conn *conn, struct bt_gatt_discover_params *params) {
    ARG_UNUSED(conn);
    discovery_calls++;
    last_discovery = params;
    return discovery_result;
}
int bt_gatt_read(struct bt_conn *conn, struct bt_gatt_read_params *params) {
    ARG_UNUSED(conn);
    read_calls++;
    last_read = params;
    if (disconnect_during_read_start) {
        struct control_peer *peer = CONTAINER_OF(params, struct control_peer, read);
        defy_control_ble_disconnected(conn, peer->slot, peer->epoch);
    }
    return read_result;
}
int bt_gatt_write_without_response_cb(struct bt_conn *conn, uint16_t handle, const void *data,
                                       uint16_t length, bool sign, bt_gatt_complete_func_t func,
                                       void *user_data) {
    ARG_UNUSED(func); ARG_UNUSED(user_data);
    zassert_false(sign);
    const uint8_t *command = data;
    zassert_true(length >= DEFY_CONTROL_HEADER_SIZE);
    /* The state and the host display have the same length: the opcode tells them apart. */
    if (command[1] == DEFY_CONTROL_LIGHTING_SET) {
        zassert_ok(defy_lighting_control_decode(data, length, DEFY_CONTROL_LIGHTING_SET,
                                                &written_lighting));
    } else if (command[1] == DEFY_CONTROL_LIGHTING_GROUP) {
        zassert_ok(defy_lighting_group_decode(data, length, &written_group));
    } else if (command[1] == DEFY_CONTROL_HOST_FEEDBACK) {
        zassert_ok(defy_host_feedback_decode(data, length, &written_feedback));
    } else {
        zassert_equal(length, DEFY_CONTROL_BATTERY_SHOW_SIZE);
        zassert_equal(command[0], DEFY_CONTROL_BATTERY_SHOW_VERSION);
        zassert_equal(command[1], DEFY_CONTROL_BATTERY_SHOW);
    }
    if (write_calls < ARRAY_SIZE(write_log)) {
        zassert_true(length <= sizeof(write_log[0].data));
        write_log[write_calls].peer = bt_conn_index(conn);
        write_log[write_calls].length = length;
        memcpy(write_log[write_calls].data, data, length);
    }
    write_calls++;
    written_peer = bt_conn_index(conn);
    written_handle = handle;
    return write_calls == failing_write ? -ENOMEM : write_result;
}
void bt_gatt_cancel(struct bt_conn *conn, void *params) {
    cancel_calls++;
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
ssize_t bt_gatt_attr_read(struct bt_conn *conn, const struct bt_gatt_attr *attr, void *buf,
                          uint16_t length, uint16_t offset, const void *value, uint16_t size) {
    ARG_UNUSED(conn); ARG_UNUSED(attr);
    if (offset > size) { return BT_GATT_ERR(BT_ATT_ERR_INVALID_OFFSET); }
    length = MIN(length, size - offset);
    memcpy(buf, (const uint8_t *)value + offset, length);
    return length;
}
int bt_gatt_notify_cb(struct bt_conn *conn, struct bt_gatt_notify_params *params) {
    zassert_not_null(conn);
    uint8_t index = bt_conn_index(conn);
    zassert_true(host_live[index] && host_subscribed[index]);
    zassert_equal(params->len, 1);
    notify_calls++;
    host_notify_calls[index]++;
    notified_level = *(const uint8_t *)params->data;
    if (notify_hook) {
        notify_hook(index);
    }
    return host_notify_result[index];
}

#define READ_STUB(name)                                                                            \
    ssize_t name(struct bt_conn *conn, const struct bt_gatt_attr *attr, void *buf,                    \
                  uint16_t length, uint16_t offset) {                                               \
        ARG_UNUSED(conn); ARG_UNUSED(attr); ARG_UNUSED(buf); ARG_UNUSED(length); ARG_UNUSED(offset); \
        return 0;                                                                                  \
    }
READ_STUB(bt_gatt_attr_read_service)
READ_STUB(bt_gatt_attr_read_chrc)
READ_STUB(bt_gatt_attr_read_ccc)
READ_STUB(bt_gatt_attr_read_cpf)
ssize_t bt_gatt_attr_write_ccc(struct bt_conn *conn, const struct bt_gatt_attr *attr,
                              const void *buf, uint16_t length, uint16_t offset, uint8_t flags) {
    ARG_UNUSED(flags);
    if (offset) {
        return BT_GATT_ERR(BT_ATT_ERR_INVALID_OFFSET);
    }
    if (!length || length > sizeof(uint16_t)) {
        return BT_GATT_ERR(BT_ATT_ERR_INVALID_ATTRIBUTE_LEN);
    }
    struct _bt_gatt_ccc *ccc = attr->user_data;
    uint16_t value = length == 1 ? *(const uint8_t *)buf : sys_get_le16(buf);
    if (ccc->cfg_write) {
        ssize_t result = ccc->cfg_write(conn, attr, value);
        if (result < 0) {
            return result;
        }
    }
    host_subscribed[bt_conn_index(conn)] = (value & BT_GATT_CCC_NOTIFY) != 0;
    uint16_t aggregate = 0;
    for (size_t i = 0; i < ARRAY_SIZE(tokens); i++) {
        if (host_live[i] && host_subscribed[i]) {
            aggregate = BT_GATT_CCC_NOTIFY;
        }
    }
    if (aggregate != ccc->value) {
        ccc->value = aggregate;
        if (ccc->cfg_changed) {
            ccc->cfg_changed(attr, aggregate);
        }
    }
    return length;
}

static struct defy_battery_frame sample(uint8_t side, uint32_t session, uint32_t sequence,
                                         uint8_t level) {
    return (struct defy_battery_frame){
        .side = side, .session = session,
        .sample = {.sequence = sequence, .words = {[DEFY_BATTERY_DEVNAME] = 0x4031,
                 [DEFY_BATTERY_REPSOC] = level * 256, [DEFY_BATTERY_VCELL] = 48000,
                 [DEFY_BATTERY_DESIGNCAP] = 0x1284}},
    };
}
static int accept(enum defy_link route, uint8_t source, uint32_t epoch,
                   struct defy_battery_frame frame) {
    uint8_t bytes[DEFY_BATTERY_FRAME_SIZE];
    zassert_ok(defy_battery_frame_encode(&frame, bytes));
    return defy_battery_central_accept(route, source, epoch, bytes, sizeof(bytes), 0);
}
static void connect_sample(enum defy_link route, uint8_t source, uint32_t epoch,
                            uint8_t side, uint8_t level) {
    zassert_ok(defy_battery_central_route_live(route, source, epoch, side));
    zassert_ok(accept(route, source, epoch, sample(side, 10 + side, 1, level)));
}
static struct defy_battery_record record(uint8_t side) {
    struct defy_battery_record result;
    zassert_ok(defy_battery_central_get(side, &result));
    return result;
}
static void reset(void *fixture) {
    ARG_UNUSED(fixture);
    memset(routes, 0, sizeof(routes));
    memset(battery_sides, 0, sizeof(battery_sides));
    memset(control_peers, 0, sizeof(control_peers));
    memset(diagnostic_reads, 0, sizeof(diagnostic_reads));
    memset(host_notifications, 0, sizeof(host_notifications));
    memset(host_live, 0, sizeof(host_live));
    memset(host_subscribed, 0, sizeof(host_subscribed));
    memset(host_notify_calls, 0, sizeof(host_notify_calls));
    memset(host_notify_result, 0, sizeof(host_notify_result));
    host_live[0] = host_subscribed[0] = true;
    level_ccc.value = BT_GATT_CCC_NOTIFY;
    battery_revision = sample_revision = 0;
    notify_hook = NULL;
    memset(refs, 0, sizeof(refs));
    now = 0;
    host_scheduled_work = NULL;
    host_work_due = INT64_MAX;
    discovery_result = read_result = 0;
    discovery_calls = read_calls = cancel_calls = notify_calls = 0;
    last_discovery = NULL;
    last_read = NULL;
    disconnect_during_read_start = false;
    scheduled_queue = NULL;
    submitted_queue = NULL;
    last_submitted = NULL;
    write_result = 0;
    write_calls = 0;
    failing_write = 0;
    memset(write_log, 0, sizeof(write_log));
    group_calls = 0;
    group_side = UINT8_MAX;
    group_error = 0;
    written_group = (struct defy_lighting_group){0};
    lighting_available = false;
    wake_calls = 0;
    observed_wake = (struct defy_lighting_wake){0};
    wake_hook = NULL;
    rejected_peers = 0;
    maintenance_active_test = false;
    desired_lighting = (struct defy_lighting_state){.boot = 7, .revision = 1, .enabled = true};
    written_lighting = (struct defy_lighting_state){0};
    written_feedback = (struct defy_host_feedback){0};
    security_level = BT_SECURITY_L2;
    scheduled_delay = 0;
}

ZTEST(defy_battery_central, test_boot_and_missing_half_have_no_aggregate) {
    uint8_t level = 47;
    zassert_equal(defy_battery_central_aggregate(&level, NULL), -ENODATA);
    zassert_equal(level, 47);
    connect_sample(DEFY_LINK_WIRED, 0, 1, 0, 80);
    zassert_equal(defy_battery_central_aggregate(&level, NULL), -ENODATA);
    uint8_t byte;
    zassert_equal(read_level(connection(0), NULL, &byte, 1, 0), BT_GATT_ERR(BT_ATT_ERR_UNLIKELY));
}
ZTEST(defy_battery_central, test_swapped_sources_and_minimum_real_zero) {
    connect_sample(DEFY_LINK_BLE, 0, 1, 1, 100);
    connect_sample(DEFY_LINK_WIRED, 1, 1, 0, 0);
    uint8_t level = 100;
    zassert_ok(defy_battery_central_aggregate(&level, NULL));
    zassert_equal(level, 0);
    zassert_equal(record(1).reading.level, 100);
}

ZTEST(defy_battery_central, test_host_cache_requires_valid_sample_from_each_half) {
    uint8_t level = 47;
    uint32_t revision = 71;
    zassert_equal(defy_battery_central_aggregate(NULL, &revision), -EINVAL);
    connect_sample(DEFY_LINK_WIRED, 0, 1, 0, 80);
    zassert_ok(defy_battery_central_route_live(DEFY_LINK_WIRED, 1, 1, 1));
    struct defy_battery_frame frame = sample(1, 11, 1, 60);
    frame.sample.status = DEFY_BATTERY_SAMPLE_IO_ERROR;
    zassert_ok(accept(DEFY_LINK_WIRED, 1, 1, frame));
    zassert_equal(defy_battery_central_aggregate(&level, &revision), -ENODATA);
    zassert_equal(level, 47);
    zassert_equal(revision, 71);
    frame = sample(1, 11, 2, 60);
    frame.sample.age_ms = DEFY_BATTERY_MAX_AGE_MS;
    zassert_ok(accept(DEFY_LINK_WIRED, 1, 1, frame));
    zassert_equal(defy_battery_central_aggregate(&level, &revision), -ENODATA);
    zassert_ok(accept(DEFY_LINK_WIRED, 1, 1, sample(1, 11, 3, 60)));
    zassert_ok(defy_battery_central_aggregate(&level, &revision));
    zassert_equal(level, 60);
}

ZTEST(defy_battery_central, test_sleeping_cache_keeps_live_diagnostic_flags_and_updates_one_side) {
    connect_sample(DEFY_LINK_WIRED, 0, 1, 0, 80);
    connect_sample(DEFY_LINK_BLE, 1, 1, 1, 60);
    defy_battery_central_disconnect(DEFY_LINK_WIRED, 0, 1);
    defy_battery_central_disconnect(DEFY_LINK_BLE, 1, 1);
    now = DEFY_BATTERY_MAX_AGE_MS + 1000;
    uint8_t level;
    zassert_ok(defy_battery_central_aggregate(&level, NULL));
    zassert_equal(level, 60);
    for (uint8_t side = 0; side < 2; side++) {
        struct defy_battery_record current = record(side);
        zassert_true(current.present);
        zassert_false(current.connected || current.fresh || current.valid);
        uint8_t diagnostic[DEFY_BATTERY_DIAGNOSTIC_SIZE];
        zassert_ok(defy_battery_central_diagnostic(side, diagnostic));
        zassert_equal(diagnostic[2], BIT(0));
    }
    zassert_ok(defy_battery_central_route_live(DEFY_LINK_WIRED, 0, 2, 0));
    zassert_ok(accept(DEFY_LINK_WIRED, 0, 2, sample(0, 10, 2, 50)));
    zassert_ok(defy_battery_central_aggregate(&level, NULL));
    zassert_equal(level, 50);
    zassert_ok(accept(DEFY_LINK_WIRED, 0, 2, sample(0, 10, 3, 90)));
    zassert_ok(defy_battery_central_aggregate(&level, NULL));
    zassert_equal(level, 60);
}

ZTEST(defy_battery_central, test_cache_revision_ignores_invalid_duplicate_and_rejected_frames) {
    connect_sample(DEFY_LINK_WIRED, 0, 1, 0, 80);
    connect_sample(DEFY_LINK_BLE, 1, 1, 1, 60);
    uint8_t level;
    uint32_t revision, original;
    zassert_ok(defy_battery_central_aggregate(&level, &original));
    zassert_ok(accept(DEFY_LINK_WIRED, 0, 1, sample(0, 10, 1, 80)));
    struct defy_battery_frame frame = sample(0, 10, 2, 5);
    frame.sample.status = DEFY_BATTERY_SAMPLE_IO_ERROR;
    zassert_ok(accept(DEFY_LINK_WIRED, 0, 1, frame));
    zassert_false(record(0).valid);
    frame = sample(0, 10, 3, 5);
    frame.sample.age_ms = DEFY_BATTERY_MAX_AGE_MS;
    zassert_ok(accept(DEFY_LINK_WIRED, 0, 1, frame));
    zassert_equal(accept(DEFY_LINK_WIRED, 0, 1, sample(0, 10, 1, 1)), -ESTALE);
    zassert_equal(accept(DEFY_LINK_WIRED, 0, 2, sample(0, 10, 4, 1)), -ENOTCONN);
    zassert_ok(defy_battery_central_aggregate(&level, &revision));
    zassert_equal(revision, original);
    zassert_equal(level, 60);
    zassert_ok(accept(DEFY_LINK_WIRED, 0, 1, sample(0, 10, 4, 40)));
    zassert_ok(defy_battery_central_aggregate(&level, &revision));
    zassert_equal(revision, original + 1);
    zassert_equal(level, 40);
}
ZTEST(defy_battery_central, test_unknown_ble_side_is_identified_before_any_key) {
    zassert_ok(defy_battery_central_route_live(DEFY_LINK_BLE, 0, 1,
                                             DEFY_WIRED_SIDE_UNKNOWN));
    zassert_ok(accept(DEFY_LINK_BLE, 0, 1, sample(1, 11, 1, 65)));
    zassert_true(record(1).connected);
    zassert_false(record(0).present);
}
ZTEST(defy_battery_central, test_later_key_identity_conflict_removes_wrong_reading) {
    zassert_ok(defy_battery_central_route_live(DEFY_LINK_BLE, 0, 1,
                                             DEFY_WIRED_SIDE_UNKNOWN));
    zassert_ok(accept(DEFY_LINK_BLE, 0, 1, sample(1, 11, 1, 65)));
    zassert_equal(defy_battery_central_route_identify(DEFY_LINK_BLE, 0, 1, 0), -EPROTO);
    zassert_false(record(1).connected);
    zassert_equal(accept(DEFY_LINK_BLE, 0, 1, sample(1, 11, 2, 65)), -EPROTO);
}
ZTEST(defy_battery_central, test_delayed_key_identity_cannot_revive_disconnected_epoch) {
    connect_sample(DEFY_LINK_BLE, 0, 1, 0, 80);
    defy_battery_central_disconnect(DEFY_LINK_BLE, 0, 1);
    zassert_equal(defy_battery_central_route_identify(DEFY_LINK_BLE, 0, 1, 0), -ENOTCONN);
    zassert_false(record(0).connected);
    zassert_equal(accept(DEFY_LINK_BLE, 0, 1, sample(0, 10, 2, 79)), -ENOTCONN);
}
ZTEST(defy_battery_central, test_known_side_rejects_wrong_frame) {
    zassert_ok(defy_battery_central_route_live(DEFY_LINK_WIRED, 0, 1, 0));
    zassert_equal(accept(DEFY_LINK_WIRED, 0, 1, sample(1, 11, 1, 65)), -EPROTO);
    zassert_false(record(1).present);
}
ZTEST(defy_battery_central, test_duplicate_physical_side_waits_for_incumbent_disconnect) {
    connect_sample(DEFY_LINK_BLE, 0, 1, 0, 80);
    zassert_ok(defy_battery_central_route_live(DEFY_LINK_BLE, 1, 1,
                                             DEFY_WIRED_SIDE_UNKNOWN));
    zassert_equal(accept(DEFY_LINK_BLE, 1, 1, sample(0, 20, 1, 70)), -EADDRINUSE);
    zassert_equal(record(0).reading.level, 80);
    defy_battery_central_disconnect(DEFY_LINK_BLE, 0, 1);
    zassert_ok(accept(DEFY_LINK_BLE, 1, 1, sample(0, 20, 1, 70)));
    zassert_equal(record(0).reading.level, 70);
}
ZTEST(defy_battery_central, test_malformed_frame_does_not_replace_valid_sample) {
    connect_sample(DEFY_LINK_WIRED, 0, 1, 0, 80);
    uint8_t data[DEFY_BATTERY_FRAME_SIZE];
    struct defy_battery_frame frame = sample(0, 10, 2, 79);
    zassert_ok(defy_battery_frame_encode(&frame, data));
    data[14] ^= 1;
    zassert_equal(defy_battery_central_accept(DEFY_LINK_WIRED, 0, 1, data, sizeof(data), 0),
                  -EBADMSG);
    zassert_equal(record(0).reading.level, 80);
}
ZTEST(defy_battery_central, test_stale_boundary_and_duplicate_does_not_renew) {
    connect_sample(DEFY_LINK_WIRED, 0, 1, 0, 80);
    now = 89999;
    zassert_true(record(0).fresh);
    zassert_ok(accept(DEFY_LINK_WIRED, 0, 1, sample(0, 10, 1, 80)));
    now = 90000;
    zassert_false(record(0).fresh);
    zassert_false(record(0).valid);
}
ZTEST(defy_battery_central, test_new_sequence_refreshes_and_old_sequence_is_rejected) {
    connect_sample(DEFY_LINK_WIRED, 0, 1, 0, 80);
    now = 90000;
    zassert_ok(accept(DEFY_LINK_WIRED, 0, 1, sample(0, 10, 2, 79)));
    zassert_true(record(0).fresh);
    zassert_equal(accept(DEFY_LINK_WIRED, 0, 1, sample(0, 10, 1, 80)), -ESTALE);
    zassert_equal(record(0).reading.level, 79);
}
ZTEST(defy_battery_central, test_mutated_duplicate_is_rejected) {
    connect_sample(DEFY_LINK_WIRED, 0, 1, 0, 80);
    zassert_equal(accept(DEFY_LINK_WIRED, 0, 1, sample(0, 10, 1, 99)), -EPROTO);
    zassert_equal(record(0).reading.level, 80);
}
ZTEST(defy_battery_central, test_reused_route_rejects_old_epoch_and_disconnect) {
    connect_sample(DEFY_LINK_WIRED, 0, 1, 0, 80);
    connect_sample(DEFY_LINK_WIRED, 0, 2, 1, 70);
    defy_battery_central_disconnect(DEFY_LINK_WIRED, 0, 1);
    zassert_true(record(1).connected);
    zassert_false(record(0).connected);
    zassert_equal(accept(DEFY_LINK_WIRED, 0, 1, sample(0, 10, 2, 79)), -ENOTCONN);
}
ZTEST(defy_battery_central, test_radio_restart_rejects_late_old_session) {
    connect_sample(DEFY_LINK_WIRED, 0, 1, 0, 80);
    connect_sample(DEFY_LINK_BLE, 1, 2, 0, 80);
    zassert_ok(accept(DEFY_LINK_WIRED, 0, 1, sample(0, 20, 1, 79)));
    zassert_equal(accept(DEFY_LINK_BLE, 1, 2, sample(0, 10, 2, 80)), -ESTALE);
    zassert_ok(accept(DEFY_LINK_BLE, 1, 2, sample(0, 20, 1, 79)));
}
ZTEST(defy_battery_central, test_diagnostics_flags_and_long_read_snapshot) {
    uint8_t data[DEFY_BATTERY_DIAGNOSTIC_SIZE];
    struct bt_gatt_attr attr = {.user_data = (void *)0};
    zassert_equal(read_diagnostic(connection(0), &attr, data, 22, 0), 22);
    zassert_equal(data[2], 0);
    connect_sample(DEFY_LINK_WIRED, 0, 1, 0, 80);
    zassert_equal(read_diagnostic(connection(0), &attr, data + 22, 14, 22), 14);
    for (size_t i = 4; i < sizeof(data); i++) { zassert_equal(data[i], 0); }
    zassert_equal(read_diagnostic(connection(0), &attr, data, sizeof(data), 0), sizeof(data));
    zassert_equal(data[2], 15);
    host_disconnected(connection(0), 0);
    zassert_equal(read_diagnostic(connection(0), &attr, data, 1, 22),
                  BT_GATT_ERR(BT_ATT_ERR_INVALID_OFFSET));
}
ZTEST(defy_battery_central, test_usb_snapshot_missing_is_explicit) {
    uint8_t data[DEFY_BATTERY_SNAPSHOT_SIZE];
    now = 123456;
    zassert_equal(defy_battery_central_snapshot(data, sizeof(data)), sizeof(data));
    zassert_equal(data[0], 1);
    zassert_equal(data[1], 2);
    zassert_equal(sys_get_le16(data + 2), 48);
    zassert_equal(sys_get_le32(data + 4), now);
    for (uint8_t side = 0; side < 2; side++) {
        uint8_t *entry = data + 8 + 48 * side;
        zassert_equal(entry[0], side);
        zassert_equal(entry[1], 0);
        zassert_equal(entry[2], 0);
        zassert_equal(sys_get_le32(entry + 4), UINT32_MAX);
        zassert_equal((int32_t)sys_get_le32(entry + 8), -ENODATA);
        zassert_equal(entry[12], UINT8_MAX);
        zassert_equal(sys_get_le16(entry + 14), UINT16_MAX);
        for (size_t i = 16; i < 48; i++) { zassert_equal(entry[i], 0); }
    }
}
ZTEST(defy_battery_central, test_usb_snapshot_valid_zero_and_matching_routes) {
    connect_sample(DEFY_LINK_WIRED, 1, 6, 0, 0);
    connect_sample(DEFY_LINK_BLE, 0, 7, 0, 0);
    uint8_t data[DEFY_BATTERY_SNAPSHOT_SIZE];
    zassert_equal(defy_battery_central_snapshot(data, sizeof(data)), sizeof(data));
    zassert_equal(data[9], 15);
    zassert_equal(data[10], BIT(DEFY_LINK_WIRED) | BIT(DEFY_LINK_BLE));
    zassert_equal(data[11], DEFY_BATTERY_SOURCE_GAUGE);
    zassert_equal(data[20], 0);
    zassert_equal(sys_get_le32(data + 16), 0);
    struct defy_battery_frame decoded;
    zassert_ok(defy_battery_frame_decode(data + 24, 32, &decoded));
    zassert_equal(decoded.side, 0);
    defy_battery_central_disconnect(DEFY_LINK_WIRED, 1, 6);
    zassert_equal(defy_battery_central_snapshot(data, sizeof(data)), sizeof(data));
    zassert_equal(data[10], BIT(DEFY_LINK_BLE));
}
ZTEST(defy_battery_central, test_usb_snapshot_analog_raw_and_invalid_are_preserved) {
    zassert_ok(defy_battery_central_route_live(DEFY_LINK_WIRED, 0, 1, 0));
    zassert_ok(accept(DEFY_LINK_WIRED, 0, 1, analog_sample(0, 2, 53, 4)));
    zassert_ok(defy_battery_central_route_live(DEFY_LINK_BLE, 1, 1, 1));
    zassert_ok(accept(DEFY_LINK_BLE, 1, 1, analog_sample(1, 3, 0, 2)));
    uint8_t data[DEFY_BATTERY_SNAPSHOT_SIZE];
    zassert_equal(defy_battery_central_snapshot(data, sizeof(data)), sizeof(data));
    zassert_equal(data[11], DEFY_BATTERY_SOURCE_ANALOG);
    zassert_equal(data[20], 100);
    struct defy_battery_frame decoded;
    zassert_ok(defy_battery_frame_decode(data + 72, 32, &decoded));
    zassert_equal(decoded.sample.words[DEFY_BATTERY_ANALOG_THRESHOLD], 0);
    zassert_equal(decoded.sample.words[DEFY_BATTERY_ANALOG_MILLIVOLTS], 55);
    zassert_equal((int32_t)sys_get_le32(data + 64), -ERANGE);
    zassert_equal(data[68], UINT8_MAX);
    zassert_equal(sys_get_le16(data + 70), UINT16_MAX);
}
ZTEST(defy_battery_central, test_usb_snapshot_age_does_not_refresh_duplicates_or_disconnected) {
    connect_sample(DEFY_LINK_WIRED, 0, 1, 0, 80);
    now = 1234;
    zassert_ok(accept(DEFY_LINK_WIRED, 0, 1, sample(0, 10, 1, 80)));
    uint8_t data[DEFY_BATTERY_SNAPSHOT_SIZE];
    zassert_equal(defy_battery_central_snapshot(data, sizeof(data)), sizeof(data));
    zassert_equal(sys_get_le32(data + 12), 1234);
    zassert_equal(data[24 + 12], 2);
    defy_battery_central_disconnect(DEFY_LINK_WIRED, 0, 1);
    now = DEFY_BATTERY_MAX_AGE_MS;
    zassert_equal(defy_battery_central_snapshot(data, sizeof(data)), sizeof(data));
    zassert_equal(data[9], 1);
    zassert_equal(data[10], 0);
    zassert_equal(sys_get_le32(data + 12), now);
    zassert_equal((int32_t)sys_get_le32(data + 16), -ESTALE);
    zassert_equal(data[20], UINT8_MAX);
}
ZTEST(defy_battery_central, test_bas_notifies_only_complete_valid_measurements) {
    battery_notify_work(NULL);
    zassert_equal(notify_calls, 0);
    connect_sample(DEFY_LINK_WIRED, 0, 1, 0, 80);
    connect_sample(DEFY_LINK_BLE, 1, 1, 1, 60);
    battery_notify_work(NULL);
    zassert_equal(notify_calls, 1);
    zassert_equal(notified_level, 60);
    defy_battery_central_disconnect(DEFY_LINK_BLE, 1, 1);
    battery_notify_work(NULL);
    zassert_equal(notify_calls, 1);
}

ZTEST(defy_battery_central, test_filtered_estimate_reaches_bas_without_changing_raw_voltage) {
    for (uint8_t side = 0; side < 2; side++) {
        enum defy_link route = side ? DEFY_LINK_BLE : DEFY_LINK_WIRED;
        struct defy_battery_frame frame = analog_sample(side, 1, 53, 4);
        frame.sample.words[0] = DEFY_BATTERY_ANALOG_FILTERED_TAG;
        frame.sample.words[1] |= (uint16_t)(side ? 50 : 70) << 8;
        frame.sample.words[4] = 0x94;
        frame.sample.words[7] = 0x0300;
        zassert_ok(defy_battery_central_route_live(route, side, 1, side));
        zassert_ok(accept(route, side, 1, frame));
    }
    battery_notify_work(NULL);
    zassert_equal(notify_calls, 1);
    zassert_equal(notified_level, 50);
    zassert_equal(record(0).reading.level, 70);
    zassert_equal(record(1).reading.level, 50);
    uint8_t data[DEFY_BATTERY_SNAPSHOT_SIZE];
    zassert_equal(defy_battery_central_snapshot(data, sizeof(data)), sizeof(data));
    for (uint8_t side = 0; side < 2; side++) {
        uint8_t *entry = data + 8 + 48 * side;
        zassert_equal(entry[12], side ? 50 : 70);
        struct defy_battery_frame decoded;
        zassert_ok(defy_battery_frame_decode(entry + 16, DEFY_BATTERY_FRAME_SIZE, &decoded));
        zassert_equal(decoded.sample.words[1] & 0xffu, 53);
        zassert_equal(decoded.sample.words[3], 4007);
    }
}

static void bas_ready(void) {
    connect_sample(DEFY_LINK_WIRED, 0, 1, 0, 80);
    connect_sample(DEFY_LINK_WIRED, 1, 1, 1, 60);
    run_bas_work();
    zassert_equal(notify_calls, 1);
}

static ssize_t bas_subscribe(uint8_t index, bool enabled) {
    uint8_t value[2] = {enabled ? BT_GATT_CCC_NOTIFY : 0, 0};
    const struct bt_gatt_attr *attr = &defy_battery_bas.attrs[3];
    zassert_equal(attr->write, bt_gatt_attr_write_ccc);
    return attr->write(connection(index), attr, value, sizeof(value), 0, 0);
}

ZTEST(defy_battery_central, test_bas_suppresses_duplicate_routes_and_unchanged_new_samples) {
    bas_ready();
    now = 5000;
    struct defy_battery_frame frame = sample(1, 11, 1, 60);
    frame.sample.age_ms = 5000;
    zassert_ok(accept(DEFY_LINK_WIRED, 1, 1, frame));
    battery_notify_work(NULL);
    zassert_ok(defy_battery_central_route_live(DEFY_LINK_BLE, 1, 9, 1));
    zassert_ok(accept(DEFY_LINK_BLE, 1, 9, frame));
    battery_notify_work(NULL);
    now = 30000;
    zassert_ok(accept(DEFY_LINK_WIRED, 0, 1, sample(0, 10, 2, 79)));
    zassert_ok(accept(DEFY_LINK_WIRED, 1, 1, sample(1, 11, 2, 60)));
    battery_notify_work(NULL);
    zassert_equal(notify_calls, 1);
    uint8_t level = 0;
    zassert_equal(read_level(connection(0), &defy_battery_bas.attrs[1], &level, 1, 0), 1);
    zassert_equal(level, 60);
}

ZTEST(defy_battery_central, test_bas_sends_changed_aggregate_once) {
    bas_ready();
    zassert_ok(accept(DEFY_LINK_WIRED, 1, 1, sample(1, 11, 2, 59)));
    battery_notify_work(NULL);
    battery_notify_work(NULL);
    zassert_equal(notify_calls, 2);
    zassert_equal(notified_level, 59);
}

ZTEST(defy_battery_central, test_bas_notification_errors_retry_per_host) {
    host_live[1] = host_subscribed[1] = true;
    host_notify_result[0] = -ENOMEM;
    connect_sample(DEFY_LINK_WIRED, 0, 1, 0, 80);
    connect_sample(DEFY_LINK_WIRED, 1, 1, 1, 60);
    run_bas_work();
    zassert_equal(host_notify_calls[0], 1);
    zassert_equal(host_notify_calls[1], 1);
    zassert_equal(host_work_due - now, NOTIFY_RETRY_MS);
    host_notify_result[0] = 0;
    run_bas_work();
    zassert_equal(host_notify_calls[0], 2);
    zassert_equal(host_notify_calls[1], 1);
    zassert_equal(host_work_due, INT64_MAX);
}

static void bas_failed_notification(int err) {
    host_notify_result[0] = err;
    connect_sample(DEFY_LINK_WIRED, 0, 1, 0, 80);
    connect_sample(DEFY_LINK_WIRED, 1, 1, 1, 60);
    run_bas_work();
}

static void bas_exhaust_retries(void) {
    bas_failed_notification(-ENOMEM);
    for (unsigned int i = 1; i < NOTIFY_ATTEMPTS; i++) {
        zassert_equal(host_work_due - now, NOTIFY_RETRY_MS);
        run_bas_work();
    }
    zassert_equal(host_notify_calls[0], NOTIFY_ATTEMPTS);
    zassert_equal(host_work_due, INT64_MAX);
}

ZTEST(defy_battery_central, test_bas_duplicate_reports_preserve_retry_deadline) {
    bas_failed_notification(-ENOMEM);
    for (now = 50; now < NOTIFY_RETRY_MS; now += 50) {
        zassert_ok(accept(DEFY_LINK_WIRED, 1, 1, sample(1, 11, 1, 60)));
        run_bas_work();
        zassert_equal(host_notify_calls[0], 1);
        zassert_equal(host_work_due, NOTIFY_RETRY_MS);
    }
    host_notify_result[0] = 0;
    run_bas_work();
    zassert_equal(host_notify_calls[0], 2);
    zassert_equal(host_work_due, INT64_MAX);
}

ZTEST(defy_battery_central, test_bas_fresh_same_level_sample_can_retry_immediately) {
    bas_failed_notification(-ENOMEM);
    now = 100;
    host_notify_result[0] = 0;
    zassert_ok(accept(DEFY_LINK_WIRED, 1, 1, sample(1, 11, 2, 60)));
    run_bas_work();
    zassert_equal(now, 100);
    zassert_equal(host_notify_calls[0], 2);
    zassert_equal(host_work_due, INT64_MAX);
}

ZTEST(defy_battery_central, test_bas_retry_budget_ignores_duplicates_and_new_sample_renews_it) {
    bas_exhaust_retries();
    zassert_ok(accept(DEFY_LINK_WIRED, 1, 1, sample(1, 11, 1, 60)));
    run_bas_work();
    zassert_equal(host_notify_calls[0], NOTIFY_ATTEMPTS);
    zassert_equal(host_work_due, INT64_MAX);
    host_notify_result[0] = 0;
    zassert_ok(accept(DEFY_LINK_WIRED, 1, 1, sample(1, 11, 2, 60)));
    run_bas_work();
    zassert_equal(host_notify_calls[0], NOTIFY_ATTEMPTS + 1);
    zassert_equal(notified_level, 60);
    zassert_equal(host_work_due, INT64_MAX);
}

ZTEST(defy_battery_central, test_bas_transient_errors_are_retried_without_new_samples) {
    bas_failed_notification(-EAGAIN);
    host_notify_result[0] = -ENOBUFS;
    run_bas_work();
    host_notify_result[0] = 0;
    run_bas_work();
    zassert_equal(host_notify_calls[0], 3);
    zassert_equal(notified_level, 60);
    zassert_equal(host_work_due, INT64_MAX);
}

ZTEST(defy_battery_central, test_bas_permanent_notification_error_has_no_delayed_retry) {
    bas_failed_notification(-EINVAL);
    zassert_equal(host_notify_calls[0], 1);
    zassert_equal(host_work_due, INT64_MAX);
}

ZTEST(defy_battery_central, test_bas_retry_uses_changed_measurement) {
    bas_failed_notification(-ENOMEM);
    host_notify_result[0] = 0;
    zassert_ok(accept(DEFY_LINK_WIRED, 1, 1, sample(1, 11, 2, 40)));
    zassert_equal(host_work_due, now);
    run_bas_work();
    zassert_equal(host_notify_calls[0], 2);
    zassert_equal(notified_level, 40);
    zassert_equal(host_work_due, INT64_MAX);
}

ZTEST(defy_battery_central, test_bas_retry_stops_after_host_disconnect) {
    bas_failed_notification(-ENOMEM);
    host_live[0] = false;
    host_disconnected(connection(0), 0);
    run_bas_work();
    zassert_equal(host_notify_calls[0], 1);
    zassert_equal(host_work_due, INT64_MAX);
}

static void bas_change_during_notify(uint8_t index) {
    ARG_UNUSED(index);
    notify_hook = NULL;
    zassert_ok(accept(DEFY_LINK_WIRED, 1, 1, sample(1, 11, 2, 40)));
}

ZTEST(defy_battery_central, test_bas_change_during_failure_keeps_immediate_work) {
    notify_hook = bas_change_during_notify;
    bas_failed_notification(-ENOMEM);
    zassert_equal(host_work_due, now);
    host_notify_result[0] = 0;
    run_bas_work();
    zassert_equal(notified_level, 40);
    zassert_equal(host_notify_calls[0], 2);
    zassert_equal(host_work_due, INT64_MAX);
}

ZTEST(defy_battery_central, test_bas_first_subscription_gets_current_level) {
    host_subscribed[0] = false;
    level_ccc.value = 0;
    connect_sample(DEFY_LINK_WIRED, 0, 1, 0, 80);
    connect_sample(DEFY_LINK_WIRED, 1, 1, 1, 60);
    battery_notify_work(NULL);
    zassert_equal(notify_calls, 0);
    zassert_equal(bas_subscribe(0, true), 2);
    zassert_equal(host_work_due, now);
    run_bas_work();
    zassert_equal(notify_calls, 1);
    zassert_equal(host_work_due, INT64_MAX);
}

ZTEST(defy_battery_central, test_bas_new_subscriber_receives_cache_while_both_halves_sleep) {
    bas_ready();
    defy_battery_central_disconnect(DEFY_LINK_WIRED, 0, 1);
    defy_battery_central_disconnect(DEFY_LINK_WIRED, 1, 1);
    now = DEFY_BATTERY_MAX_AGE_MS + 1000;
    host_live[1] = true;
    host_connected(connection(1), 0);
    zassert_equal(bas_subscribe(1, true), 2);
    run_bas_work();
    zassert_equal(host_notify_calls[0], 1);
    zassert_equal(host_notify_calls[1], 1);
    zassert_equal(notified_level, 60);
    zassert_equal(host_work_due, INT64_MAX);
    uint8_t level;
    zassert_equal(read_level(connection(1), &defy_battery_bas.attrs[1], &level, 1, 0), 1);
    zassert_equal(level, 60);
    zassert_false(record(0).connected || record(1).connected);
    zassert_false(record(0).fresh || record(1).fresh);
}

ZTEST(defy_battery_central, test_bas_nonsubscribed_connection_does_not_poll) {
    bas_ready();
    host_live[1] = true;
    host_connected(connection(1), 0);
    run_bas_work();
    zassert_equal(host_notify_calls[1], 0);
    zassert_equal(host_work_due, INT64_MAX);
}

ZTEST(defy_battery_central, test_bas_same_subscription_preserves_dedup) {
    bas_ready();
    zassert_equal(bas_subscribe(0, true), 2);
    battery_notify_work(NULL);
    zassert_equal(notify_calls, 1);
}

ZTEST(defy_battery_central, test_bas_unsubscribe_stops_sends_and_resubscribe_refreshes) {
    bas_ready();
    zassert_ok(accept(DEFY_LINK_WIRED, 1, 1, sample(1, 11, 2, 59)));
    zassert_equal(bas_subscribe(0, false), 2);
    battery_notify_work(NULL);
    zassert_equal(notify_calls, 1);
    zassert_equal(bas_subscribe(0, true), 2);
    battery_notify_work(NULL);
    battery_notify_work(NULL);
    zassert_equal(notify_calls, 2);
    zassert_equal(notified_level, 59);
}

ZTEST(defy_battery_central, test_bas_reconnect_restored_subscription_gets_initial_value) {
    bas_ready();
    host_live[0] = false;
    host_disconnected(connection(0), 0);
    battery_notify_work(NULL);
    host_live[0] = true;
    host_connected(connection(0), 0);
    battery_notify_work(NULL);
    battery_notify_work(NULL);
    zassert_equal(notify_calls, 2);
}

static struct defy_battery_frame analog_sample(uint8_t side, uint32_t sequence,
                                                uint16_t threshold, uint16_t reference) {
    struct defy_battery_frame frame = {
        .side = side, .session = 10 + side,
        .sample = {.sequence = sequence, .status = DEFY_BATTERY_SAMPLE_ANALOG_OK,
                   .words = {DEFY_BATTERY_ANALOG_CHARGER_TAG, threshold, reference, 0, 0x0094, 0,
                             0, 0x0100}},
    };
    zassert_ok(defy_battery_analog_mv(threshold, reference, &frame.sample.words[3]));
    return frame;
}

#define CHARGER_READY 0x00
#define CHARGER_CHARGING 0x10
#define CHARGER_DONE 0x20
#define CHARGER_FAULT 0x34

static struct defy_battery_frame filtered_sample(uint8_t side, uint32_t sequence, uint8_t level,
                                                 uint8_t charger_status) {
    struct defy_battery_frame frame = analog_sample(side, sequence, 53, 4);
    frame.sample.words[0] = DEFY_BATTERY_ANALOG_FILTERED_TAG;
    frame.sample.words[1] |= (uint16_t)level << 8;
    frame.sample.words[4] = 0x94 | (uint16_t)charger_status << 8;
    frame.sample.words[7] = 0x0300;
    return frame;
}

static int host_level(void) {
    uint8_t level = 123;
    int err = defy_battery_central_aggregate(&level, NULL);
    return err ? err : level;
}

static void connect_both(void) {
    zassert_ok(defy_battery_central_route_live(DEFY_LINK_WIRED, 0, 1, 0));
    zassert_ok(defy_battery_central_route_live(DEFY_LINK_BLE, 1, 1, 1));
}

ZTEST(defy_battery_central, test_charging_half_is_left_out_beside_fresh_other_half) {
    connect_both();
    zassert_ok(accept(DEFY_LINK_WIRED, 0, 1, filtered_sample(0, 1, 15, CHARGER_READY)));
    zassert_ok(accept(DEFY_LINK_BLE, 1, 1, filtered_sample(1, 1, 70, CHARGER_READY)));
    zassert_equal(host_level(), 15);
    battery_notify_work(NULL);
    zassert_equal(notified_level, 15);

    zassert_ok(accept(DEFY_LINK_WIRED, 0, 1,
                      filtered_sample(0, 2, DEFY_BATTERY_LEVEL_UNAVAILABLE, CHARGER_CHARGING)));
    zassert_equal(host_level(), 70);
    battery_notify_work(NULL);
    zassert_equal(notify_calls, 2);
    zassert_equal(notified_level, 70);
    uint8_t level = 0;
    zassert_equal(read_level(connection(0), NULL, &level, 1, 0), 1);
    zassert_equal(level, 70);
    zassert_ok(accept(DEFY_LINK_BLE, 1, 1, filtered_sample(1, 2, 50, CHARGER_READY)));
    battery_notify_work(NULL);
    zassert_equal(notified_level, 50);

    zassert_ok(accept(DEFY_LINK_WIRED, 0, 1,
                      filtered_sample(0, 3, DEFY_BATTERY_LEVEL_UNAVAILABLE, CHARGER_DONE)));
    zassert_equal(host_level(), 50);
    zassert_ok(accept(DEFY_LINK_WIRED, 0, 1,
                      filtered_sample(0, 4, DEFY_BATTERY_LEVEL_UNAVAILABLE, CHARGER_READY)));
    zassert_equal(host_level(), 50);
    zassert_ok(accept(DEFY_LINK_BLE, 1, 1,
                      filtered_sample(1, 3, DEFY_BATTERY_LEVEL_UNAVAILABLE, CHARGER_CHARGING)));
    zassert_equal(host_level(), 100);
    battery_notify_work(NULL);
    zassert_equal(notified_level, 100);
}

ZTEST(defy_battery_central, test_unplugged_half_waits_for_its_new_level) {
    connect_both();
    zassert_ok(accept(DEFY_LINK_WIRED, 0, 1, filtered_sample(0, 1, 15, CHARGER_READY)));
    zassert_ok(accept(DEFY_LINK_BLE, 1, 1, filtered_sample(1, 1, 70, CHARGER_READY)));
    zassert_ok(accept(DEFY_LINK_WIRED, 0, 1, filtered_sample(0, 2, 15, CHARGER_FAULT)));
    zassert_equal(host_level(), 15);
    uint32_t revision, original;
    uint8_t level;
    zassert_ok(defy_battery_central_aggregate(&level, &original));
    zassert_ok(accept(DEFY_LINK_WIRED, 0, 1,
                      filtered_sample(0, 3, DEFY_BATTERY_LEVEL_UNAVAILABLE, CHARGER_CHARGING)));
    zassert_ok(defy_battery_central_aggregate(&level, &revision));
    zassert_equal(level, 70);
    zassert_equal(revision, original + 1);
    zassert_ok(accept(DEFY_LINK_WIRED, 0, 1,
                      filtered_sample(0, 4, DEFY_BATTERY_LEVEL_UNAVAILABLE, CHARGER_CHARGING)));
    zassert_ok(defy_battery_central_aggregate(&level, &revision));
    zassert_equal(revision, original + 1);

    zassert_ok(accept(DEFY_LINK_WIRED, 0, 1,
                      filtered_sample(0, 5, DEFY_BATTERY_LEVEL_UNAVAILABLE, CHARGER_READY)));
    zassert_equal(host_level(), 70);
    zassert_ok(accept(DEFY_LINK_WIRED, 0, 1,
                      filtered_sample(0, 6, DEFY_BATTERY_LEVEL_UNAVAILABLE, CHARGER_FAULT)));
    zassert_equal(host_level(), 70);
    zassert_ok(accept(DEFY_LINK_WIRED, 0, 1, filtered_sample(0, 7, 40, CHARGER_READY)));
    zassert_equal(host_level(), 40);
}

ZTEST(defy_battery_central, test_neuron_restart_while_half_is_on_external_power) {
    connect_both();
    zassert_ok(accept(DEFY_LINK_WIRED, 0, 1,
                      filtered_sample(0, 7, DEFY_BATTERY_LEVEL_UNAVAILABLE, CHARGER_CHARGING)));
    zassert_ok(accept(DEFY_LINK_BLE, 1, 1,
                      filtered_sample(1, 9, DEFY_BATTERY_LEVEL_UNAVAILABLE, CHARGER_READY)));
    zassert_equal(host_level(), -ENODATA);
    uint8_t level = 123;
    zassert_equal(read_level(connection(0), NULL, &level, 1, 0),
                  BT_GATT_ERR(BT_ATT_ERR_UNLIKELY));
    battery_notify_work(NULL);
    zassert_equal(notify_calls, 0);

    zassert_ok(accept(DEFY_LINK_BLE, 1, 1, filtered_sample(1, 10, 70, CHARGER_READY)));
    zassert_equal(read_level(connection(0), NULL, &level, 1, 0), 1);
    zassert_equal(level, 70);
    battery_notify_work(NULL);
    zassert_equal(notify_calls, 1);
    zassert_equal(notified_level, 70);
    zassert_ok(accept(DEFY_LINK_BLE, 1, 1, filtered_sample(1, 11, 50, CHARGER_READY)));
    battery_notify_work(NULL);
    zassert_equal(notified_level, 50);
    zassert_ok(accept(DEFY_LINK_WIRED, 0, 1,
                      filtered_sample(0, 8, DEFY_BATTERY_LEVEL_UNAVAILABLE, CHARGER_DONE)));
    zassert_equal(host_level(), 50);
}

ZTEST(defy_battery_central, test_both_halves_charging_report_lowest_earlier_level) {
    connect_both();
    zassert_ok(accept(DEFY_LINK_WIRED, 0, 1, filtered_sample(0, 1, 15, CHARGER_READY)));
    zassert_ok(accept(DEFY_LINK_BLE, 1, 1, filtered_sample(1, 1, 40, CHARGER_READY)));
    struct defy_battery_frame raw = analog_sample(0, 2, 55, 4);
    raw.sample.words[0] = DEFY_BATTERY_ANALOG_CHARGER_TAG;
    raw.sample.words[4] = 0x94 | (uint16_t)CHARGER_CHARGING << 8;
    raw.sample.words[7] = 0x0300;
    zassert_ok(accept(DEFY_LINK_WIRED, 0, 1, raw));
    zassert_equal(record(0).reading.level, 100);
    zassert_equal(host_level(), 40);
    zassert_ok(accept(DEFY_LINK_BLE, 1, 1,
                      filtered_sample(1, 2, DEFY_BATTERY_LEVEL_UNAVAILABLE, CHARGER_CHARGING)));
    zassert_equal(host_level(), 15);
    zassert_ok(accept(DEFY_LINK_BLE, 1, 1,
                      filtered_sample(1, 3, DEFY_BATTERY_LEVEL_UNAVAILABLE, CHARGER_DONE)));
    zassert_equal(host_level(), 100);

    reset(NULL);
    connect_both();
    zassert_ok(accept(DEFY_LINK_WIRED, 0, 1,
                      filtered_sample(0, 1, DEFY_BATTERY_LEVEL_UNAVAILABLE, CHARGER_CHARGING)));
    zassert_ok(accept(DEFY_LINK_BLE, 1, 1,
                      filtered_sample(1, 1, DEFY_BATTERY_LEVEL_UNAVAILABLE, CHARGER_CHARGING)));
    zassert_equal(host_level(), -ENODATA);
    battery_notify_work(NULL);
    zassert_equal(notify_calls, 0);
    zassert_ok(accept(DEFY_LINK_BLE, 1, 1,
                      filtered_sample(1, 2, DEFY_BATTERY_LEVEL_UNAVAILABLE, CHARGER_DONE)));
    zassert_equal(host_level(), 100);
}

static void poll(void) { control_poll_work(NULL); }
static void discovered(struct bt_conn *conn, uint16_t handle) {
    struct bt_gatt_chrc characteristic = {.uuid = &battery_uuid.uuid, .value_handle = handle};
    struct bt_gatt_attr attr = {.user_data = &characteristic};
    zassert_equal(last_discovery->func(conn, &attr, last_discovery), BT_GATT_ITER_CONTINUE);
    characteristic.uuid = &command_uuid.uuid;
    characteristic.value_handle = handle + 19;
    zassert_equal(last_discovery->func(conn, &attr, last_discovery), BT_GATT_ITER_STOP);
}
static void finish_preflight(uint8_t index) {
    uint8_t data[DEFY_LIGHTING_CONTROL_SIZE];
    zassert_equal(read_done(last_read), wake_read_done);
    zassert_ok(defy_lighting_control_encode(&desired_lighting, DEFY_CONTROL_LIGHTING_ACK, data));
    zassert_equal(last_read->func(connection(index), 0, last_read, data, sizeof(data)),
                  BT_GATT_ITER_CONTINUE);
    zassert_equal(last_read->func(connection(index), 0, last_read, NULL, 0), BT_GATT_ITER_STOP);
    poll();
}
static void start_read(uint8_t index, uint8_t slot, uint32_t epoch) {
    defy_control_ble_connected(connection(index), slot, epoch);
    now += 1000;
    poll();
    zassert_not_null(last_discovery);
    discovered(connection(index), 42);
    now++;
    poll();
    zassert_not_null(last_read);
    finish_preflight(index);
    zassert_equal(last_read->single.handle, 42);
}
static void deliver(struct bt_conn *conn, struct defy_battery_frame frame) {
    uint8_t data[DEFY_BATTERY_FRAME_SIZE];
    zassert_ok(defy_battery_frame_encode(&frame, data));
    zassert_equal(last_read->func(conn, 0, last_read, data, 22), BT_GATT_ITER_CONTINUE);
    zassert_equal(last_read->func(conn, 0, last_read, data + 22, 10), BT_GATT_ITER_CONTINUE);
    zassert_equal(last_read->func(conn, 0, last_read, NULL, 0), BT_GATT_ITER_STOP);
    poll();
}
ZTEST(defy_battery_central, test_ble_discovery_then_fragmented_read) {
    start_read(0, 1, 5);
    zassert_equal(scheduled_queue, &control_work_queue);
    zassert_not_equal(scheduled_queue, &k_sys_work_q);
    deliver(connection(0), sample(0, 10, 1, 80));
    zassert_true(record(0).valid);
    zassert_true(record(0).connected);
    zassert_equal(refs[0], 1);
    now += BATTERY_POLL_MS;
    poll();
    zassert_equal(discovery_calls, 1);
    zassert_equal(read_calls, 3);
}
ZTEST(defy_battery_central, test_ble_read_latency_counts_toward_age) {
    start_read(0, 1, 5);
    now += 1200;
    deliver(connection(0), sample(0, 10, 1, 80));
    zassert_equal(record(0).frame.sample.age_ms, 1200);
}
ZTEST(defy_battery_central, test_ble_missing_characteristic_is_retried) {
    defy_control_ble_connected(connection(0), 0, 1);
    now = 1000;
    poll();
    zassert_equal(last_discovery->func(connection(0), NULL, last_discovery), BT_GATT_ITER_STOP);
    now += BATTERY_POLL_MS;
    poll();
    zassert_equal(discovery_calls, 2);
    zassert_equal(read_calls, 0);
}
ZTEST(defy_battery_central, test_ble_disconnect_during_request_start_cancels_before_unref) {
    defy_control_ble_connected(connection(0), 0, 1);
    now = 1000;
    poll();
    discovered(connection(0), 42);
    disconnect_during_read_start = true;
    now++;
    poll();
    zassert_equal(refs[0], 1);
    poll();
    zassert_equal(cancel_calls, 1);
    zassert_equal(refs[0], 1);
    poll();
    zassert_equal(refs[0], 0);
}
ZTEST(defy_battery_central, test_ble_short_and_oversized_reads_do_not_publish) {
    start_read(0, 0, 1);
    uint8_t data[33] = {0};
    last_read->func(connection(0), 0, last_read, data, 10);
    last_read->func(connection(0), 0, last_read, NULL, 0);
    poll();
    zassert_false(record(0).present);
    now += BATTERY_POLL_MS;
    poll();
    zassert_equal(last_read->func(connection(0), 0, last_read, data, sizeof(data)),
                  BT_GATT_ITER_STOP);
    poll();
    zassert_false(record(0).present);
}
ZTEST(defy_battery_central, test_ble_request_failure_retries_without_changing_epoch) {
    discovery_result = -ENOMEM;
    defy_control_ble_connected(connection(0), 0, 1);
    now = 1000;
    poll();
    zassert_equal(discovery_calls, 1);
    discovery_result = 0;
    now += RETRY_MS;
    poll();
    zassert_equal(discovery_calls, 2);
    discovered(connection(0), 42);
    read_result = -ENOMEM;
    now++;
    poll();
    zassert_equal(read_calls, 1);
    read_result = 0;
    now += RETRY_MS;
    poll();
    finish_preflight(0);
    deliver(connection(0), sample(0, 10, 1, 80));
    zassert_true(record(0).connected);
}
ZTEST(defy_battery_central, test_ble_disconnect_cancels_inflight_and_releases_reference) {
    start_read(0, 0, 1);
    defy_control_ble_disconnected(connection(0), 0, 1);
    poll();
    zassert_equal(cancel_calls, 1);
    poll();
    zassert_equal(refs[0], 0);
    zassert_false(record(0).present);
}
ZTEST(defy_battery_central, test_ble_ready_read_cannot_revive_disconnected_route) {
    start_read(0, 0, 1);
    uint8_t data[DEFY_BATTERY_FRAME_SIZE];
    struct defy_battery_frame frame = sample(0, 10, 1, 80);
    zassert_ok(defy_battery_frame_encode(&frame, data));
    last_read->func(connection(0), 0, last_read, data, sizeof(data));
    last_read->func(connection(0), 0, last_read, NULL, 0);
    defy_control_ble_disconnected(connection(0), 0, 1);
    poll();
    zassert_equal(refs[0], 0);
    zassert_false(record(0).present);
}

ZTEST(defy_battery_central, test_show_selects_physical_side_and_actual_discovered_handle) {
    start_read(0, 1, 5);
    deliver(connection(0), sample(0, 10, 1, 80));
    defy_control_ble_battery_show(BIT(0));
    zassert_equal(write_calls, 0);
    poll();
    zassert_equal(last_submitted, &battery_show_send);
    zassert_equal(submitted_queue, &control_work_queue);
    zassert_equal(write_calls, 0);
    battery_show_work(NULL);
    zassert_equal(write_calls, 1);
    zassert_equal(written_peer, 0);
    zassert_equal(written_handle, 61);
    zassert_equal(refs[0], 1);
    battery_show_work(NULL);
    zassert_equal(write_calls, 1);
}
ZTEST(defy_battery_central, test_show_needs_decoded_side_but_not_valid_gauge) {
    start_read(0, 1, 5);
    defy_control_ble_battery_show(BIT(0) | BIT(1));
    poll();
    battery_show_work(NULL);
    zassert_equal(write_calls, 0);
    struct defy_battery_frame frame = sample(1, 10, 1, 80);
    frame.sample.status = DEFY_BATTERY_SAMPLE_IO_ERROR;
    deliver(connection(0), frame);
    defy_control_ble_battery_show(BIT(0));
    poll();
    battery_show_work(NULL);
    zassert_equal(write_calls, 0);
    defy_control_ble_battery_show(BIT(1));
    poll();
    battery_show_work(NULL);
    zassert_equal(write_calls, 1);
}
ZTEST(defy_battery_central, test_show_expires_before_and_after_it_is_queued) {
    start_read(0, 1, 5);
    deliver(connection(0), sample(0, 10, 1, 80));
    defy_control_ble_battery_show(BIT(0));
    now += DEFY_CONTROL_TIMEOUT_MS;
    poll();
    battery_show_work(NULL);
    zassert_equal(write_calls, 0);
    defy_control_ble_battery_show(BIT(0));
    poll();
    now += DEFY_CONTROL_TIMEOUT_MS;
    battery_show_work(NULL);
    zassert_equal(write_calls, 0);
}
ZTEST(defy_battery_central, test_show_disconnect_and_reconnect_drop_pending_action) {
    start_read(0, 1, 5);
    deliver(connection(0), sample(0, 10, 1, 80));
    defy_control_ble_battery_show(BIT(0));
    poll();
    defy_control_ble_disconnected(connection(0), 1, 5);
    defy_control_ble_connected(connection(1), 1, 6);
    battery_show_work(NULL);
    zassert_equal(write_calls, 0);
    poll();
    zassert_equal(refs[0], 0);
}
ZTEST(defy_battery_central, test_show_later_key_identity_conflict_drops_pending_action) {
    start_read(0, 1, 5);
    deliver(connection(0), sample(0, 10, 1, 80));
    defy_control_ble_battery_show(BIT(0));
    poll();
    zassert_equal(defy_battery_central_route_identify(DEFY_LINK_BLE, 1, 5, 1), -EPROTO);
    battery_show_work(NULL);
    zassert_equal(write_calls, 0);
    uint8_t side;
    zassert_equal(defy_battery_central_route_side(DEFY_LINK_BLE, 1, 5, &side), -ENODATA);
}
ZTEST(defy_battery_central, test_show_buffer_exhaustion_does_not_retry_later) {
    start_read(0, 1, 5);
    deliver(connection(0), sample(0, 10, 1, 80));
    write_result = -ENOMEM;
    defy_control_ble_battery_show(BIT(0));
    poll();
    battery_show_work(NULL);
    zassert_equal(write_calls, 1);
    write_result = 0;
    now += 2000;
    poll();
    battery_show_work(NULL);
    zassert_equal(write_calls, 1);
}

static struct control_peer *lighting_peer(uint8_t index) {
    for (size_t i = 0; i < ARRAY_SIZE(control_peers); i++) {
        if (control_peers[i].conn == connection(index)) {
            return &control_peers[i];
        }
    }
    zassert_unreachable();
    return NULL;
}

static struct control_peer *lighting_ready(uint8_t index, uint8_t slot, uint32_t epoch) {
    start_read(index, slot, epoch);
    last_read->func(connection(index), BT_ATT_ERR_UNLIKELY, last_read, NULL, 0);
    poll();
    return lighting_peer(index);
}

static struct defy_host_feedback feedback_request(void) {
    return (struct defy_host_feedback){.boot = 5, .sequence = 8, .remaining_ms = 2000,
                                       .profile = 2, .flags = DEFY_HOST_PREFERRED_BLE |
                                           DEFY_HOST_SELECTED_USB | DEFY_HOST_USB_READY};
}

ZTEST(defy_battery_central, test_feedback_direct_write_uses_current_ready_links_without_poll) {
    lighting_ready(0, 1, 5);
    lighting_ready(1, 0, 6);
    int reads_before = read_calls;
    int discovers_before = discovery_calls;
    scheduled_queue = NULL;
    struct defy_host_feedback state = feedback_request();
    defy_control_ble_host_feedback_show(&state);
    zassert_equal(last_submitted, &feedback_send);
    zassert_equal(submitted_queue, &control_work_queue);
    zassert_is_null(scheduled_queue);
    zassert_equal(write_calls, 0);
    now += 500;
    feedback_send_work(NULL);
    zassert_equal(write_calls, 2);
    zassert_equal(written_feedback.remaining_ms, 1500);
    zassert_equal(written_feedback.profile, 2);
    zassert_equal(written_feedback.flags, state.flags);
    zassert_equal(read_calls, reads_before);
    zassert_equal(discovery_calls, discovers_before);
    zassert_equal(refs[0], 1);
    zassert_equal(refs[1], 1);
}

ZTEST(defy_battery_central, test_feedback_requires_encryption_and_discovered_control) {
    defy_control_ble_connected(connection(1), 1, 6);
    struct defy_host_feedback state = feedback_request();
    defy_control_ble_host_feedback_show(&state);
    feedback_send_work(NULL);
    zassert_equal(write_calls, 0);
    zassert_equal(discovery_calls + read_calls, 0);
    defy_control_ble_disconnected(connection(1), 1, 6);
    poll();
    lighting_ready(0, 0, 5);
    security_level = BT_SECURITY_L1;
    defy_control_ble_host_feedback_show(&state);
    feedback_send_work(NULL);
    zassert_equal(write_calls, 0);
    security_level = BT_SECURITY_L2;
    feedback_send_work(NULL);
    zassert_equal(write_calls, 0);
    defy_control_ble_host_feedback_show(&state);
    feedback_send_work(NULL);
    zassert_equal(write_calls, 1);
}

ZTEST(defy_battery_central, test_feedback_expired_or_disconnected_does_not_replay) {
    lighting_ready(0, 0, 5);
    struct defy_host_feedback state = feedback_request();
    defy_control_ble_host_feedback_show(&state);
    now += 2000;
    feedback_send_work(NULL);
    zassert_equal(write_calls, 0);
    defy_control_ble_host_feedback_show(&state);
    defy_control_ble_disconnected(connection(0), 0, 5);
    lighting_ready(1, 0, 6);
    feedback_send_work(NULL);
    zassert_equal(write_calls, 0);
}

ZTEST(defy_battery_central, test_feedback_maintenance_and_write_failure_drop_pending_request) {
    lighting_ready(0, 0, 5);
    struct defy_host_feedback state = feedback_request();
    defy_control_ble_host_feedback_show(&state);
    maintenance_active_test = true;
    feedback_send_work(NULL);
    defy_control_ble_host_feedback_show(&state);
    maintenance_active_test = false;
    feedback_send_work(NULL);
    zassert_equal(write_calls, 0);
    write_result = -ENOMEM;
    defy_control_ble_host_feedback_show(&state);
    feedback_send_work(NULL);
    feedback_send_work(NULL);
    zassert_equal(write_calls, 1);
}

static void lighting_ack(uint8_t index, const struct defy_lighting_state *state) {
    struct control_peer *peer = lighting_peer(index);
    uint8_t data[DEFY_LIGHTING_CONTROL_SIZE];
    zassert_equal(peer->read_done, ack_read_done);
    zassert_equal(peer->read.single.handle, 61);
    zassert_ok(defy_lighting_control_encode(state, DEFY_CONTROL_LIGHTING_ACK, data));
    zassert_equal(peer->read.func(connection(index), 0, &peer->read, data, sizeof(data)),
                  BT_GATT_ITER_CONTINUE);
    zassert_equal(peer->read.func(connection(index), 0, &peer->read, NULL, 0),
                  BT_GATT_ITER_STOP);
}

static struct control_peer *start_preflight(uint8_t index, uint8_t slot, uint32_t epoch) {
    defy_control_ble_connected(connection(index), slot, epoch);
    now += 1000;
    poll();
    discovered(connection(index), 42);
    now++;
    poll();
    struct control_peer *peer = lighting_peer(index);
    zassert_not_null(peer);
    zassert_equal(peer->read_done, wake_read_done);
    zassert_equal(peer->read.single.handle, 61);
    zassert_false(peer->lighting_preflight_done);
    return peer;
}

static void deliver_wake(uint8_t index, uint8_t side, uint32_t age) {
    struct control_peer *peer = lighting_peer(index);
    struct defy_lighting_wake wake = {
        .side = side, .from_sleep = true, .boot = 19 + index, .age_ms = age,
    };
    uint8_t data[DEFY_LIGHTING_WAKE_SIZE];
    zassert_ok(defy_lighting_wake_encode(&wake, data));
    zassert_equal(peer->read.func(connection(index), 0, &peer->read, data, 5),
                  BT_GATT_ITER_CONTINUE);
    zassert_equal(peer->read.func(connection(index), 0, &peer->read, data + 5,
                                 sizeof(data) - 5), BT_GATT_ITER_CONTINUE);
    zassert_equal(peer->read.func(connection(index), 0, &peer->read, NULL, 0),
                  BT_GATT_ITER_STOP);
}

static void wake_turns_lights_on(void) {
    struct control_peer *peer = lighting_peer(0);
    zassert_false(peer->lighting_preflight_done);
    peer->lighting_queued = true;
    lighting_send_work(NULL);
    zassert_equal(write_calls, 0);
    desired_lighting.enabled = true;
    desired_lighting.revision++;
    defy_control_ble_lighting_changed();
}

ZTEST(defy_battery_central, test_preflight_wake_precedes_queued_off_and_refreshes_desired) {
    lighting_available = true;
    desired_lighting.enabled = false;
    struct control_peer *peer = start_preflight(0, 1, 8);
    uint8_t side;
    zassert_equal(defy_battery_central_route_side(DEFY_LINK_BLE, 1, 8, &side), -ENODATA);
    peer->lighting_queued = true;
    lighting_send_work(NULL);
    zassert_equal(write_calls, 0);
    poll();
    zassert_true(scheduled_delay >= RETRY_MS);
    now += 120;
    deliver_wake(0, 0, 300);
    zassert_equal(wake_calls, 0);
    peer->lighting_queued = true;
    lighting_send_work(NULL);
    zassert_equal(write_calls, 0);
    wake_hook = wake_turns_lights_on;
    now += 80;
    poll();
    zassert_equal(wake_calls, 1);
    zassert_equal(observed_wake.side, 0);
    zassert_equal(observed_wake.age_ms, 500);
    zassert_true(peer->lighting_preflight_done);
    lighting_send_work(NULL);
    zassert_equal(write_calls, 1);
    zassert_true(written_lighting.enabled);
    zassert_equal(written_lighting.revision, 2);
}

ZTEST(defy_battery_central, test_preflight_old_half_unlikely_falls_back_after_three_attempts) {
    lighting_available = true;
    desired_lighting.enabled = false;
    struct control_peer *peer = start_preflight(0, 0, 8);
    int64_t started = now;
    for (int attempt = 0; attempt < 3; attempt++) {
        zassert_equal(read_calls, attempt + 1);
        peer->read.func(connection(0), BT_ATT_ERR_UNLIKELY, &peer->read, NULL, 0);
        if (attempt < 2) {
            poll();
            lighting_send_work(NULL);
            zassert_equal(write_calls, 0);
            now += 499;
            poll();
            zassert_equal(read_calls, attempt + 1);
            now++;
            poll();
        }
    }
    zassert_equal(now - started, 1000);
    poll();
    zassert_true(peer->lighting_preflight_done);
    lighting_send_work(NULL);
    zassert_equal(write_calls, 1);
    zassert_false(written_lighting.enabled);
    zassert_equal(wake_calls, 0);
}

ZTEST(defy_battery_central, test_preflight_security_errors_retry_before_wake) {
    lighting_available = true;
    desired_lighting.enabled = false;
    struct control_peer *peer = start_preflight(0, 0, 8);
    /* The third error retried, BT_ATT_ERR_UNLIKELY, is the old half's test above. */
    const uint8_t errors[] = {BT_ATT_ERR_AUTHENTICATION, BT_ATT_ERR_INSUFFICIENT_ENCRYPTION};
    for (size_t i = 0; i < ARRAY_SIZE(errors); i++) {
        peer->read.func(connection(0), errors[i], &peer->read, NULL, 0);
        poll();
        lighting_send_work(NULL);
        zassert_equal(write_calls, 0);
        now += 500;
        poll();
    }
    wake_hook = wake_turns_lights_on;
    deliver_wake(0, 1, 2200);
    poll();
    lighting_send_work(NULL);
    zassert_equal(read_calls, 4);
    zassert_equal(wake_calls, 1);
    zassert_true(written_lighting.enabled);
}

ZTEST(defy_battery_central, test_preflight_malformed_reply_falls_back_without_wake) {
    lighting_available = true;
    struct control_peer *peer = start_preflight(0, 0, 8);
    uint8_t data[DEFY_LIGHTING_WAKE_SIZE] = {1, DEFY_CONTROL_LIGHTING_WAKE, 2};
    peer->read.func(connection(0), 0, &peer->read, data, sizeof(data));
    peer->read.func(connection(0), 0, &peer->read, NULL, 0);
    poll();
    lighting_send_work(NULL);
    zassert_equal(wake_calls, 0);
    zassert_equal(write_calls, 1);
}

ZTEST(defy_battery_central, test_preflight_request_failures_are_bounded) {
    lighting_available = true;
    read_result = -ENOMEM;
    struct control_peer *peer = start_preflight(0, 0, 8);
    for (int attempt = 1; attempt < 3; attempt++) {
        now += 500;
        poll();
    }
    zassert_equal(read_calls, 3);
    zassert_true(peer->lighting_preflight_done);
    poll();
    lighting_send_work(NULL);
    zassert_equal(write_calls, 1);
    zassert_equal(wake_calls, 0);
}

ZTEST(defy_battery_central, test_preflight_disconnect_discards_completed_wake) {
    lighting_available = true;
    start_preflight(0, 0, 8);
    deliver_wake(0, 0, 10);
    defy_control_ble_disconnected(connection(0), 0, 8);
    poll();
    lighting_send_work(NULL);
    zassert_equal(wake_calls, 0);
    zassert_equal(write_calls, 0);
    zassert_equal(refs[0], 0);
    struct control_peer *replacement = start_preflight(1, 0, 9);
    zassert_false(replacement->lighting_preflight_done);
    deliver_wake(1, 1, 20);
    poll();
    zassert_equal(wake_calls, 1);
    zassert_equal(observed_wake.side, 1);
    zassert_equal(observed_wake.boot, 20);
}

ZTEST(defy_battery_central, test_preflight_stale_callback_cannot_consume_replacement) {
    struct control_peer *old = start_preflight(0, 0, 8);
    struct bt_gatt_read_params *params = &old->read;
    bt_gatt_read_func_t callback = params->func;
    defy_control_ble_disconnected(connection(0), 0, 8);
    defy_control_ble_connected(connection(1), 0, 9);
    poll();
    now += 1000;
    poll();
    discovered(connection(1), 42);
    now++;
    poll();
    struct control_peer *replacement = lighting_peer(1);
    zassert_not_equal(params, &replacement->read);
    zassert_equal(callback(connection(0), 0, params, NULL, 0), BT_GATT_ITER_STOP);
    zassert_equal(wake_calls, 0);
    zassert_false(replacement->lighting_preflight_done);
    deliver_wake(1, 0, 100);
    poll();
    zassert_equal(wake_calls, 1);
}

ZTEST(defy_battery_central, test_preflight_maintenance_defers_wake_policy_and_counts_age) {
    lighting_available = true;
    start_preflight(0, 0, 8);
    deliver_wake(0, 0, UINT32_MAX - 10);
    maintenance_active_test = true;
    now += 100;
    poll();
    lighting_send_work(NULL);
    zassert_equal(wake_calls, 0);
    zassert_equal(write_calls, 0);
    maintenance_active_test = false;
    poll();
    zassert_equal(wake_calls, 1);
    zassert_equal(observed_wake.age_ms, UINT32_MAX);
}

ZTEST(defy_battery_central, test_lighting_before_side_identification_and_ack_stops_retries) {
    struct control_peer *peer = lighting_ready(0, 1, 5);
    uint8_t side;
    zassert_equal(defy_battery_central_route_side(DEFY_LINK_BLE, 1, 5, &side), -ENODATA);
    lighting_available = true;
    defy_control_ble_lighting_changed();
    zassert_equal(scheduled_queue, &control_work_queue);
    poll();
    zassert_equal(last_submitted, &lighting_send);
    zassert_equal(submitted_queue, &control_work_queue);
    zassert_equal(write_calls, 0);
    lighting_send_work(NULL);
    zassert_equal(write_calls, 1);
    zassert_equal(written_handle, 61);
    zassert_true(defy_lighting_equal(&written_lighting, &desired_lighting));
    poll();
    lighting_ack(0, &desired_lighting);
    zassert_true(peer->lighting.confirmed);
    now += 5000;
    poll();
    lighting_send_work(NULL);
    zassert_equal(write_calls, 1);
    zassert_true(scheduled_delay > 1000);
}

ZTEST(defy_battery_central, test_lighting_mismatched_ack_retries_then_confirms) {
    struct control_peer *peer = lighting_ready(0, 0, 5);
    lighting_available = true;
    poll();
    lighting_send_work(NULL);
    poll();
    struct defy_lighting_state wrong = desired_lighting;
    wrong.enabled = !wrong.enabled;
    lighting_ack(0, &wrong);
    zassert_false(peer->lighting.confirmed);
    now = peer->lighting.due;
    poll();
    lighting_send_work(NULL);
    zassert_equal(write_calls, 2);
    poll();
    lighting_ack(0, &desired_lighting);
    zassert_true(peer->lighting.confirmed);
}

ZTEST(defy_battery_central, test_lighting_buffer_failures_are_bounded_and_new_revision_retries) {
    struct control_peer *peer = lighting_ready(0, 0, 5);
    lighting_available = true;
    write_result = -ENOMEM;
    for (size_t i = 0; i < DEFY_LIGHTING_MAX_ATTEMPTS; i++) {
        poll();
        lighting_send_work(NULL);
        zassert_equal(write_calls, i + 1);
        zassert_false(peer->lighting_read_pending);
        zassert_true(peer->lighting.due - now <= 4000);
        now = peer->lighting.due;
    }
    for (size_t i = 0; i < 3; i++) {
        now += 5000;
        poll();
        lighting_send_work(NULL);
    }
    zassert_equal(write_calls, DEFY_LIGHTING_MAX_ATTEMPTS);
    desired_lighting.revision++;
    desired_lighting.enabled = false;
    poll();
    lighting_send_work(NULL);
    zassert_equal(write_calls, DEFY_LIGHTING_MAX_ATTEMPTS + 1);
    zassert_equal(peer->lighting.attempts, 1);
    zassert_false(written_lighting.enabled);
}

ZTEST(defy_battery_central, test_lighting_disconnect_drops_queued_work_and_reconnect_replays) {
    lighting_ready(0, 0, 5);
    lighting_available = true;
    poll();
    defy_control_ble_disconnected(connection(0), 0, 5);
    lighting_send_work(NULL);
    zassert_equal(write_calls, 0);
    poll();
    zassert_equal(refs[0], 0);
    lighting_ready(1, 0, 6);
    poll();
    lighting_send_work(NULL);
    zassert_equal(write_calls, 1);
    zassert_equal(written_peer, 1);
    zassert_true(defy_lighting_equal(&written_lighting, &desired_lighting));
}

ZTEST(defy_battery_central, test_lighting_queued_old_state_and_old_ack_cannot_confirm_new_state) {
    struct control_peer *peer = lighting_ready(0, 0, 5);
    lighting_available = true;
    poll();
    desired_lighting.revision++;
    desired_lighting.enabled = false;
    lighting_send_work(NULL);
    zassert_equal(write_calls, 0);
    poll();
    lighting_send_work(NULL);
    poll();
    struct defy_lighting_state old = desired_lighting;
    desired_lighting.revision++;
    desired_lighting.enabled = true;
    poll();
    lighting_send_work(NULL);
    lighting_ack(0, &old);
    zassert_false(peer->lighting.confirmed);
    poll();
    lighting_ack(0, &desired_lighting);
    zassert_true(peer->lighting.confirmed);
    zassert_equal(write_calls, 2);
}

ZTEST(defy_battery_central, test_lighting_old_write_only_peer_stops_after_read_permission_error) {
    struct control_peer *peer = lighting_ready(0, 0, 5);
    lighting_available = true;
    poll();
    lighting_send_work(NULL);
    poll();
    peer->read.func(connection(0), BT_ATT_ERR_READ_NOT_PERMITTED, &peer->read, NULL, 0);
    zassert_equal(peer->lighting.attempts, DEFY_LIGHTING_MAX_ATTEMPTS);
    now += BATTERY_POLL_MS;
    poll();
    lighting_send_work(NULL);
    zassert_equal(write_calls, 1);
    zassert_equal(read_done(last_read), battery_read_done);
    deliver(connection(0), sample(0, 10, 1, 80));
    zassert_true(record(0).valid);
}

ZTEST(defy_battery_central, test_frame_only_peer_keeps_telemetry_without_guessing_control_handle) {
    lighting_available = true;
    defy_control_ble_connected(connection(0), 1, 5);
    now = 1000;
    poll();
    struct bt_gatt_chrc characteristic = {.uuid = &battery_uuid.uuid, .value_handle = 42};
    struct bt_gatt_attr attr = {.user_data = &characteristic};
    zassert_equal(last_discovery->func(connection(0), &attr, last_discovery), BT_GATT_ITER_CONTINUE);
    zassert_equal(last_discovery->func(connection(0), NULL, last_discovery), BT_GATT_ITER_STOP);
    now++;
    poll();
    deliver(connection(0), sample(0, 10, 1, 80));
    zassert_true(record(0).valid);
    lighting_send_work(NULL);
    zassert_equal(lighting_peer(0)->lighting.attempts, 0);
    zassert_equal(scheduled_delay, BATTERY_POLL_MS);
    defy_control_ble_battery_show(BIT(0));
    poll();
    battery_show_work(NULL);
    zassert_equal(write_calls, 0);
    now += DEFY_CONTROL_TIMEOUT_MS;
    poll();
    battery_show_work(NULL);
    zassert_equal(write_calls, 0);
}

/* A peer whose wake report named side, with its first battery read failed. */
static struct control_peer *groups_ready(uint8_t index, uint8_t slot, uint32_t epoch,
                                         uint8_t side) {
    struct control_peer *peer = start_preflight(index, slot, epoch);
    deliver_wake(index, side, 10);
    poll();
    zassert_true(peer->lighting_side_known);
    zassert_equal(peer->lighting_side, side);
    zassert_equal(read_done(last_read), battery_read_done);
    last_read->func(connection(index), BT_ATT_ERR_UNLIKELY, last_read, NULL, 0);
    poll();
    return peer;
}

/* The write numbered n, from 0: its opcode, and for a group its index. */
static void assert_write(size_t n, uint8_t opcode, uint8_t index) {
    zassert_true(n < MIN(write_calls, ARRAY_SIZE(write_log)));
    zassert_equal(write_log[n].data[0], DEFY_LIGHTING_VERSION);
    zassert_equal(write_log[n].data[1], opcode);
    if (opcode == DEFY_CONTROL_LIGHTING_GROUP) {
        zassert_equal(write_log[n].length, DEFY_LIGHTING_GROUP_SIZE);
        zassert_equal(write_log[n].data[6], index);
    } else {
        zassert_equal(write_log[n].length, DEFY_LIGHTING_CONTROL_SIZE);
    }
}

ZTEST(defy_battery_central, test_lighting_groups_go_before_the_state_until_confirmed) {
    struct control_peer *peer = groups_ready(0, 0, 5, DEFY_WIRED_RIGHT);
    lighting_available = true;
    desired_lighting.groups = BIT(1) | BIT(4);
    desired_lighting.level = 128;
    poll();
    lighting_send_work(NULL);
    zassert_equal(write_calls, 3);
    assert_write(0, DEFY_CONTROL_LIGHTING_GROUP, 1);
    assert_write(1, DEFY_CONTROL_LIGHTING_GROUP, 4);
    assert_write(2, DEFY_CONTROL_LIGHTING_SET, 0);
    zassert_equal(group_side, DEFY_WIRED_RIGHT);
    zassert_equal(write_log[0].peer, 0);
    zassert_equal(written_handle, 61);

    /* Unacknowledged: the retry sends both groups again. */
    poll();
    peer->read.func(connection(0), BT_ATT_ERR_UNLIKELY, &peer->read, NULL, 0);
    now = peer->lighting.due;
    poll();
    lighting_send_work(NULL);
    zassert_equal(write_calls, 6);
    assert_write(3, DEFY_CONTROL_LIGHTING_GROUP, 1);
    assert_write(4, DEFY_CONTROL_LIGHTING_GROUP, 4);
    assert_write(5, DEFY_CONTROL_LIGHTING_SET, 0);
    poll();
    lighting_ack(0, &desired_lighting);
    zassert_true(peer->lighting.confirmed);
    zassert_equal(peer->lighting_groups, BIT(1) | BIT(4));

    /* A state that names one more group sends that one alone. */
    desired_lighting.revision++;
    desired_lighting.groups = BIT(4) | BIT(6);
    poll();
    lighting_send_work(NULL);
    zassert_equal(write_calls, 8);
    assert_write(6, DEFY_CONTROL_LIGHTING_GROUP, 6);
    assert_write(7, DEFY_CONTROL_LIGHTING_SET, 0);
    poll();
    lighting_ack(0, &desired_lighting);
    zassert_equal(peer->lighting_groups, BIT(1) | BIT(4) | BIT(6));
    desired_lighting.revision++;
    desired_lighting.groups = BIT(1) | BIT(6);
    poll();
    lighting_send_work(NULL);
    zassert_equal(write_calls, 9);
    assert_write(8, DEFY_CONTROL_LIGHTING_SET, 0);
    zassert_equal(group_calls, 5);
}

ZTEST(defy_battery_central, test_lighting_ack_of_another_state_confirms_no_groups) {
    struct control_peer *peer = groups_ready(0, 0, 5, DEFY_WIRED_LEFT);
    lighting_available = true;
    desired_lighting.groups = BIT(3);
    desired_lighting.level = 10;
    poll();
    lighting_send_work(NULL);
    poll();
    struct defy_lighting_state other = desired_lighting;
    other.groups = BIT(3) | BIT(9);
    other.level = 11;
    lighting_ack(0, &other);
    zassert_false(peer->lighting.confirmed);
    zassert_equal(peer->lighting_groups, 0);
}

ZTEST(defy_battery_central, test_lighting_reconnect_sends_confirmed_groups_again) {
    struct control_peer *peer = groups_ready(0, 0, 5, DEFY_WIRED_LEFT);
    lighting_available = true;
    desired_lighting.groups = BIT(0) | BIT(15);
    desired_lighting.level = 255;
    poll();
    lighting_send_work(NULL);
    poll();
    lighting_ack(0, &desired_lighting);
    zassert_equal(peer->lighting_groups, BIT(0) | BIT(15));
    defy_control_ble_disconnected(connection(0), 0, 5);
    poll();
    zassert_equal(refs[0], 0);
    peer = groups_ready(1, 0, 6, DEFY_WIRED_LEFT);
    zassert_equal(peer->lighting_groups, 0);
    lighting_send_work(NULL);
    zassert_equal(write_calls, 6);
    assert_write(3, DEFY_CONTROL_LIGHTING_GROUP, 0);
    assert_write(4, DEFY_CONTROL_LIGHTING_GROUP, 15);
    assert_write(5, DEFY_CONTROL_LIGHTING_SET, 0);
    zassert_equal(write_log[5].peer, 1);
    zassert_equal(group_side, DEFY_WIRED_LEFT);
}

ZTEST(defy_battery_central, test_lighting_group_failures_hold_the_state_for_a_retry) {
    struct control_peer *peer = groups_ready(0, 0, 5, DEFY_WIRED_LEFT);
    lighting_available = true;
    desired_lighting.groups = BIT(2) | BIT(3) | BIT(5);
    desired_lighting.level = 64;
    group_error = -EAGAIN;
    poll();
    lighting_send_work(NULL);
    zassert_equal(write_calls, 0);
    zassert_equal(peer->lighting.attempts, 1);
    zassert_false(peer->lighting_read_pending);

    /* The third write fails: the next attempt sends every group again. */
    group_error = 0;
    failing_write = 3;
    now = peer->lighting.due;
    poll();
    lighting_send_work(NULL);
    zassert_equal(write_calls, 3);
    zassert_equal(peer->lighting.attempts, 2);
    zassert_false(peer->lighting_read_pending);
    now = peer->lighting.due;
    poll();
    lighting_send_work(NULL);
    zassert_equal(write_calls, 7);
    assert_write(3, DEFY_CONTROL_LIGHTING_GROUP, 2);
    assert_write(4, DEFY_CONTROL_LIGHTING_GROUP, 3);
    assert_write(5, DEFY_CONTROL_LIGHTING_GROUP, 5);
    assert_write(6, DEFY_CONTROL_LIGHTING_SET, 0);
    zassert_true(peer->lighting_read_pending);
}

ZTEST(defy_battery_central, test_lighting_groups_wait_for_the_side_from_battery_frames) {
    /* No wake report and no battery frame: the half's side is not known. */
    struct control_peer *peer = lighting_ready(0, 1, 5);
    zassert_false(peer->lighting_side_known);
    lighting_available = true;
    desired_lighting.groups = BIT(7);
    desired_lighting.level = 32;
    poll();
    lighting_send_work(NULL);
    zassert_equal(write_calls, 0);
    zassert_equal(group_calls, 0);
    zassert_equal(peer->lighting.attempts, 1);
    now += BATTERY_POLL_MS;
    poll();
    deliver(connection(0), sample(1, 10, 1, 80));
    zassert_true(record(1).valid);
    lighting_send_work(NULL);
    zassert_equal(write_calls, 2);
    assert_write(0, DEFY_CONTROL_LIGHTING_GROUP, 7);
    assert_write(1, DEFY_CONTROL_LIGHTING_SET, 0);
    zassert_equal(group_side, DEFY_WIRED_RIGHT);
    zassert_equal(written_group.leds, BIT64(7) | BIT64(33));
}

ZTEST(defy_battery_central, test_maintenance_drops_pending_and_new_battery_display_requests) {
    start_read(0, 1, 5);
    deliver(connection(0), sample(0, 10, 1, 80));
    defy_control_ble_battery_show(BIT(0));
    poll();
    zassert_equal(last_submitted, &battery_show_send);
    maintenance_active_test = true;
    battery_show_work(NULL);
    zassert_equal(write_calls, 0);
    defy_control_ble_battery_show(BIT(0));
    poll();
    maintenance_active_test = false;
    poll();
    battery_show_work(NULL);
    zassert_equal(write_calls, 0);
    defy_control_ble_battery_show(BIT(0));
    poll();
    battery_show_work(NULL);
    zassert_equal(write_calls, 1);
}

ZTEST(defy_battery_central, test_maintenance_defers_lighting_and_resumes_current_desired_state) {
    struct control_peer *peer = lighting_ready(0, 0, 5);
    lighting_available = true;
    poll();
    zassert_equal(last_submitted, &lighting_send);
    maintenance_active_test = true;
    lighting_send_work(NULL);
    zassert_equal(write_calls, 0);
    desired_lighting.revision++;
    desired_lighting.enabled = false;
    now += 5000;
    poll();
    lighting_send_work(NULL);
    zassert_equal(write_calls, 0);
    maintenance_active_test = false;
    poll();
    lighting_send_work(NULL);
    zassert_equal(write_calls, 1);
    zassert_true(defy_lighting_equal(&written_lighting, &desired_lighting));
    zassert_equal(peer->lighting.attempts, 1);
}

ZTEST(defy_battery_central, test_maintenance_pauses_new_gatt_io_but_cleans_up_disconnected_peer) {
    defy_control_ble_connected(connection(0), 0, 1);
    maintenance_active_test = true;
    now = 1000;
    poll();
    zassert_equal(discovery_calls, 0);
    zassert_equal(read_calls, 0);
    zassert_equal(scheduled_queue, &control_work_queue);
    zassert_equal(scheduled_delay, RETRY_MS);
    maintenance_active_test = false;
    poll();
    zassert_equal(discovery_calls, 1);
    maintenance_active_test = true;
    defy_control_ble_disconnected(connection(0), 0, 1);
    poll();
    zassert_equal(cancel_calls, 1);
    poll();
    zassert_equal(refs[0], 0);
    zassert_equal(discovery_calls, 1);
    zassert_equal(read_calls, 0);
}

ZTEST_SUITE(defy_battery_central, NULL, NULL, reset, NULL, NULL);

ZTEST(defy_battery_central, test_ble_side_contradiction_requests_current_epoch_recovery) {
    start_read(0, 1, 5);
    zassert_ok(defy_battery_central_route_identify(DEFY_LINK_BLE, 1, 5, 1));
    deliver(connection(0), sample(0, 10, 1, 80));
    zassert_equal(rejected_peers, 1);
    zassert_equal(rejected_slot, 1);
    zassert_equal(rejected_epoch, 5);
    zassert_false(record(0).valid);
}

ZTEST(defy_battery_central, test_ble_duplicate_side_requests_newcomer_recovery) {
    zassert_ok(defy_battery_central_route_live(DEFY_LINK_BLE, 0, 4, 0));
    zassert_ok(accept(DEFY_LINK_BLE, 0, 4, sample(0, 9, 1, 80)));
    start_read(0, 1, 5);
    deliver(connection(0), sample(0, 10, 1, 70));
    zassert_equal(rejected_peers, 1);
    zassert_equal(rejected_slot, 1);
    zassert_equal(rejected_epoch, 5);
    zassert_equal(record(0).frame.session, 9);
}

ZTEST(defy_battery_central, test_ble_stale_sample_does_not_recover_healthy_peer) {
    start_read(0, 1, 5);
    zassert_ok(accept(DEFY_LINK_BLE, 1, 5, sample(0, 10, 2, 80)));
    deliver(connection(0), sample(0, 10, 1, 70));
    zassert_equal(rejected_peers, 0);
    zassert_equal(record(0).frame.sample.sequence, 2);
}
