/* SPDX-License-Identifier: MIT */

#ifndef DYGMA_DEFY_WIRED_PROTOCOL_H
#define DYGMA_DEFY_WIRED_PROTOCOL_H

/*
 * The cable protocol's codec and session state, in plain C for the host
 * tests. docs/automatic-transports.md describes the frames.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <dygma/defy_control.h>
#include <dygma/defy_half_version.h>
#include <dygma/defy_lighting.h>
#include <dygma/defy_wired_frame.h>

#define DEFY_WIRED_BITMAP_SIZE 9u
#define DEFY_WIRED_KEY_COUNT 70u
#define DEFY_WIRED_QUEUE_CAPACITY 32u
#define DEFY_WIRED_VERSION 2u

enum defy_wired_side { DEFY_WIRED_LEFT = 0, DEFY_WIRED_RIGHT = 1 };
#define DEFY_WIRED_SIDES 2u
/* A half whose side is not known yet. The key trace records it as this value. */
#define DEFY_WIRED_SIDE_UNKNOWN 2u

enum defy_wired_kind { DEFY_WIRED_DATA = 1, DEFY_WIRED_ACK = 2 };
enum defy_wired_flag { DEFY_WIRED_FLAG_SYNC = 1, DEFY_WIRED_FLAG_RESYNC = 2 };
enum defy_wired_accept_result {
    DEFY_WIRED_DUPLICATE = 0,
    DEFY_WIRED_UPDATED = 1,
    DEFY_WIRED_RESYNC = 2,
};

struct defy_wired_packet {
    uint8_t kind;
    uint8_t side;
    uint8_t flags;
    uint32_t session;
    uint32_t sequence;
    uint8_t bitmap[DEFY_WIRED_BITMAP_SIZE];
};

struct defy_wired_snapshot {
    uint32_t sequence;
    uint8_t bitmap[DEFY_WIRED_BITMAP_SIZE];
};

struct defy_wired_tx {
    uint8_t side;
    uint32_t session;
    uint32_t next_sequence;
    uint8_t bitmap[DEFY_WIRED_BITMAP_SIZE];
    struct defy_wired_snapshot queue[DEFY_WIRED_QUEUE_CAPACITY];
    struct defy_wired_snapshot acknowledged;
    size_t head;
    size_t count;
    bool has_acknowledged;
};

struct defy_wired_rx {
    bool connected;
    uint8_t side;
    uint32_t session;
    uint32_t sequence;
    uint8_t bitmap[DEFY_WIRED_BITMAP_SIZE];
};

bool defy_wired_position_owned(uint8_t side, uint16_t position);
bool defy_wired_bitmap_valid(uint8_t side, const uint8_t bitmap[DEFY_WIRED_BITMAP_SIZE]);
bool defy_wired_bitmap_empty(const uint8_t bitmap[DEFY_WIRED_BITMAP_SIZE]);
int defy_wired_decode(const uint8_t *frame, size_t length, struct defy_wired_packet *packet);
void defy_wired_encode(const struct defy_wired_packet *packet,
                       uint8_t frame[DEFY_WIRED_FRAME_SIZE]);

/*
 * A 'DL' frame carries a control channel message, as dygma/defy_control.h
 * describes them, to or from the half on side, in the cable session it is in:
 *
 *    2  version
 *    3  opcode
 *    4  side
 *    5  the lighting state's enabled flag, 0 in other messages
 *    6  0, 0
 *    8  session, 32 bits
 *   12  the message's own fields, then zeros up to the CRC
 *
 * defy_wired_control_check() checks what every 'DL' frame shares, once, and
 * the decoder for its opcode the rest.
 */
#define DEFY_WIRED_CONTROL_PAYLOAD_OFFSET 12u
#define DEFY_WIRED_CONTROL_PAYLOAD_SIZE (DEFY_WIRED_CRC_OFFSET - DEFY_WIRED_CONTROL_PAYLOAD_OFFSET)

struct defy_wired_control {
    uint8_t version;
    uint8_t opcode;
    uint8_t side;
    uint8_t flag;
    uint32_t session;
};

/* Zeroes the frame and writes control's header. The caller adds the message's
 * own fields and seals the frame. */
void defy_wired_control_put(const struct defy_wired_control *control,
                            uint8_t frame[DEFY_WIRED_FRAME_SIZE]);
/*
 * Reads a 'DL' frame's header: -EINVAL for a wrong length, -EBADMSG for a bad
 * CRC, and -EPROTO for another kind of frame, a side other than left or right,
 * session 0 or a byte 6 or 7 other than 0.
 */
