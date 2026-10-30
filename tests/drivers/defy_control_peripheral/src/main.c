/* SPDX-License-Identifier: MIT */

#include <dygma/defy_battery.h>
#include <dygma/defy_control.h>
#include <dygma/defy_half.h>
#include <dygma/defy_half_power.h>
#include <dygma/defy_half_version.h>
#include <dygma/defy_lighting.h>
#include <dygma/defy_feedback.h>
#include <dygma/defy_wired_protocol.h>
#include <zephyr/bluetooth/conn.h>
#include <zephyr/bluetooth/gatt.h>
#include <zephyr/random/random.h>
#include <zephyr/ztest.h>

#include "fake_uptime.h"

static const struct defy_half_version local_version = {
    .side = DEFY_WIRED_RIGHT, .program_crc = 0xc0ffee11, .seal_version = 7,
    .module = "v0.2.0-5-gabcdef1-",
};
static unsigned int local_version_calls;
void defy_half_version_local(struct defy_half_version *version) {
    local_version_calls++;
    *version = local_version;
}

/* The service's declarations, rather than Bluetooth's registry. */
#define READ_STUB(name)                                                                            \
    ssize_t name(struct bt_conn *conn, const struct bt_gatt_attr *attr, void *buf,                 \
                 uint16_t len, uint16_t offset) {                                                  \
        ARG_UNUSED(conn); ARG_UNUSED(attr); ARG_UNUSED(buf); ARG_UNUSED(len); ARG_UNUSED(offset); \
        return 0;                                                                                  \
    }
READ_STUB(bt_gatt_attr_read_service)
READ_STUB(bt_gatt_attr_read_chrc)

static unsigned int feedback_calls;
static struct defy_host_feedback feedback_applied;
int defy_half_leds_host_show(const struct defy_host_feedback *state) {
    feedback_calls++;
    feedback_applied = *state;
    return 0;
}

static struct defy_battery_sample input;
static uint32_t input_generation;
static int input_error;
static uint32_t random_value;
static unsigned int random_calls;
static bool change_generation_in_get;
static bool disconnect_in_get;
static bool bluetooth_ready;
static bool woke_from_key;
static unsigned int show_calls;
static struct k_work *submitted_work;
static struct defy_battery_frame concurrent_frame;
static unsigned int lighting_calls;
static unsigned int lighting_epoch_calls;
static uint32_t active_lighting_epoch;
static uint32_t applied_lighting_epoch;
static uint8_t applied_lighting_route;
static int lighting_error;
static bool lighting_available;
static struct defy_lighting_state applied_lighting;
static void disconnected(struct bt_conn *conn, uint8_t reason);

int defy_lighting_peripheral_apply(const struct defy_lighting_state *state, enum defy_link route,
                                  uint32_t epoch) {
    lighting_calls++;
    applied_lighting_route = route;
    applied_lighting_epoch = epoch;
    if (lighting_error) {
        return lighting_error;
    }
    if (!epoch || epoch != active_lighting_epoch) {
        return -ESTALE;
    }
    applied_lighting = *state;
    lighting_available = true;
    return 0;
}

bool defy_lighting_peripheral_get(struct defy_lighting_state *state) {
    if (lighting_available) {
        *state = applied_lighting;
    }
    return lighting_available;
}

void defy_lighting_peripheral_ble_epoch(uint32_t epoch) {
    active_lighting_epoch = epoch;
    lighting_epoch_calls++;
}

/* The groups the service passed on, the last of them, and what keeping one returns. */
static unsigned int group_calls;
static struct defy_lighting_group group_kept;
static int group_result;

int defy_lighting_peripheral_group(const struct defy_lighting_group *group) {
    group_calls++;
    group_kept = *group;
    return group_result;
}

int defy_battery_hci_get_generation(struct defy_battery_sample *sample, uint32_t *generation) {
    if (disconnect_in_get) {
        disconnect_in_get = false;
        disconnected(NULL, 0);
    }
    if (input_error != 0) {
        return input_error;
    }
    *sample = input;
    *generation = input_generation;
    if (change_generation_in_get) {
        change_generation_in_get = false;
        input_generation++;
        input.sequence = 1;
        zassert_ok(defy_battery_local_frame(&concurrent_frame));
    }
    return 0;
}

bool bt_is_ready(void) { return bluetooth_ready; }

int defy_half_leds_battery_show(void) { show_calls++; return 0; }
static bool test_woke_from_key(void) { return woke_from_key; }
static int test_submit(struct k_work *work) { submitted_work = work; return 1; }

static uint32_t test_random(void) {
    random_calls++;
    return random_value;
}

ssize_t bt_gatt_attr_read(struct bt_conn *conn, const struct bt_gatt_attr *attr,
                         void *buf, uint16_t len, uint16_t offset,
                         const void *value, uint16_t value_len) {
    ARG_UNUSED(conn);
    ARG_UNUSED(attr);
    if (offset > value_len) {
        return BT_GATT_ERR(BT_ATT_ERR_INVALID_OFFSET);
    }
    size_t count = MIN(len, value_len - offset);
    memcpy(buf, (const uint8_t *)value + offset, count);
    return count;
}

#undef BT_GATT_SERVICE_DEFINE
#define BT_GATT_SERVICE_DEFINE(name, ...)                                                          \
    static const struct bt_gatt_attr name##_attrs[] = {__VA_ARGS__}
#undef BT_CONN_CB_DEFINE
#define BT_CONN_CB_DEFINE(name) struct bt_conn_cb name
#define sys_rand32_get test_random
#define k_work_submit test_submit
#define defy_half_power_woke_from_key test_woke_from_key
#include "../../../../battery/defy_control_peripheral.c"
#undef sys_rand32_get
#undef k_uptime_get
#undef k_work_submit
#undef defy_half_power_woke_from_key

