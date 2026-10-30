/* SPDX-License-Identifier: MIT */

#include <errno.h>

#include <zephyr/init.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/atomic.h>
#include <zephyr/ztest.h>

#include <zmk/split/transport/peripheral.h>
#include <dygma/defy_hybrid.h>
#include <dygma/defy_startup.h>
#include <dygma/defy_wired_protocol.h>

#include "defy_split_internal.h"
#include "fake_links.h"

static const struct zmk_split_transport_peripheral *active_transport;
static bool unavailable_at_core_init;
static bool selected_before_scan;
static bool boot_held_key_reported;
static atomic_t connected_events;
static atomic_t disconnected_events;
static atomic_t status_callbacks;

static int select_transport(void) {
    STRUCT_SECTION_FOREACH(zmk_split_transport_peripheral, transport) {
        if (!transport->api->get_status || transport->api->get_status().available) {
            if (active_transport == transport) {
                return 0;
            }
            if (active_transport != NULL && active_transport->api->set_enabled != NULL) {
                active_transport->api->set_enabled(false);
            }
            active_transport = transport;
            return active_transport->api->set_enabled(true);
        }
    }
    return -ENODEV;
}

static int core_status_changed(const struct zmk_split_transport_peripheral *transport,
                                struct zmk_split_transport_status status) {
    atomic_inc(&status_callbacks);
    if (transport != active_transport ||
        status.connections == ZMK_SPLIT_TRANSPORT_CONNECTIONS_STATUS_DISCONNECTED) {
        return select_transport();
    }
    return 0;
}

static int install_core_callbacks(void) {
    STRUCT_SECTION_FOREACH(zmk_split_transport_peripheral, transport) {
        transport->api->set_status_callback(core_status_changed);
    }
    return select_transport();
}

static int core_init(void) {
    unavailable_at_core_init = install_core_callbacks() == -ENODEV;
    return 0;
}

SYS_INIT(core_init, APPLICATION, 40);

static int early_scan(void) {
    selected_before_scan = active_transport == &defy_hybrid_peripheral;
    if (selected_before_scan) {
        struct zmk_split_transport_peripheral_event event = {
            .type = ZMK_SPLIT_TRANSPORT_PERIPHERAL_EVENT_TYPE_KEY_POSITION_EVENT,
            .data.key_position_event = {.position = 28, .pressed = true},
        };

        boot_held_key_reported = active_transport->api->report_event(&event) == 0 &&
                                fake_links_snapshot().bluetooth_keys[3] == BIT(4);
    }
    return 0;
}

SYS_INIT(early_scan, APPLICATION, 85);

/* The start-up LEDs' connection state, which the hybrid sets from both links. */
void defy_startup_split_connected(bool connected) {
    zassert_true(defy_hybrid_peripheral.api->get_status().available);
    atomic_inc(connected ? &connected_events : &disconnected_events);
}

static int key_event(uint8_t position, bool pressed) {
    struct zmk_split_transport_peripheral_event event = {
        .type = ZMK_SPLIT_TRANSPORT_PERIPHERAL_EVENT_TYPE_KEY_POSITION_EVENT,
        .data.key_position_event = {.position = position, .pressed = pressed},
    };

    return active_transport->api->report_event(&event);
}

static void await_boolean(bool (*condition)(void), bool expected) {
    int64_t deadline = k_uptime_get() + 350;

    while (condition() != expected && k_uptime_get() < deadline) {
        k_sleep(K_MSEC(1));
    }
    zassert_equal(condition(), expected);
}

static void await_events(const atomic_t *events, int expected) {
    int64_t deadline = k_uptime_get() + 350;

    while (atomic_get(events) != expected && k_uptime_get() < deadline) {
        k_sleep(K_MSEC(1));
    }
    zassert_equal(atomic_get(events), expected);
}

static bool cable_connected(void) {
    return defy_wired_peripheral.api->get_status().connections ==
           ZMK_SPLIT_TRANSPORT_CONNECTIONS_STATUS_ALL_CONNECTED;
}

static bool hybrid_connected(void) {
    return defy_hybrid_peripheral.api->get_status().connections ==
           ZMK_SPLIT_TRANSPORT_CONNECTIONS_STATUS_ALL_CONNECTED;
}

