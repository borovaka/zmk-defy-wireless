/* SPDX-License-Identifier: MIT */

#ifndef DYGMA_DEFY_SPLIT_CENTRAL_H
#define DYGMA_DEFY_SPLIT_CENTRAL_H

/*
 * The hybrid central's ties to ZMK's Bluetooth split central,
 * app/src/split/bluetooth/central.c. The patched copy of that file
 * (drivers/split/zmk-split-central.patch) includes this header, so the
 * compiler checks these declarations against ZMK's definitions.
 */

#include <stdint.h>

#include <zephyr/bluetooth/conn.h>
#include <zephyr/bluetooth/gatt.h>
#include <zephyr/kernel.h>

#include <zmk/split/transport/central.h>

/* ZMK's own, which it declares in no header. */

/* The Bluetooth split central transport. */
extern struct zmk_split_transport_central bt_central;
/* The work queue that runs the split central's GATT work. */
extern struct k_work_q split_central_split_run_q;
/* The split slot of a connection, or -EINVAL. */
int peripheral_slot_index_for_conn(struct bt_conn *conn);

/* The hybrid central's, which the patched central.c calls. The first three
 * take the place of the ZMK or Zephyr function each one extends. */

/* bt_gatt_discover() that also records a split peer's discovery progress for
 * the split diagnostics (opcode 8) and learns its key position characteristic. */
int defy_hybrid_ble_discover(struct bt_conn *conn, struct bt_gatt_discover_params *params);

/* bt_gatt_subscribe() that routes a split peer's key position notifications
 * into the hybrid central and tracks the subscription. */
int defy_hybrid_ble_subscribe(struct bt_conn *conn, struct bt_gatt_subscribe_params *params);

/* zmk_split_transport_central_peripheral_event_handler() for an event from
 * bt_central's slot `source`: maps the slot to its side and hands the event on
 * as the hybrid central, which merges key positions itself. */
int defy_hybrid_central_peripheral_event(uint8_t source,
                                         struct zmk_split_transport_peripheral_event event);

/* Milliseconds before the half on a slot, whose identity the hybrid central
 * rejected, may reconnect: the scanner holds the slot off until then. */
int64_t defy_hybrid_ble_hold_off(uint8_t slot);

#endif