static void before(void *fixture) {
    ARG_UNUSED(fixture);
    k_thread_abort(defy_control_nonce);
    session = generation = 0;
    wake_boot = 0;
    nonce_ready = false;
    connection_epoch = 0;
    read_available = false;
    memset(read_frame, 0, sizeof(read_frame));
    input = (struct defy_battery_sample){.sequence = 10, .age_ms = 1000};
    for (size_t i = 0; i < DEFY_BATTERY_WORD_COUNT; i++) {
        input.words[i] = 0x1800 + i;
    }
    input_generation = 1;
    input_error = 0;
    random_value = 0x12345678;
    random_calls = 0;
    change_generation_in_get = false;
    disconnect_in_get = false;
    bluetooth_ready = true;
    woke_from_key = false;
    now = 0;
    show_calls = 0;
    show_pending = false;
    feedback_calls = 0;
    feedback_pending = false;
    feedback_deadline = feedback_epoch = 0;
    lighting_pending = false;
    lighting_epoch = lighting_deadline = 0;
    lighting_requested = (struct defy_lighting_state){0};
    lighting_calls = lighting_epoch_calls = 0;
    active_lighting_epoch = applied_lighting_epoch = 0;
    applied_lighting_route = UINT8_MAX;
    lighting_error = 0;
    lighting_available = false;
    applied_lighting = (struct defy_lighting_state){0};
    group_calls = 0;
    group_kept = (struct defy_lighting_group){0};
    group_result = 0;
    submitted_work = NULL;
    memset(&concurrent_frame, 0, sizeof(concurrent_frame));
    initialize_nonce();
}

ZTEST(control_peripheral, test_wired_and_ble_share_one_session) {
    struct defy_battery_frame wired, ble;
    uint8_t data[DEFY_BATTERY_FRAME_SIZE];

    zassert_ok(defy_battery_local_frame(&wired));
    zassert_equal(read_battery(NULL, NULL, data, sizeof(data), 0), sizeof(data));
    zassert_ok(defy_battery_frame_decode(data, sizeof(data), &ble));
    zassert_equal(wired.side, 1);
    zassert_equal(ble.side, wired.side);
    zassert_equal(ble.session, wired.session);
    zassert_equal(random_calls, 1);
    input.sequence++;
    zassert_ok(defy_battery_local_frame(&wired));
    zassert_equal(wired.session, ble.session);
    zassert_equal(random_calls, 1);
}

ZTEST(control_peripheral, test_radio_generation_changes_session) {
    struct defy_battery_frame before_reset, after_reset;

    zassert_ok(defy_battery_local_frame(&before_reset));
    input_generation++;
    input.sequence = 1;
    random_value++;
    zassert_ok(defy_battery_local_frame(&after_reset));
    zassert_not_equal(after_reset.session, before_reset.session);
    zassert_equal(after_reset.sample.sequence, 1);
    zassert_equal(random_calls, 1);
}

ZTEST(control_peripheral, test_unavailable_sample_does_not_make_frame) {
    struct defy_battery_frame frame = {.session = 123};

    input_error = -ENODATA;
    zassert_equal(defy_battery_local_frame(&frame), -ENODATA);
    zassert_equal(frame.session, 123);
    zassert_equal(random_calls, 1);
    zassert_equal(defy_battery_local_frame(NULL), -EINVAL);
}

ZTEST(control_peripheral, test_concurrent_older_generation_cannot_revert_session) {
    struct defy_battery_frame frame = {.session = 456};

    change_generation_in_get = true;
    zassert_equal(defy_battery_local_frame(&frame), -EAGAIN);
    zassert_equal(frame.session, 456);
    zassert_equal(generation, 2);
    zassert_equal(concurrent_frame.sample.sequence, 1);
}

ZTEST(control_peripheral, test_long_read_freezes_sample_across_offsets) {
    struct defy_battery_frame decoded;
    uint8_t data[DEFY_BATTERY_FRAME_SIZE];

    zassert_equal(read_battery(NULL, NULL, data, 22, 0), 22);
    input.sequence++;
    input.words[5] ^= 0xffff;
    zassert_equal(read_battery(NULL, NULL, data + 22, 22, 22), 10);
    zassert_ok(defy_battery_frame_decode(data, sizeof(data), &decoded));
    zassert_equal(decoded.sample.sequence, 10);
    zassert_equal(decoded.sample.words[5], 0x1805);
    zassert_equal(read_battery(NULL, NULL, data, sizeof(data), 0), sizeof(data));
    zassert_ok(defy_battery_frame_decode(data, sizeof(data), &decoded));
    zassert_equal(decoded.sample.sequence, 11);
    zassert_equal(decoded.sample.words[5], input.words[5]);
}

ZTEST(control_peripheral, test_disconnect_invalidates_partial_read) {
    uint8_t data[DEFY_BATTERY_FRAME_SIZE];

    zassert_equal(read_battery(NULL, NULL, data, 22, 0), 22);
    control_connection.disconnected(NULL, 0);
    zassert_equal(read_battery(NULL, NULL, data + 22, 10, 22),
                  BT_GATT_ERR(BT_ATT_ERR_UNLIKELY));
    zassert_equal(read_battery(NULL, NULL, data, sizeof(data), 0), sizeof(data));
}

ZTEST(control_peripheral, test_disconnect_during_snapshot_cannot_restore_partial_read) {
    uint8_t data[DEFY_BATTERY_FRAME_SIZE];

    zassert_equal(read_battery(NULL, NULL, data, 22, 0), 22);
    disconnect_in_get = true;
    zassert_equal(read_battery(NULL, NULL, data, 22, 0),
                  BT_GATT_ERR(BT_ATT_ERR_UNLIKELY));
    zassert_equal(read_battery(NULL, NULL, data + 22, 10, 22),
                  BT_GATT_ERR(BT_ATT_ERR_UNLIKELY));
}