static bool bluetooth_enabled(void) { return fake_links_snapshot().bluetooth_enabled; }

static bool cable_enabled(void) { return defy_wired_peripheral.api->get_status().enabled; }

static void connect_bluetooth(void) {
    fake_bluetooth_available(true);
    await_boolean(bluetooth_enabled, true);
    fake_bluetooth_connected(true);
    await_boolean(hybrid_connected, true);
    k_sleep(K_MSEC(25));
}

static void before(void *fixture) {
    ARG_UNUSED(fixture);
    defy_hybrid_peripheral_cancel_sleep();
    zassert_ok(defy_hybrid_peripheral.api->set_enabled(true));
    for (uint8_t position = 0; position < DEFY_WIRED_KEY_COUNT; position++) {
        if (defy_wired_position_owned(DEFY_WIRED_LEFT, position)) {
            zassert_ok(key_event(position, false));
        }
    }
    zassert_ok(defy_hybrid_peripheral.api->set_enabled(false));
    await_boolean(cable_enabled, false);
    await_boolean(bluetooth_enabled, false);
    k_sleep(K_MSEC(10));
    fake_links_reset();
    atomic_clear(&connected_events);
    atomic_clear(&disconnected_events);
    atomic_clear(&status_callbacks);
    active_transport = NULL;
    zassert_ok(install_core_callbacks());
    await_boolean(cable_enabled, true);
    zassert_equal(active_transport, &defy_hybrid_peripheral);
    k_sleep(K_MSEC(CONFIG_DEFY_WIRED_PERIPHERAL_TIMEOUT_MS + 1));
}

ZTEST(defy_hybrid_peripheral, test_core_selects_mux_before_boot_held_key_scan) {
    zassert_true(unavailable_at_core_init);
    zassert_true(selected_before_scan);
    zassert_true(boot_held_key_reported);
    size_t count = 0;

    STRUCT_SECTION_FOREACH(zmk_split_transport_peripheral, transport) {
        if (count++ == 0) {
            zassert_equal(transport, &defy_hybrid_peripheral);
        }
    }
    zassert_equal(count, 3);
}

ZTEST(defy_hybrid_peripheral, test_delayed_bluetooth_availability_starts_both_backends) {
    k_sleep(K_MSEC(30));
    zassert_true(fake_links_snapshot().cable_exchanges > 0);
    zassert_false(bluetooth_enabled());
    fake_bluetooth_available(true);
    await_boolean(bluetooth_enabled, true);
    zassert_true(cable_enabled());
    zassert_equal(active_transport, &defy_hybrid_peripheral);
    k_sleep(K_MSEC(100));
    zassert_equal(fake_links_snapshot().bluetooth_enable_calls, 1);
}

ZTEST(defy_hybrid_peripheral, test_cable_reports_keys_when_bluetooth_never_initializes) {
    fake_cable_online(true);
    await_boolean(cable_connected, true);
    zassert_true(hybrid_connected());
    zassert_false(bluetooth_enabled());
    zassert_ok(key_event(28, true));
    k_sleep(K_MSEC(10));
    zassert_equal(fake_links_snapshot().cable_keys[3], BIT(4));
    zassert_ok(key_event(28, false));
    k_sleep(K_MSEC(10));
    zassert_equal(fake_links_snapshot().cable_keys[3], 0);
    zassert_equal(fake_links_snapshot().bluetooth_enable_calls, 0);
    zassert_equal(atomic_get(&connected_events), 1);
    zassert_equal(fake_links_snapshot().zmk_connected_events, 0);
}

ZTEST(defy_hybrid_peripheral, test_cable_requires_ack_and_keeps_bluetooth_state_current) {
    connect_bluetooth();
    zassert_false(cable_connected());
    fake_cable_online(true);
    await_boolean(cable_connected, true);
    zassert_ok(key_event(0, true));
    zassert_ok(key_event(14, true));
    zassert_ok(key_event(0, false));
    k_sleep(K_MSEC(25));
    struct fake_link_snapshot state = fake_links_snapshot();

    zassert_equal(state.cable_keys[0], 0);
    zassert_equal(state.cable_keys[1], BIT(6));
    zassert_mem_equal(state.cable_keys, state.bluetooth_keys, 9);
    zassert_mem_equal(state.bluetooth_keys, state.notified_keys, 16);
    for (size_t i = 9; i < 16; i++) {
        zassert_equal(state.bluetooth_keys[i], 0);
    }
    zassert_true(state.bluetooth_enabled);
}