int defy_wired_control_check(const uint8_t *frame, size_t length,
                             struct defy_wired_control *control);

/* The battery display, DEFY_CONTROL_BATTERY_SHOW, for the half on side. */
struct defy_wired_battery_show_command {
    uint8_t side;
    uint32_t session;
};

struct defy_wired_lighting_command {
    /* DEFY_CONTROL_LIGHTING_SET from the Neuron, DEFY_CONTROL_LIGHTING_ACK from a half. */
    uint8_t opcode;
    uint8_t side;
    uint32_t session;
    struct defy_lighting_state state;
};

/* DEFY_CONTROL_LIGHTING_GROUP from the Neuron, for the half on side. */
struct defy_wired_lighting_group_command {
    uint8_t side;
    uint32_t session;
    struct defy_lighting_group group;
};

/*
 * Each decoder reads the rest of a frame whose header control is, as
 * defy_wired_control_check() read it, and returns -EPROTO unless the frame is
 * a valid message of its kind.
 */
int defy_wired_battery_show_encode(const struct defy_wired_battery_show_command *command,
                                   uint8_t frame[DEFY_WIRED_FRAME_SIZE]);
int defy_wired_battery_show_decode(const uint8_t *frame, const struct defy_wired_control *control,
                                   struct defy_wired_battery_show_command *command);
int defy_wired_lighting_encode(const struct defy_wired_lighting_command *command,
                               uint8_t frame[DEFY_WIRED_FRAME_SIZE]);
int defy_wired_lighting_decode(const uint8_t *frame, const struct defy_wired_control *control,
                               struct defy_wired_lighting_command *command);
int defy_wired_lighting_group_encode(const struct defy_wired_lighting_group_command *command,
                                     uint8_t frame[DEFY_WIRED_FRAME_SIZE]);
int defy_wired_lighting_group_decode(const uint8_t *frame, const struct defy_wired_control *control,
                                     struct defy_wired_lighting_group_command *command);

/*
 * A 'DV' frame carries a half's version report, the 28-byte record that
 * dygma/defy_half_version.h encodes and decodes, in bytes 2..29, in place of
 * an idle keep-alive. The decoder returns the record as it came: -EINVAL for
 * a wrong length, -EBADMSG for a bad CRC and -EPROTO for another kind of
 * frame.
 */
#define DEFY_WIRED_HALF_VERSION_OFFSET 2u

void defy_wired_half_version_encode(const uint8_t record[DEFY_HALF_VERSION_SIZE],
                                    uint8_t frame[DEFY_WIRED_FRAME_SIZE]);
int defy_wired_half_version_decode(const uint8_t *frame, size_t length,
                                   uint8_t record[DEFY_HALF_VERSION_SIZE]);

/* Starts session (0 becomes 1) with every key up. */
void defy_wired_tx_init(struct defy_wired_tx *tx, uint8_t side, uint32_t session);
/*
 * Queues a snapshot with one key changed, or nothing when the key already is
 * in that state. -EINVAL for a position the side does not own. -ENOSPC when
 * the queue was full: a new session then starts with the current keys, so
 * only the history is lost.
 */
int defy_wired_tx_set_key(struct defy_wired_tx *tx, uint16_t position, bool pressed);
/* Starts a new session with the current keys. */
void defy_wired_tx_resync(struct defy_wired_tx *tx);
/* Encodes the oldest unacknowledged snapshot, else the last one acknowledged. */
void defy_wired_tx_frame(const struct defy_wired_tx *tx,
                         uint8_t frame[DEFY_WIRED_FRAME_SIZE]);
/*
 * Whether ack, in this session, acknowledges the oldest pending snapshot,
 * which then leaves the queue, or the last one acknowledged. A resync request
 * also returns true, after starting a new session.
 */
bool defy_wired_tx_ack(struct defy_wired_tx *tx, const struct defy_wired_packet *ack);

/* No half connected. */
void defy_wired_rx_init(struct defy_wired_rx *rx);
/*
 * Takes a data packet and writes the acknowledgement to send: a
 * defy_wired_accept_result, with rx unchanged and ack asking for a new
 * session on DEFY_WIRED_RESYNC, or -EINVAL for anything but a valid data
 * packet.
 */
int defy_wired_rx_accept(struct defy_wired_rx *rx, const struct defy_wired_packet *packet,
                         struct defy_wired_packet *ack);

#endif