ZTEST(control_peripheral, test_failed_new_read_invalidates_previous_partial_read) {
    uint8_t data[DEFY_BATTERY_FRAME_SIZE];

    zassert_equal(read_battery(NULL, NULL, data, 22, 0), 22);
    input_error = -ENODATA;
    zassert_equal(read_battery(NULL, NULL, data, 22, 0),
                  BT_GATT_ERR(BT_ATT_ERR_UNLIKELY));
    zassert_equal(read_battery(NULL, NULL, data + 22, 10, 22),
                  BT_GATT_ERR(BT_ATT_ERR_UNLIKELY));
}

ZTEST(control_peripheral, test_zero_random_still_produces_nonzero_session) {
    struct defy_battery_frame frame;

    random_value = 0;
    initialize_nonce();
    zassert_ok(defy_battery_local_frame(&frame));
    zassert_not_equal(frame.session, 0);
}

ZTEST(control_peripheral, test_repeated_maximum_random_skips_zero_session) {
    struct defy_battery_frame first, second;

    random_value = UINT32_MAX;
    initialize_nonce();
    zassert_ok(defy_battery_local_frame(&first));
    input_generation++;
    zassert_ok(defy_battery_local_frame(&second));
    zassert_not_equal(second.session, first.session);
    zassert_not_equal(second.session, 0);
}

ZTEST(control_peripheral, test_transport_waits_for_nonce_without_requesting_random) {
    struct defy_battery_frame frame = {.session = 456};
    uint8_t data[DEFY_BATTERY_FRAME_SIZE];
    nonce_ready = false;
    random_calls = 0;

    zassert_equal(defy_battery_local_frame(&frame), -EAGAIN);
    zassert_equal(frame.session, 456);
    zassert_equal(read_battery(NULL, NULL, data, sizeof(data), 0),
                  BT_GATT_ERR(BT_ATT_ERR_UNLIKELY));
    zassert_equal(random_calls, 0);
}

static K_THREAD_STACK_DEFINE(nonce_test_stack, 1024);
static struct k_thread nonce_test_thread;

ZTEST(control_peripheral, test_nonce_thread_waits_for_bluetooth_and_runs_once) {
    nonce_ready = false;
    session = 0;
    bluetooth_ready = false;
    random_calls = 0;
    k_thread_create(&nonce_test_thread, nonce_test_stack, K_THREAD_STACK_SIZEOF(nonce_test_stack),
                    nonce_thread, NULL, NULL, NULL, K_PRIO_PREEMPT(8), 0, K_NO_WAIT);
    k_sleep(K_MSEC(10));
    zassert_false(nonce_ready);
    zassert_equal(random_calls, 0);
    bluetooth_ready = true;
    k_sleep(K_MSEC(1100));
    zassert_true(nonce_ready);
    zassert_equal(random_calls, 1);
    zassert_equal(session, random_value);
    struct defy_battery_frame frame;
    zassert_ok(defy_battery_local_frame(&frame));
    input_generation++;
    zassert_ok(defy_battery_local_frame(&frame));
    zassert_equal(random_calls, 1);
}

static const uint8_t show_command[] = {
    DEFY_CONTROL_BATTERY_SHOW_VERSION, DEFY_CONTROL_BATTERY_SHOW,
};

ZTEST(control_peripheral, test_control_schedules_show_outside_bluetooth_callback) {
    zassert_equal(write_control(NULL, NULL, show_command, sizeof(show_command), 0,
                               BT_GATT_WRITE_FLAG_CMD), sizeof(show_command));
    zassert_equal(show_calls, 0);
    zassert_equal(submitted_work, &show_battery);
    show_battery_work(NULL);
    zassert_equal(show_calls, 1);
    show_battery_work(NULL);
    zassert_equal(show_calls, 1);
}
ZTEST(control_peripheral, test_control_rejects_other_operations_lengths_offsets_and_flags) {
    uint8_t bad[] = {DEFY_CONTROL_BATTERY_SHOW_VERSION, DEFY_CONTROL_LIGHTING_ACK};
    zassert_equal(write_control(NULL, NULL, bad, sizeof(bad), 0, BT_GATT_WRITE_FLAG_CMD),
                  BT_GATT_ERR(BT_ATT_ERR_VALUE_NOT_ALLOWED));
    /* Another command's opcode in the display command's length. */
    bad[1] = DEFY_CONTROL_LIGHTING_SET;
    zassert_equal(write_control(NULL, NULL, bad, sizeof(bad), 0, BT_GATT_WRITE_FLAG_CMD),
                  BT_GATT_ERR(BT_ATT_ERR_INVALID_ATTRIBUTE_LEN));
    bad[0] = 2;
    bad[1] = DEFY_CONTROL_BATTERY_SHOW;
    zassert_equal(write_control(NULL, NULL, bad, sizeof(bad), 0, BT_GATT_WRITE_FLAG_CMD),
                  BT_GATT_ERR(BT_ATT_ERR_VALUE_NOT_ALLOWED));
    zassert_equal(write_control(NULL, NULL, show_command, 1, 0, BT_GATT_WRITE_FLAG_CMD),
                  BT_GATT_ERR(BT_ATT_ERR_INVALID_ATTRIBUTE_LEN));
    zassert_equal(write_control(NULL, NULL, show_command, 2, 1, BT_GATT_WRITE_FLAG_CMD),
                  BT_GATT_ERR(BT_ATT_ERR_INVALID_OFFSET));
    zassert_equal(write_control(NULL, NULL, show_command, 2, 0, BT_GATT_WRITE_FLAG_PREPARE),
                  BT_GATT_ERR(BT_ATT_ERR_NOT_SUPPORTED));
    zassert_equal(write_control(NULL, NULL, show_command, 2, 0, 0),
                  BT_GATT_ERR(BT_ATT_ERR_NOT_SUPPORTED));
    show_battery_work(NULL);
    zassert_equal(show_calls, 0);
}
ZTEST(control_peripheral, test_control_expires_before_delayed_work_runs) {
    zassert_equal(write_control(NULL, NULL, show_command, 2, 0, BT_GATT_WRITE_FLAG_CMD), 2);
    now += DEFY_CONTROL_TIMEOUT_MS;
    show_battery_work(NULL);
    zassert_equal(show_calls, 0);
}
ZTEST(control_peripheral, test_control_disconnect_drops_queued_display) {
    zassert_equal(write_control(NULL, NULL, show_command, 2, 0, BT_GATT_WRITE_FLAG_CMD), 2);
    disconnected(NULL, 0);
    show_battery_work(NULL);
    zassert_equal(show_calls, 0);
    zassert_equal(write_control(NULL, NULL, show_command, 2, 0, BT_GATT_WRITE_FLAG_CMD), 2);
    show_battery_work(NULL);
    zassert_equal(show_calls, 1);
}