ZTEST(defy_hybrid_peripheral, test_startup_leds_follow_either_link_and_zmk_sees_bluetooth) {
    fake_cable_online(true);
    await_events(&connected_events, 1);
    fake_cable_online(false);
    await_events(&disconnected_events, 1);
    connect_bluetooth();
    await_events(&connected_events, 2);
    fake_cable_online(true);
    await_boolean(cable_connected, true);
    fake_bluetooth_connected(false);
    k_sleep(K_MSEC(30));
    zassert_equal(atomic_get(&disconnected_events), 1);
    fake_cable_online(false);
    await_events(&disconnected_events, 2);
    zassert_equal(atomic_get(&connected_events), 2);
    struct fake_link_snapshot state = fake_links_snapshot();

    zassert_equal(state.zmk_connected_events, 1);
    zassert_equal(state.zmk_disconnected_events, 1);
}

ZTEST(defy_hybrid_peripheral, test_cable_loss_keeps_held_and_released_bluetooth_keys_accurate) {
    connect_bluetooth();
    fake_cable_online(true);
    await_boolean(cable_connected, true);
    zassert_ok(key_event(28, true));
    fake_cable_online(false);
    await_boolean(cable_connected, false);
    zassert_true(hybrid_connected());
    zassert_equal(atomic_get(&disconnected_events), 0);
    zassert_equal(fake_links_snapshot().bluetooth_keys[3], BIT(4));
    zassert_ok(key_event(28, false));
    zassert_equal(fake_links_snapshot().bluetooth_keys[3], 0);
    fake_cable_online(true);
    await_boolean(cable_connected, true);
    zassert_equal(fake_links_snapshot().cable_keys[3], 0);
}

ZTEST(defy_hybrid_peripheral, test_reconnect_refreshes_held_state_without_new_key_event) {
    zassert_ok(key_event(28, true));
    zassert_equal(fake_links_snapshot().snapshots, 0);
    connect_bluetooth();
    k_sleep(K_MSEC(CONFIG_DEFY_HYBRID_SNAPSHOT_INTERVAL_MS + 30));
    struct fake_link_snapshot state = fake_links_snapshot();

    zassert_true(state.snapshots > 0);
    zassert_equal(state.notified_keys[3], BIT(4));
    size_t reports = state.bluetooth_reports;
    size_t snapshots = state.snapshots;

    k_sleep(K_MSEC(CONFIG_DEFY_HYBRID_SNAPSHOT_INTERVAL_MS + 30));
    state = fake_links_snapshot();
    zassert_equal(state.bluetooth_reports, reports);
    zassert_true(state.snapshots > snapshots);
}

ZTEST(defy_hybrid_peripheral, test_snapshot_refresh_serializes_with_key_reports) {
    connect_bluetooth();
    fake_bluetooth_slow_reports(true);
    for (size_t i = 0; i < 60; i++) {
        zassert_ok(key_event(14, i % 2 == 0));
        k_sleep(K_MSEC(2));
    }
    struct fake_link_snapshot state = fake_links_snapshot();

    zassert_equal(state.concurrent_reports, 0);
    zassert_equal(state.bluetooth_keys[1], 0);
    zassert_true(state.snapshots > state.bluetooth_reports);
}

ZTEST(defy_hybrid_peripheral, test_foreign_keys_never_reach_either_backend) {
    zassert_equal(key_event(7, true), -EINVAL);
    zassert_equal(key_event(70, true), -EINVAL);
    zassert_equal(key_event(120, true), -EINVAL);
    zassert_equal(fake_links_snapshot().bluetooth_reports, 0);
}

static void wait_for_idle(void) {
    k_sleep(K_MSEC(CONFIG_DEFY_HYBRID_IDLE_TIMEOUT_MS +
                   CONFIG_DEFY_HYBRID_IDLE_STATUS_POLL_MS + 20));
}

