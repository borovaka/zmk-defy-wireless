/* SPDX-License-Identifier: MIT */

#ifndef DYGMA_DEFY_SPLIT_PERIPHERAL_H
#define DYGMA_DEFY_SPLIT_PERIPHERAL_H

/*
 * The parts of ZMK's Bluetooth split peripheral that the hybrid peripheral
 * uses. ZMK declares them in no public header, so each declaration names the
 * file under ZMK's app/src/split/bluetooth/ that defines it.
 */

#include <zephyr/kernel.h>

struct zmk_split_transport_peripheral;

/* peripheral.c, declared in its private peripheral.h: ZMK's Bluetooth split
 * peripheral transport. */
struct zmk_split_transport_peripheral *zmk_split_transport_peripheral_bt(void);

/* peripheral.c: the work item that starts advertising. */
extern struct k_work advertising_work;

/* service.c: queues a notification of every key position's current state. */
int send_position_state(void);

/* service.c: the work queue that sends the split service's notifications. */
extern struct k_work_q service_work_q;

#endif