static ssize_t request_state(const struct defy_lighting_state *state) {
    uint8_t command[DEFY_LIGHTING_CONTROL_SIZE];
    zassert_ok(defy_lighting_control_encode(state, DEFY_CONTROL_LIGHTING_SET, command));
    return write_control(NULL, NULL, command, sizeof(command), 0, BT_GATT_WRITE_FLAG_CMD);
}

static ssize_t request_lighting(uint32_t boot, uint32_t revision, bool enabled) {
    struct defy_lighting_state state = {boot, revision, enabled};
    return request_state(&state);
}

ZTEST(control_peripheral, test_lighting_connection_registers_epoch_before_worker) {
    control_connection.connected(NULL, 1);
    zassert_equal(lighting_epoch_calls, 0);
    control_connection.connected(NULL, 0);
    zassert_equal(active_lighting_epoch, 1);
    zassert_equal(lighting_epoch_calls, 1);
    zassert_equal(request_lighting(123, 4, false), DEFY_LIGHTING_CONTROL_SIZE);
    zassert_equal(lighting_calls, 0);
    zassert_equal(submitted_work, &apply_lighting);
    apply_lighting_work(NULL);
    zassert_equal(lighting_calls, 1);
    zassert_equal(applied_lighting_route, DEFY_LINK_BLE);
    zassert_equal(applied_lighting_epoch, active_lighting_epoch);
    zassert_equal(applied_lighting.boot, 123);
    zassert_equal(applied_lighting.revision, 4);
    zassert_false(applied_lighting.enabled);
    apply_lighting_work(NULL);
    zassert_equal(lighting_calls, 1);
}

ZTEST(control_peripheral, test_lighting_rejects_malformed_writes_without_queueing) {
    struct defy_lighting_state state = {1, 1, true};
    uint8_t command[DEFY_LIGHTING_CONTROL_SIZE];
    zassert_ok(defy_lighting_control_encode(&state, DEFY_CONTROL_LIGHTING_SET, command));
    zassert_equal(write_control(NULL, NULL, command, sizeof(command) - 1, 0,
                               BT_GATT_WRITE_FLAG_CMD),
                  BT_GATT_ERR(BT_ATT_ERR_INVALID_ATTRIBUTE_LEN));
    zassert_equal(write_control(NULL, NULL, command, sizeof(command), 1,
                               BT_GATT_WRITE_FLAG_CMD),
                  BT_GATT_ERR(BT_ATT_ERR_INVALID_OFFSET));
    zassert_equal(write_control(NULL, NULL, command, sizeof(command), 0, 0),
                  BT_GATT_ERR(BT_ATT_ERR_NOT_SUPPORTED));
    const uint8_t offsets[] = {0, 1, 2, 3, 7};
    const uint8_t values[] = {1, DEFY_CONTROL_LIGHTING_ACK, 2, 0, 0};
    for (size_t i = 0; i < ARRAY_SIZE(offsets); i++) {
        zassert_ok(defy_lighting_control_encode(&state, DEFY_CONTROL_LIGHTING_SET, command));
        command[offsets[i]] = values[i];
        zassert_equal(write_control(NULL, NULL, command, sizeof(command), 0,
                                   BT_GATT_WRITE_FLAG_CMD),
                      BT_GATT_ERR(BT_ATT_ERR_VALUE_NOT_ALLOWED));
    }
    zassert_is_null(submitted_work);
    apply_lighting_work(NULL);
    zassert_equal(lighting_calls, 0);
}

ZTEST(control_peripheral, test_lighting_older_pending_revision_does_not_replace_latest) {
    control_connection.connected(NULL, 0);
    zassert_equal(request_lighting(123, 9, false), DEFY_LIGHTING_CONTROL_SIZE);
    now += 100;
    zassert_equal(request_lighting(123, 8, true), DEFY_LIGHTING_CONTROL_SIZE);
    apply_lighting_work(NULL);
    zassert_equal(lighting_calls, 1);
    zassert_equal(applied_lighting.revision, 9);
    zassert_false(applied_lighting.enabled);
    zassert_equal(request_lighting(123, 10, false), DEFY_LIGHTING_CONTROL_SIZE);
    zassert_equal(request_lighting(123, 11, true), DEFY_LIGHTING_CONTROL_SIZE);
    apply_lighting_work(NULL);
    zassert_equal(applied_lighting.revision, 11);
    zassert_true(applied_lighting.enabled);
}

