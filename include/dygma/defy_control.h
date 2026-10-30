/* SPDX-License-Identifier: MIT */

#ifndef DYGMA_DEFY_CONTROL_H
#define DYGMA_DEFY_CONTROL_H

/*
 * The Neuron's control channel to each half: the battery display, the
 * lighting state and its acknowledgement, the half's wake report and the
 * host display. Over Bluetooth it is a GATT service on each half, which also
 * serves the half's battery frame and version report. Over the cable the same
 * messages travel in 'DL' frames. Every message starts with its version and
 * opcode: the first two bytes of a Bluetooth payload, bytes 2 and 3 of a 'DL'
 * frame. Plain C, so the host tests build it without Zephyr.
 */

#include <stdint.h>

/* The control service and its characteristics share one base, ending in 0xb000 + n. */
#define DEFY_CONTROL_UUID(n) \
    BT_UUID_128_ENCODE(0x69a897d6, 0x87cf, 0x4e08, 0x9eb2, 0x973bbf71b000 + (n))
#define DEFY_CONTROL_SERVICE_UUID DEFY_CONTROL_UUID(0x00)
/* The half's battery frame, read only. */
#define DEFY_CONTROL_BATTERY_UUID DEFY_CONTROL_UUID(0x01)
/* The Neuron's commands, written without response. A read returns the half's
 * lighting acknowledgement, or its wake report before it has a lighting state. */
#define DEFY_CONTROL_COMMAND_UUID DEFY_CONTROL_UUID(0x02)
/* The half's version report, dygma/defy_half_version.h, read only, right after the
 * command characteristic. */
#define DEFY_HALF_VERSION_UUID DEFY_CONTROL_UUID(0x03)
/* The Neuron's battery diagnostics for each half, by enum defy_wired_side. */
#define DEFY_CONTROL_DIAGNOSTIC_UUID(side) DEFY_CONTROL_UUID(0x10 + (side))

/* The two links between the Neuron and a half. Each is a bit of the battery
 * snapshot's route mask, BIT(link). */
enum defy_link {
    DEFY_LINK_WIRED = 0,
    DEFY_LINK_BLE = 1,
};

enum defy_control_opcode {
    /* Show the half's battery on its keys for five seconds: the battery display. */
    DEFY_CONTROL_BATTERY_SHOW = 1,
    /* The Neuron's lighting state, and the half's acknowledgement of it. */
    DEFY_CONTROL_LIGHTING_SET = 2,
    DEFY_CONTROL_LIGHTING_ACK = 3,
    /* Whether the half woke from sleep by a key, and when. */
    DEFY_CONTROL_LIGHTING_WAKE = 4,
    /* The host overlay or the profile chooser. */
    DEFY_CONTROL_HOST_FEEDBACK = 5,
    /* A colour for some of the half's keys, which a lighting state can show. */
    DEFY_CONTROL_LIGHTING_GROUP = 6,
};

/* The version and opcode that start every message. */
#define DEFY_CONTROL_HEADER_SIZE 2u
/* The battery display's command is the header alone, at version 1. */
#define DEFY_CONTROL_BATTERY_SHOW_VERSION 1u
#define DEFY_CONTROL_BATTERY_SHOW_SIZE DEFY_CONTROL_HEADER_SIZE
/* How long a command waits to be sent or followed before it is dropped,
 * rather than followed late. */
#define DEFY_CONTROL_TIMEOUT_MS 1000u

struct bt_conn;
struct defy_battery_frame;
struct defy_host_feedback;

/*
 * The Neuron's end, over each link. Over Bluetooth it is the client of each
 * half's service, in battery/defy_control_ble.c: the hybrid central reports
 * the links, slot is the split slot and epoch tells one connection on it from
 * the next. Over the cable it is drivers/split/defy_wired_central.c.
 */
void defy_control_ble_connected(struct bt_conn *conn, uint8_t slot, uint32_t epoch);
void defy_control_ble_disconnected(struct bt_conn *conn, uint8_t slot, uint32_t epoch);

/* The battery display for the halves in side_mask, bit 0 the left, on the halves whose side
 * the link knows. */
void defy_control_ble_battery_show(uint8_t side_mask);
/* The battery display for the half on side: -ENOTCONN when it has no live cable link. */
int defy_wired_battery_show(uint8_t side);

/* The Neuron's lighting state changed: sends it to every half. */
void defy_control_ble_lighting_changed(void);
void defy_wired_lighting_changed(void);

void defy_control_ble_host_feedback_show(const struct defy_host_feedback *state);
void defy_wired_host_feedback_show(const struct defy_host_feedback *state);

/*
 * A half's end, in battery/defy_control_peripheral.c: its battery frame,
 * which the service serves and the cable sends too, under the same session.
 */
int defy_battery_local_frame(struct defy_battery_frame *frame);

#endif