static unsigned int status_reads_during(int milliseconds) {
    unsigned int before = fake_links_snapshot().bluetooth_status_reads;

    k_sleep(K_MSEC(milliseconds));
    return fake_links_snapshot().bluetooth_status_reads - before;
}

static void await_status_update(unsigned int before) {
    int64_t deadline = k_uptime_get() + 10;

    while (fake_links_snapshot().bluetooth_status_reads == before && k_uptime_get() < deadline) {
        k_sleep(K_MSEC(1));
    }
    zassert_true(fake_links_snapshot().bluetooth_status_reads > before,
                 "idle worker did not wake promptly");
}

ZTEST(defy_hybrid_peripheral, test_idle_reduces_status_work_without_repeating_events) {
    connect_bluetooth();
    unsigned int active_reads = status_reads_during(300);

    wait_for_idle();
    unsigned int callbacks = atomic_get(&status_callbacks);
    unsigned int connections = atomic_get(&connected_events);
    unsigned int idle_reads = status_reads_during(300);

    zassert_true(active_reads >= 30, "active reads: %u", active_reads);
    zassert_true(idle_reads <= 15, "idle reads: %u", idle_reads);
    zassert_equal(atomic_get(&status_callbacks), callbacks);
    zassert_equal(atomic_get(&connected_events), connections);
    zassert_equal(atomic_get(&disconnected_events), 0);
}

ZTEST(defy_hybrid_peripheral, test_first_short_idle_tap_reports_and_wakes_worker) {
    connect_bluetooth();
    wait_for_idle();
    unsigned int reads = fake_links_snapshot().bluetooth_status_reads;
    size_t reports = fake_links_snapshot().bluetooth_reports;

    zassert_ok(key_event(28, true));
    zassert_equal(fake_links_snapshot().notified_keys[3], BIT(4));
    zassert_ok(key_event(28, false));
    zassert_equal(fake_links_snapshot().notified_keys[3], 0);
    zassert_equal(fake_links_snapshot().bluetooth_reports, reports + 2);
    await_status_update(reads);
    zassert_true(status_reads_during(100) >= 12);
}

ZTEST(defy_hybrid_peripheral, test_held_key_prevents_idle_until_release_timeout) {
    connect_bluetooth();
    zassert_ok(key_event(28, true));
    wait_for_idle();
    zassert_true(status_reads_during(200) >= 24);
    zassert_ok(key_event(28, false));
    zassert_true(status_reads_during(200) >= 24);
    wait_for_idle();
    zassert_true(status_reads_during(200) <= 12);
}

ZTEST(defy_hybrid_peripheral, test_idle_preserves_snapshot_deadlines) {
    connect_bluetooth();
    wait_for_idle();
    size_t snapshots = fake_links_snapshot().snapshots;

    k_sleep(K_MSEC(5 * CONFIG_DEFY_HYBRID_SNAPSHOT_INTERVAL_MS + 10));
    struct fake_link_snapshot state = fake_links_snapshot();

    zassert_true(state.snapshots - snapshots >= 5);
    zassert_true(state.max_snapshot_gap_ms <= CONFIG_DEFY_HYBRID_SNAPSHOT_INTERVAL_MS + 5,
                 "snapshot gap: %lld", state.max_snapshot_gap_ms);
}

ZTEST(defy_hybrid_peripheral, test_idle_backend_callbacks_remain_prompt) {
    wait_for_idle();
    int64_t started = k_uptime_get();

    fake_bluetooth_available(true);
    await_boolean(bluetooth_enabled, true);
    zassert_true(k_uptime_get() - started <= 10);
    fake_bluetooth_connected(true);
    int64_t deadline = k_uptime_get() + 10;

    while (atomic_get(&connected_events) == 0 && k_uptime_get() < deadline) {
        k_sleep(K_MSEC(1));
    }
    zassert_equal(atomic_get(&connected_events), 1);
    fake_bluetooth_connected(false);
    deadline = k_uptime_get() + 10;
    while (atomic_get(&disconnected_events) == 0 && k_uptime_get() < deadline) {
        k_sleep(K_MSEC(1));
    }
    zassert_equal(atomic_get(&disconnected_events), 1);
}