ZTEST(control_peripheral, test_lighting_older_pending_revision_cannot_extend_deadline) {
    control_connection.connected(NULL, 0);
    zassert_equal(request_lighting(123, 9, false), DEFY_LIGHTING_CONTROL_SIZE);
    now = DEFY_CONTROL_TIMEOUT_MS - 1;
    zassert_equal(request_lighting(123, 8, true), DEFY_LIGHTING_CONTROL_SIZE);
    now++;
    apply_lighting_work(NULL);
    zassert_equal(lighting_calls, 0);
}

ZTEST(control_peripheral, test_lighting_disconnect_drops_queued_set_and_reconnect_replays) {
    control_connection.connected(NULL, 0);
    uint32_t old_epoch = active_lighting_epoch;
    zassert_equal(request_lighting(123, 1, false), DEFY_LIGHTING_CONTROL_SIZE);
    control_connection.disconnected(NULL, 0);
    zassert_equal(active_lighting_epoch, 0);
    apply_lighting_work(NULL);
    zassert_equal(lighting_calls, 0);
    control_connection.connected(NULL, 0);
    zassert_not_equal(active_lighting_epoch, old_epoch);
    zassert_equal(request_lighting(123, 1, false), DEFY_LIGHTING_CONTROL_SIZE);
    apply_lighting_work(NULL);
    zassert_equal(lighting_calls, 1);
    zassert_equal(applied_lighting_epoch, active_lighting_epoch);
}

ZTEST(control_peripheral, test_lighting_expired_set_does_not_apply) {
    control_connection.connected(NULL, 0);
    zassert_equal(request_lighting(123, 1, false), DEFY_LIGHTING_CONTROL_SIZE);
    now += DEFY_CONTROL_TIMEOUT_MS;
    apply_lighting_work(NULL);
    zassert_equal(lighting_calls, 0);
    zassert_false(lighting_pending);
}

ZTEST(control_peripheral, test_lighting_ack_reports_only_successful_applied_tuple) {
    uint8_t data[DEFY_LIGHTING_READ_SIZE];
    struct defy_lighting_state decoded;
    struct defy_lighting_wake wake;
    control_connection.connected(NULL, 0);
    zassert_equal(read_lighting(NULL, NULL, data, sizeof(data), 0), DEFY_LIGHTING_WAKE_SIZE);
    zassert_ok(defy_lighting_wake_decode(data, DEFY_LIGHTING_WAKE_SIZE, &wake));
    zassert_equal(request_lighting(123, 7, false), DEFY_LIGHTING_CONTROL_SIZE);
    zassert_equal(read_lighting(NULL, NULL, data, sizeof(data), 0), DEFY_LIGHTING_WAKE_SIZE);
    zassert_ok(defy_lighting_wake_decode(data, DEFY_LIGHTING_WAKE_SIZE, &wake));
    lighting_error = -ENODEV;
    apply_lighting_work(NULL);
    zassert_equal(read_lighting(NULL, NULL, data, sizeof(data), 0), DEFY_LIGHTING_WAKE_SIZE);
    zassert_ok(defy_lighting_wake_decode(data, DEFY_LIGHTING_WAKE_SIZE, &wake));
    lighting_error = 0;
    zassert_equal(request_lighting(123, 7, false), DEFY_LIGHTING_CONTROL_SIZE);
    apply_lighting_work(NULL);
    zassert_equal(read_lighting(NULL, NULL, data, sizeof(data), 0), DEFY_LIGHTING_CONTROL_SIZE);
    zassert_ok(defy_lighting_control_decode(data, DEFY_LIGHTING_CONTROL_SIZE,
                                            DEFY_CONTROL_LIGHTING_ACK, &decoded));
    zassert_true(defy_lighting_equal(&decoded, &applied_lighting));
}

ZTEST(control_peripheral, test_lighting_group_is_kept_at_once_without_reply) {
    const uint8_t command[] = {3, DEFY_CONTROL_LIGHTING_GROUP, 0x78, 0x56, 0x34, 0x12, 9,
                               0x10, 0x20, 0x30, 0x01, 0x02, 0x03, 0x04, 0x05};
    zassert_equal(sizeof(command), DEFY_LIGHTING_GROUP_SIZE);
    zassert_equal(write_control(NULL, NULL, command, sizeof(command), 0,
                               BT_GATT_WRITE_FLAG_CMD), sizeof(command));
    zassert_equal(group_calls, 1);
    zassert_is_null(submitted_work);
    zassert_equal(group_kept.boot, 0x12345678);
    zassert_equal(group_kept.index, 9);
    zassert_equal(group_kept.colour.r, 0x10);
    zassert_equal(group_kept.colour.g, 0x20);
    zassert_equal(group_kept.colour.b, 0x30);
    zassert_equal(group_kept.leds, 0x0504030201ull);
    /* One the lighting drops, from an earlier Neuron boot, has no reply either. */
    group_result = -ESTALE;
    zassert_equal(write_control(NULL, NULL, command, sizeof(command), 0,
                               BT_GATT_WRITE_FLAG_CMD), sizeof(command));
    zassert_equal(group_calls, 2);
    zassert_equal(lighting_calls, 0);
    zassert_false(lighting_pending);
}

