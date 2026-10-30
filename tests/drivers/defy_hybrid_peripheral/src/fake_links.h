/* SPDX-License-Identifier: MIT */

#ifndef DEFY_HYBRID_TEST_LINKS_H
#define DEFY_HYBRID_TEST_LINKS_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

struct fake_link_snapshot {
    uint8_t cable_keys[9];
    uint8_t bluetooth_keys[16];
    uint8_t notified_keys[16];
    size_t cable_exchanges;
    size_t cable_updates;
    size_t bluetooth_reports;
    size_t snapshots;
    size_t concurrent_reports;
    unsigned int bluetooth_enable_calls;
    unsigned int bluetooth_status_reads;
    unsigned int zmk_connected_events;
    unsigned int zmk_disconnected_events;
    int64_t last_snapshot_ms;
    int64_t max_snapshot_gap_ms;
    bool bluetooth_enabled;
};

void fake_links_reset(void);
void fake_cable_online(bool value);
void fake_cable_pause_transfer(bool require_ack);
int fake_cable_await_pause(void);
void fake_cable_resume_transfer(void);
void fake_bluetooth_available(bool value);
void fake_bluetooth_connected(bool value);
void fake_bluetooth_slow_reports(bool value);
struct fake_link_snapshot fake_links_snapshot(void);

#if IS_ENABLED(CONFIG_DEFY_HALF_SLEEP)
#include <zephyr/bluetooth/conn.h>
void fake_bluetooth_delay_disconnect(bool value);
void fake_bluetooth_state(enum bt_conn_state state);
void fake_bluetooth_pause_stop(void);
int fake_bluetooth_await_stop(void);
void fake_bluetooth_resume_stop(void);
void fake_bluetooth_stop_error(int error);
bool fake_bluetooth_advertising_allowed(void);
unsigned int fake_bluetooth_stop_calls(void);
int fake_bluetooth_ref_count(void);
#endif

#endif