ZTEST(defy_hybrid_peripheral, test_idle_disable_stops_work_and_reenable_is_active) {
    connect_bluetooth();
    wait_for_idle();
    zassert_ok(defy_hybrid_peripheral.api->set_enabled(false));
    await_boolean(bluetooth_enabled, false);
    await_boolean(cable_enabled, false);
    k_sleep(K_MSEC(10));
    zassert_equal(status_reads_during(200), 0);
    zassert_equal(key_event(28, true), -EACCES);
    zassert_ok(defy_hybrid_peripheral.api->set_enabled(true));
    await_boolean(bluetooth_enabled, true);
    zassert_true(status_reads_during(100) >= 12);
}

ZTEST(defy_hybrid_peripheral, test_reenable_preserves_held_key_idle_guard) {
    connect_bluetooth();
    zassert_ok(key_event(28, true));
    zassert_ok(defy_hybrid_peripheral.api->set_enabled(false));
    await_boolean(bluetooth_enabled, false);
    zassert_ok(defy_hybrid_peripheral.api->set_enabled(true));
    await_boolean(bluetooth_enabled, true);
    wait_for_idle();
    zassert_true(status_reads_during(200) >= 24);
    zassert_equal(fake_links_snapshot().bluetooth_keys[3], BIT(4));
}

ZTEST(defy_hybrid_peripheral, test_duplicate_release_does_not_exit_idle) {
    connect_bluetooth();
    wait_for_idle();
    zassert_ok(key_event(28, false));
    zassert_true(status_reads_during(200) <= 12);
}

static K_SEM_DEFINE(prepared, 0, 1);
static int64_t requested_idle_ms;
static int prepare_result;

static void prepare_from_work(struct k_work *work) {
    ARG_UNUSED(work);
    prepare_result = defy_hybrid_peripheral_prepare_sleep(requested_idle_ms);
    k_sem_give(&prepared);
}
static K_WORK_DEFINE(prepare_work, prepare_from_work);

static void start_prepare(int64_t min_idle_ms) {
    k_sem_reset(&prepared);
    requested_idle_ms = min_idle_ms;
    zassert_true(k_work_submit(&prepare_work) >= 0);
}

static int await_prepare(void) {
    zassert_ok(k_sem_take(&prepared, K_MSEC(350)));
    return prepare_result;
}

static int prepare_sleep(int64_t min_idle_ms) {
    start_prepare(min_idle_ms);
    return await_prepare();
}

ZTEST(defy_hybrid_peripheral, test_sleep_requires_unheld_keys_and_transition_idle) {
    connect_bluetooth();
    zassert_equal(prepare_sleep(-1), -EINVAL);
    zassert_ok(key_event(28, true));
    zassert_equal(prepare_sleep(0), -EBUSY);
    zassert_equal(fake_links_snapshot().notified_keys[3], BIT(4));
    zassert_ok(key_event(28, false));
    zassert_equal(prepare_sleep(50), -EBUSY);
    k_sleep(K_MSEC(55));
    zassert_ok(prepare_sleep(50));
    zassert_true(cable_enabled());
    zassert_true(bluetooth_enabled());
    defy_hybrid_peripheral_cancel_sleep();
    zassert_ok(key_event(28, true));
    zassert_equal(fake_links_snapshot().notified_keys[3], BIT(4));
    zassert_ok(key_event(28, false));
}

ZTEST(defy_hybrid_peripheral, test_sleep_stops_io_and_gates_backend_callbacks) {
    connect_bluetooth();
    zassert_ok(prepare_sleep(0));
    struct fake_link_snapshot before = fake_links_snapshot();

    fake_bluetooth_connected(false);
    fake_bluetooth_available(false);
    unsigned int reads = fake_links_snapshot().bluetooth_status_reads;

    k_sleep(K_MSEC(150));
    struct fake_link_snapshot after = fake_links_snapshot();

    zassert_equal(after.cable_exchanges, before.cable_exchanges);
    zassert_equal(after.snapshots, before.snapshots);
    zassert_equal(after.bluetooth_enable_calls, before.bluetooth_enable_calls);
    zassert_equal(after.bluetooth_status_reads, reads);
    defy_hybrid_peripheral_cancel_sleep();
    await_boolean(bluetooth_enabled, false);
    k_sleep(K_MSEC(30));
    zassert_true(fake_links_snapshot().cable_exchanges > before.cable_exchanges);
    fake_bluetooth_available(true);
    await_boolean(bluetooth_enabled, true);
}