ZTEST(control_peripheral, test_lighting_group_rejects_malformed_writes) {
    const struct defy_lighting_group group = {
        .boot = 5, .index = 15, .colour = {1, 2, 3}, .leds = BIT64(34),
    };
    uint8_t command[DEFY_LIGHTING_GROUP_SIZE + 1] = {0};
    zassert_ok(defy_lighting_group_encode(&group, command));
    zassert_equal(write_control(NULL, NULL, command, DEFY_LIGHTING_GROUP_SIZE - 1, 0,
                               BT_GATT_WRITE_FLAG_CMD),
                  BT_GATT_ERR(BT_ATT_ERR_INVALID_ATTRIBUTE_LEN));
    zassert_equal(write_control(NULL, NULL, command, DEFY_LIGHTING_GROUP_SIZE + 1, 0,
                               BT_GATT_WRITE_FLAG_CMD),
                  BT_GATT_ERR(BT_ATT_ERR_INVALID_ATTRIBUTE_LEN));
    /* Version 2, boot 0, index 16 and LED 35. */
    const struct { uint8_t offset; uint8_t value; } invalid[] = {
        {0, 2}, {2, 0}, {6, 16}, {14, 0x0c},
    };
    for (size_t i = 0; i < ARRAY_SIZE(invalid); i++) {
        zassert_ok(defy_lighting_group_encode(&group, command));
        command[invalid[i].offset] = invalid[i].value;
        zassert_equal(write_control(NULL, NULL, command, DEFY_LIGHTING_GROUP_SIZE, 0,
                                   BT_GATT_WRITE_FLAG_CMD),
                      BT_GATT_ERR(BT_ATT_ERR_VALUE_NOT_ALLOWED));
    }
    zassert_equal(group_calls, 0);
    zassert_is_null(submitted_work);
}

ZTEST(control_peripheral, test_lighting_state_refused_for_a_missing_group_is_not_acknowledged) {
    control_connection.connected(NULL, 0);
    zassert_equal(request_lighting(123, 1, true), DEFY_LIGHTING_CONTROL_SIZE);
    apply_lighting_work(NULL);
    struct defy_lighting_state state = {
        .boot = 123, .revision = 2, .enabled = true, .groups = BIT(3), .level = 200,
    };
    lighting_error = -EAGAIN;
    zassert_equal(request_state(&state), DEFY_LIGHTING_CONTROL_SIZE);
    apply_lighting_work(NULL);
    zassert_equal(lighting_calls, 2);
    uint8_t data[DEFY_LIGHTING_READ_SIZE];
    struct defy_lighting_state read;
    zassert_equal(read_lighting(NULL, NULL, data, sizeof(data), 0), DEFY_LIGHTING_CONTROL_SIZE);
    zassert_ok(defy_lighting_control_decode(data, DEFY_LIGHTING_CONTROL_SIZE,
                                            DEFY_CONTROL_LIGHTING_ACK, &read));
    zassert_equal(read.revision, 1);

    /* The Neuron sends the group, then the state again. */
    const struct defy_lighting_group group = {
        .boot = 123, .index = 3, .colour = {9, 8, 7}, .leds = BIT64(0),
    };
    uint8_t command[DEFY_LIGHTING_GROUP_SIZE];
    zassert_ok(defy_lighting_group_encode(&group, command));
    zassert_equal(write_control(NULL, NULL, command, sizeof(command), 0,
                               BT_GATT_WRITE_FLAG_CMD), sizeof(command));
    zassert_equal(group_calls, 1);
    lighting_error = 0;
    zassert_equal(request_state(&state), DEFY_LIGHTING_CONTROL_SIZE);
    apply_lighting_work(NULL);
    zassert_equal(read_lighting(NULL, NULL, data, sizeof(data), 0), DEFY_LIGHTING_CONTROL_SIZE);
    const uint8_t expected[] = {3, DEFY_CONTROL_LIGHTING_ACK, 1, 123, 0, 0, 0, 2, 0, 0, 0,
                                0, 0, 0, 0, 0, 0, 0x08, 0, 200};
    zassert_mem_equal(data, expected, sizeof(expected));
}

ZTEST(control_peripheral, test_lighting_set_and_battery_show_remain_independent) {
    control_connection.connected(NULL, 0);
    zassert_equal(request_lighting(123, 1, false), DEFY_LIGHTING_CONTROL_SIZE);
    zassert_equal(write_control(NULL, NULL, show_command, sizeof(show_command), 0,
                               BT_GATT_WRITE_FLAG_CMD), sizeof(show_command));
    zassert_true(lighting_pending);
    show_battery_work(NULL);
    zassert_equal(show_calls, 1);
    zassert_equal(lighting_calls, 0);
    apply_lighting_work(NULL);
    zassert_equal(lighting_calls, 1);
    zassert_false(applied_lighting.enabled);
    zassert_equal(show_calls, 1);
}

ZTEST(control_peripheral, test_wake_status_distinguishes_normal_boot_and_dormant_wake) {
    uint8_t data[DEFY_LIGHTING_WAKE_SIZE];
    struct defy_lighting_wake wake;
    now = 4321;
    zassert_equal(read_lighting(NULL, NULL, data, sizeof(data), 0), sizeof(data));
    zassert_ok(defy_lighting_wake_decode(data, sizeof(data), &wake));
    zassert_equal(wake.side, DEFY_HALF_ID);
    zassert_false(wake.from_sleep);
    zassert_equal(wake.boot, random_value);
    zassert_equal(wake.age_ms, now);
    woke_from_key = true;
    zassert_equal(read_lighting(NULL, NULL, data, sizeof(data), 0), sizeof(data));
    zassert_ok(defy_lighting_wake_decode(data, sizeof(data), &wake));
    zassert_true(wake.from_sleep);
    zassert_equal(random_calls, 1);
    zassert_equal(lighting_calls, 0);
    zassert_is_null(submitted_work);
}

ZTEST(control_peripheral, test_wake_boot_token_survives_radio_generation_and_reconnect) {
    struct defy_battery_frame frame;
    struct defy_lighting_wake wake;
    uint8_t data[DEFY_LIGHTING_WAKE_SIZE];
    woke_from_key = true;
    zassert_ok(defy_battery_local_frame(&frame));
    uint32_t original_session = frame.session;
    input_generation++;
    zassert_ok(defy_battery_local_frame(&frame));
    zassert_not_equal(frame.session, original_session);
    control_connection.disconnected(NULL, 0);
    control_connection.connected(NULL, 0);
    zassert_equal(read_lighting(NULL, NULL, data, sizeof(data), 0), sizeof(data));
    zassert_ok(defy_lighting_wake_decode(data, sizeof(data), &wake));
    zassert_equal(wake.boot, original_session);
    zassert_true(wake.from_sleep);
    zassert_equal(random_calls, 1);
}

