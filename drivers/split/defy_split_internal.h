/* SPDX-License-Identifier: MIT */

#ifndef DYGMA_DEFY_SPLIT_INTERNAL_H
#define DYGMA_DEFY_SPLIT_INTERNAL_H

/*
 * What the sources in drivers/split share among themselves. The part the rest
 * of the module uses is in <dygma/defy_hybrid.h>.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <dygma/defy_wired_protocol.h>

/* ZMK's position state notification (POSITION_STATE_DATA_LEN in
 * split/bluetooth/central.c): the key bitmap, then zeros. */
#define DEFY_HYBRID_BLE_FRAME_SIZE 16u

struct zmk_split_transport_central;
struct zmk_split_transport_peripheral;

/*
 * The module's split transports. ZMK selects the hybrid one for the role,
 * which drives the cable transport and ZMK's Bluetooth transport beneath it.
 * The cable central is not registered with ZMK; the cable peripheral is.
 */
extern struct zmk_split_transport_central defy_hybrid_central;
extern const struct zmk_split_transport_central defy_wired_central;
extern struct zmk_split_transport_peripheral defy_hybrid_peripheral;
extern struct zmk_split_transport_peripheral defy_wired_peripheral;

/*
 * A half's cable snapshot, or its loss when connected is false. -EINVAL for a
 * bad side, session 0 or a key of the other side; -ENOMSG when the queue is
 * full, and the cable then withholds its acknowledgement. During maintenance
 * it returns 0 and queues nothing.
 */
int defy_hybrid_central_wired_update(uint8_t side, uint32_t session, bool connected,
                                     const uint8_t bitmap[DEFY_WIRED_BITMAP_SIZE]);
/*
 * ZMK's position state from the half on a Bluetooth slot, or the link's loss
 * when connected is false. -EINVAL for a bad slot, epoch 0 or a frame not
 * DEFY_HYBRID_BLE_FRAME_SIZE long; -EPROTO for keys that no one side owns or
 * a nonzero pad; -ENOMSG when the queue is full, though the snapshot stays the
 * slot's latest and is still applied. During maintenance it returns 0 and
 * queues nothing.
 */
int defy_hybrid_central_ble_update(uint8_t slot, uint32_t epoch, bool connected,
                                   const uint8_t *frame, size_t length);
/* Out of the hybrid central: it rejected the half on this Bluetooth slot and
 * connection epoch. The half is disconnected and may reconnect only after a
 * hold-off that doubles with each rejection. */
void defy_hybrid_ble_peer_rejected(uint8_t slot, uint32_t epoch);
/* Ends both slots' hold-offs and starts their doubling again, for a split
 * pairing reset, after which each slot belongs to a new half. */
void defy_hybrid_ble_clear_hold_off(void);
/* Queues the disconnect of a half overdue for a key snapshot or rejected. */
void defy_hybrid_ble_check_health(int64_t now);
/* Each link's part of defy_hybrid_central_set_maintenance(): 0, or -EAGAIN
 * when its lock stays held for 500 ms. */
int defy_hybrid_ble_set_maintenance(bool value);
int defy_wired_central_set_maintenance(bool value);

/* The cable's part of defy_hybrid_peripheral_prepare_sleep() and
 * _cancel_sleep(). */
int defy_wired_peripheral_prepare_sleep(void);
void defy_wired_peripheral_cancel_sleep(void);

#endif