ZTEST(defy_hybrid_peripheral, test_live_cable_veto_preserves_key_delivery) {
    connect_bluetooth();
    fake_cable_online(true);
    await_boolean(cable_connected, true);
    zassert_equal(prepare_sleep(0), -EBUSY);
    zassert_true(cable_connected());
    size_t updates = fake_links_snapshot().cable_updates;

    zassert_ok(key_event(14, true));
    zassert_ok(key_event(14, false));
    k_sleep(K_MSEC(20));
    struct fake_link_snapshot state = fake_links_snapshot();

    zassert_equal(state.cable_updates, updates + 2);
    zassert_equal(state.cable_keys[1], 0);
    zassert_equal(state.notified_keys[1], 0);
}

ZTEST(defy_hybrid_peripheral, test_inflight_ack_vetoes_sleep_before_connection_publication) {
    fake_cable_pause_transfer(true);
    fake_cable_online(true);
    zassert_ok(fake_cable_await_pause());
    zassert_false(cable_connected());
    start_prepare(0);
    k_sleep(K_MSEC(5));
    zassert_equal(k_sem_count_get(&prepared), 0);
    fake_cable_resume_transfer();
    zassert_equal(await_prepare(), -EBUSY);
    zassert_true(cable_connected());
    zassert_ok(key_event(28, true));
    k_sleep(K_MSEC(10));
    zassert_equal(fake_links_snapshot().cable_keys[3], BIT(4));
    zassert_ok(key_event(28, false));
}

ZTEST(defy_hybrid_peripheral, test_inflight_absent_transfer_finishes_before_sleep_success) {
    fake_cable_pause_transfer(false);
    zassert_ok(fake_cable_await_pause());
    start_prepare(0);
    k_sleep(K_MSEC(5));
    zassert_equal(k_sem_count_get(&prepared), 0);
    fake_cable_resume_transfer();
    zassert_ok(await_prepare());
    size_t exchanges = fake_links_snapshot().cable_exchanges;

    k_sleep(K_MSEC(150));
    zassert_equal(fake_links_snapshot().cable_exchanges, exchanges);
    defy_hybrid_peripheral_cancel_sleep();
    k_sleep(K_MSEC(30));
    zassert_true(fake_links_snapshot().cable_exchanges > exchanges);
}

ZTEST(defy_hybrid_peripheral, test_sleep_barrier_timeout_restores_both_transports) {
    connect_bluetooth();
    fake_cable_pause_transfer(false);
    zassert_ok(fake_cable_await_pause());
    int64_t started = k_uptime_get();

    zassert_equal(prepare_sleep(0), -ETIMEDOUT);
    zassert_true(k_uptime_get() - started >= 100);
    zassert_true(k_uptime_get() - started < 150);
    fake_cable_resume_transfer();
    size_t exchanges = fake_links_snapshot().cable_exchanges;

    zassert_ok(key_event(28, true));
    zassert_equal(fake_links_snapshot().notified_keys[3], BIT(4));
    zassert_ok(key_event(28, false));
    k_sleep(K_MSEC(30));
    zassert_true(fake_links_snapshot().cable_exchanges > exchanges);
}

ZTEST(defy_hybrid_peripheral, test_cancel_preserves_disabled_lifecycle) {
    connect_bluetooth();
    zassert_ok(prepare_sleep(0));
    zassert_ok(defy_hybrid_peripheral.api->set_enabled(false));
    defy_hybrid_peripheral_cancel_sleep();
    await_boolean(cable_enabled, false);
    await_boolean(bluetooth_enabled, false);
    defy_hybrid_peripheral_cancel_sleep();
    zassert_false(defy_hybrid_peripheral.api->get_status().enabled);
    zassert_equal(key_event(28, true), -EACCES);
    zassert_ok(defy_hybrid_peripheral.api->set_enabled(true));
    await_boolean(bluetooth_enabled, true);
    await_boolean(cable_enabled, true);
    zassert_ok(key_event(28, true));
    zassert_ok(key_event(28, false));
}

ZTEST_SUITE(defy_hybrid_peripheral, NULL, NULL, before, NULL, NULL);