ZTEST(control_peripheral, test_wake_status_needs_nonce_but_no_battery_sample) {
    uint8_t data[DEFY_LIGHTING_WAKE_SIZE];
    struct defy_lighting_wake wake;
    input_error = -ENODATA;
    nonce_ready = false;
    zassert_equal(read_lighting(NULL, NULL, data, sizeof(data), 0),
                  BT_GATT_ERR(BT_ATT_ERR_UNLIKELY));
    random_value = 0;
    initialize_nonce();
    zassert_equal(read_lighting(NULL, NULL, data, sizeof(data), 0), sizeof(data));
    zassert_ok(defy_lighting_wake_decode(data, sizeof(data), &wake));
    zassert_equal(wake.boot, 1);
}

ZTEST(control_peripheral, test_wake_age_saturates) {
    uint8_t data[DEFY_LIGHTING_WAKE_SIZE];
    struct defy_lighting_wake wake;
    now = (int64_t)UINT32_MAX + 1234;
    zassert_equal(read_lighting(NULL, NULL, data, sizeof(data), 0), sizeof(data));
    zassert_ok(defy_lighting_wake_decode(data, sizeof(data), &wake));
    zassert_equal(wake.age_ms, UINT32_MAX);
}

ZTEST(control_peripheral, test_wake_codec_wire_format_and_roundtrip) {
    const struct defy_lighting_wake wake = {
        .side = 1, .from_sleep = true, .boot = 0x12345678, .age_ms = 0x90abcdef,
    };
    const uint8_t expected[] = {3, 4, 1, 1, 0x78, 0x56, 0x34, 0x12,
                               0xef, 0xcd, 0xab, 0x90};
    uint8_t data[DEFY_LIGHTING_WAKE_SIZE];
    struct defy_lighting_wake decoded;
    zassert_ok(defy_lighting_wake_encode(&wake, data));
    zassert_mem_equal(data, expected, sizeof(expected));
    zassert_ok(defy_lighting_wake_decode(expected, sizeof(expected), &decoded));
    zassert_equal(decoded.side, wake.side);
    zassert_equal(decoded.from_sleep, wake.from_sleep);
    zassert_equal(decoded.boot, wake.boot);
    zassert_equal(decoded.age_ms, wake.age_ms);
}

ZTEST(control_peripheral, test_wake_codec_rejects_malformed_payload_without_output) {
    const struct defy_lighting_wake wake = {.side = 0, .boot = 7};
    uint8_t data[DEFY_LIGHTING_WAKE_SIZE + 1];
    const struct { uint8_t offset; uint8_t value; } invalid[] = {
        {0, 2}, {0, 4}, {1, DEFY_CONTROL_LIGHTING_ACK}, {2, 2}, {3, 2}, {4, 0},
    };
    struct defy_lighting_wake decoded = {.side = 1, .boot = 99};
    for (size_t i = 0; i < ARRAY_SIZE(invalid); i++) {
        zassert_ok(defy_lighting_wake_encode(&wake, data));
        data[invalid[i].offset] = invalid[i].value;
        zassert_equal(defy_lighting_wake_decode(data, DEFY_LIGHTING_WAKE_SIZE, &decoded),
                      -EPROTO);
        zassert_equal(decoded.boot, 99);
        zassert_equal(decoded.side, 1);
    }
    zassert_ok(defy_lighting_wake_encode(&wake, data));
    zassert_equal(defy_lighting_wake_decode(data, DEFY_LIGHTING_WAKE_SIZE - 1, &decoded),
                  -EINVAL);
    zassert_equal(defy_lighting_wake_decode(data, DEFY_LIGHTING_WAKE_SIZE + 1, &decoded),
                  -EINVAL);
    zassert_equal(defy_lighting_wake_decode(NULL, DEFY_LIGHTING_WAKE_SIZE, &decoded), -EINVAL);
    zassert_equal(defy_lighting_wake_decode(data, DEFY_LIGHTING_WAKE_SIZE, NULL), -EINVAL);
    struct defy_lighting_wake invalid_wake = {.side = 2, .boot = 7};
    zassert_equal(defy_lighting_wake_encode(&invalid_wake, data), -EINVAL);
    invalid_wake.side = 0;
    invalid_wake.boot = 0;
    zassert_equal(defy_lighting_wake_encode(&invalid_wake, data), -EINVAL);
    zassert_equal(defy_lighting_wake_encode(NULL, data), -EINVAL);
    zassert_equal(defy_lighting_wake_encode(&wake, NULL), -EINVAL);
}

static bool uuid_is(const struct bt_uuid *uuid, const struct bt_uuid_128 *expected) {
    return uuid->type == BT_UUID_TYPE_128 &&
           memcmp(BT_UUID_128(uuid)->val, expected->val, sizeof(expected->val)) == 0;
}

ZTEST(control_peripheral, test_version_characteristic_follows_command_read_only_encrypted) {
    static const struct bt_uuid_128 battery = BT_UUID_INIT_128(DEFY_CONTROL_BATTERY_UUID);
    static const struct bt_uuid_128 command = BT_UUID_INIT_128(DEFY_CONTROL_COMMAND_UUID);
    static const struct bt_uuid_128 version = BT_UUID_INIT_128(DEFY_HALF_VERSION_UUID);
    const struct bt_gatt_attr *attrs = defy_control_service_attrs;

    /* The service, then the battery's, the command's and the version's
     * declaration and value: the Neuron looks right after the command's. */
    zassert_equal(ARRAY_SIZE(defy_control_service_attrs), 7);
    const struct bt_gatt_chrc *declarations[] = {
        attrs[1].user_data, attrs[3].user_data, attrs[5].user_data,
    };
    zassert_true(uuid_is(declarations[0]->uuid, &battery));
    zassert_true(uuid_is(declarations[1]->uuid, &command));
    zassert_true(uuid_is(declarations[2]->uuid, &version));
    zassert_equal(declarations[2]->properties, BT_GATT_CHRC_READ);
    zassert_true(uuid_is(attrs[6].uuid, &version));
    zassert_equal(attrs[6].perm, BT_GATT_PERM_READ_ENCRYPT);
    zassert_equal_ptr(attrs[6].read, read_version);
    zassert_is_null(attrs[6].write);

    uint8_t data[DEFY_HALF_VERSION_SIZE + 4];
    struct defy_half_version decoded;
    local_version_calls = 0;
    zassert_equal(attrs[6].read(NULL, &attrs[6], data, sizeof(data), 0),
                  DEFY_HALF_VERSION_SIZE);
    zassert_equal(local_version_calls, 1);
    zassert_true(defy_half_version_decode(data, &decoded));
    zassert_equal(decoded.side, DEFY_WIRED_RIGHT);
    zassert_equal(decoded.program_crc, 0xc0ffee11);
    zassert_equal(decoded.seal_version, 7);
    zassert_str_equal(decoded.module, local_version.module);
}

ZTEST_SUITE(control_peripheral, NULL, NULL, before, NULL, NULL);

static void request_feedback(uint32_t sequence, uint16_t duration) {
    struct defy_host_feedback state = {
        .boot = 123, .sequence = sequence, .remaining_ms = duration,
        .profile = 2, .flags = DEFY_HOST_PREFERRED_BLE,
    };
    uint8_t data[DEFY_HOST_FEEDBACK_SIZE];
    zassert_ok(defy_host_feedback_encode(&state, data));
    zassert_equal(write_control(NULL, NULL, data, sizeof(data), 0, BT_GATT_WRITE_FLAG_CMD),
                  sizeof(data));
}

ZTEST(control_peripheral, test_feedback_accounts_for_queue_delay_and_does_not_change_lighting) {
    request_feedback(1, 2000);
    zassert_equal(submitted_work, &show_feedback);
    now = 750;
    show_feedback_work(&show_feedback);
    zassert_equal(feedback_calls, 1);
    zassert_equal(feedback_applied.remaining_ms, 1250);
    zassert_equal(feedback_applied.profile, 2);
    zassert_equal(lighting_calls, 0);
    zassert_equal(show_calls, 0);
    show_feedback_work(&show_feedback);
    zassert_equal(feedback_calls, 1);
}

ZTEST(control_peripheral, test_feedback_duplicate_and_older_queue_do_not_extend_deadline) {
    request_feedback(2, 2000);
    now = 500;
    request_feedback(2, 2000);
    request_feedback(1, 2000);
    zassert_equal(feedback_deadline, 2000);
    show_feedback_work(&show_feedback);
    zassert_equal(feedback_calls, 1);
    zassert_equal(feedback_applied.sequence, 2);
    zassert_equal(feedback_applied.remaining_ms, 1500);
}

ZTEST(control_peripheral, test_expired_and_disconnected_feedback_are_dropped) {
    request_feedback(1, 2000);
    now = 2000;
    show_feedback_work(&show_feedback);
    zassert_equal(feedback_calls, 0);
    request_feedback(2, 2000);
    disconnected(NULL, 0);
    show_feedback_work(&show_feedback);
    zassert_equal(feedback_calls, 0);
}

ZTEST(control_peripheral, test_feedback_rejects_malformed_control_without_work) {
    uint8_t data[DEFY_HOST_FEEDBACK_SIZE] = {1, DEFY_CONTROL_HOST_FEEDBACK};
    zassert_equal(write_control(NULL, NULL, data, sizeof(data), 0, BT_GATT_WRITE_FLAG_CMD),
                  BT_GATT_ERR(BT_ATT_ERR_VALUE_NOT_ALLOWED));
    zassert_is_null(submitted_work);
    zassert_equal(feedback_calls, 0);
}

ZTEST(control_peripheral, test_feedback_takes_ten_profiles_and_its_own_length_alone) {
    struct defy_host_feedback state = {
        .boot = 123, .sequence = 1, .remaining_ms = 2000, .profile = 7,
        .flags = DEFY_HOST_PREFERRED_BLE | DEFY_HOST_CHOOSER, .profile_count = 10,
        .clears = 4, .paired_profiles = BIT(7) | BIT(9), .connected_profiles = BIT(9),
    };
    uint8_t data[DEFY_HOST_FEEDBACK_SIZE];
    zassert_ok(defy_host_feedback_encode(&state, data));
    zassert_equal(write_control(NULL, NULL, data, sizeof(data), 0, BT_GATT_WRITE_FLAG_CMD),
                  sizeof(data));
    show_feedback_work(&show_feedback);
    zassert_equal(feedback_calls, 1);
    zassert_equal(feedback_applied.profile, 7);
    zassert_equal(feedback_applied.profile_count, 10);
    zassert_equal(feedback_applied.clears, 4);
    zassert_equal(feedback_applied.paired_profiles, BIT(7) | BIT(9));
    zassert_equal(feedback_applied.connected_profiles, BIT(9));

    /* Any shorter write is refused, 14 and 17 bytes among them. */
    for (uint16_t length = DEFY_CONTROL_HEADER_SIZE; length < sizeof(data); length++) {
        zassert_equal(write_control(NULL, NULL, data, length, 0, BT_GATT_WRITE_FLAG_CMD),
                      BT_GATT_ERR(BT_ATT_ERR_INVALID_ATTRIBUTE_LEN));
    }
    zassert_equal(feedback_calls, 1);
}
