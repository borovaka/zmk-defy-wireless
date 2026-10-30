/* SPDX-License-Identifier: MIT */

#include <dygma/defy_wired_protocol.h>

#include <errno.h>
#include <string.h>

/* The header that 'DW' and 'DL' frames share. The kind is a 'DW' frame's
 * enum defy_wired_kind and a 'DL' frame's opcode; byte 5 holds a 'DW' frame's
 * flags and a 'DL' frame's flag. */
#define HEADER_VERSION_OFFSET 2u
#define HEADER_KIND_OFFSET 3u
#define HEADER_SIDE_OFFSET 4u
#define HEADER_FLAGS_OFFSET 5u
#define HEADER_SESSION_OFFSET 8u

/* After the header, a 'DW' frame holds the bitmap's length (0 in an
 * acknowledgement), a zero byte, and after the session its sequence and key
 * bitmap. Bytes from the end of the bitmap up to the CRC are reserved and zero. */
#define KEY_LENGTH_OFFSET 6u
#define KEY_PAD_OFFSET 7u
#define KEY_SEQUENCE_OFFSET 12u
#define KEY_BITMAP_OFFSET 16u
#define KEY_RESERVED_OFFSET (KEY_BITMAP_OFFSET + DEFY_WIRED_BITMAP_SIZE)

_Static_assert(DEFY_WIRED_BITMAP_SIZE * 8u >= DEFY_WIRED_KEY_COUNT,
               "the bitmap has a bit for every key position");
_Static_assert(KEY_RESERVED_OFFSET <= DEFY_WIRED_CRC_OFFSET, "the bitmap ends before the CRC");

/* A 'DL' frame's two zero bytes, as dygma/defy_wired_protocol.h lays it out. */
#define CONTROL_RESERVED_OFFSET 6u

_Static_assert(HEADER_SESSION_OFFSET + 4u == DEFY_WIRED_CONTROL_PAYLOAD_OFFSET,
               "the payload follows the session");

/* The lighting state's boot, revision, colours, groups and level, in the payload. */
#define LIGHTING_BOOT_OFFSET DEFY_WIRED_CONTROL_PAYLOAD_OFFSET
#define LIGHTING_REVISION_OFFSET (LIGHTING_BOOT_OFFSET + 4u)
#define LIGHTING_COLOURS_OFFSET (LIGHTING_REVISION_OFFSET + 4u)
#define LIGHTING_GROUPS_OFFSET (LIGHTING_COLOURS_OFFSET + DEFY_LIGHTING_COLOURS_SIZE)
#define LIGHTING_END_OFFSET (LIGHTING_GROUPS_OFFSET + DEFY_LIGHTING_GROUPS_FIELD_SIZE)

_Static_assert(LIGHTING_END_OFFSET <= DEFY_WIRED_CRC_OFFSET,
               "the lighting state ends before the CRC");

/* A group's boot, then its index, colour and LEDs, in the payload. */
#define GROUP_BOOT_OFFSET DEFY_WIRED_CONTROL_PAYLOAD_OFFSET
#define GROUP_FIELDS_OFFSET (GROUP_BOOT_OFFSET + 4u)
#define GROUP_END_OFFSET (GROUP_FIELDS_OFFSET + DEFY_LIGHTING_GROUP_FIELDS_SIZE)

_Static_assert(GROUP_END_OFFSET <= DEFY_WIRED_CRC_OFFSET, "a group ends before the CRC");

bool defy_wired_position_owned(uint8_t side, uint16_t position) {
    if (side > DEFY_WIRED_RIGHT || position >= DEFY_WIRED_KEY_COUNT) {
        return false;
    }
    bool left = position < 7 || (position >= 14 && position < 21) ||
                (position >= 28 && position < 35) || (position >= 42 && position < 48) ||
                (position >= 54 && position < 58) || (position >= 62 && position < 66);
    return left == (side == DEFY_WIRED_LEFT);
}

bool defy_wired_bitmap_valid(uint8_t side, const uint8_t bitmap[DEFY_WIRED_BITMAP_SIZE]) {
    if (side > DEFY_WIRED_RIGHT || bitmap == NULL) {
        return false;
    }
    for (uint16_t position = 0; position < DEFY_WIRED_BITMAP_SIZE * 8; position++) {
        if ((bitmap[position / 8] & (1u << (position % 8))) != 0 &&
            !defy_wired_position_owned(side, position)) {
            return false;
        }
    }
    return true;
}

bool defy_wired_bitmap_empty(const uint8_t bitmap[DEFY_WIRED_BITMAP_SIZE]) {
    for (size_t i = 0; i < DEFY_WIRED_BITMAP_SIZE; i++) {
        if (bitmap[i] != 0) {
            return false;
        }
    }
    return true;
}

/* Whether a frame holds zeros from offset up to its CRC. */
static bool zero_to_crc(const uint8_t *frame, size_t offset) {
    for (size_t i = offset; i < DEFY_WIRED_CRC_OFFSET; i++) {
        if (frame[i] != 0) {
            return false;
        }
    }
    return true;
}

static bool packet_valid(const struct defy_wired_packet *packet) {
    if (packet == NULL || packet->side > DEFY_WIRED_RIGHT || packet->session == 0) {
        return false;
    }
    if (packet->kind == DEFY_WIRED_DATA) {
        return packet->flags == (packet->sequence == 0 ? DEFY_WIRED_FLAG_SYNC : 0) &&
               defy_wired_bitmap_valid(packet->side, packet->bitmap);
    }
    return packet->kind == DEFY_WIRED_ACK &&
           (packet->flags == 0 || packet->flags == DEFY_WIRED_FLAG_RESYNC) &&
           defy_wired_bitmap_empty(packet->bitmap);
}

int defy_wired_decode(const uint8_t *frame, size_t length, struct defy_wired_packet *packet) {
    if (frame == NULL || packet == NULL || length != DEFY_WIRED_FRAME_SIZE) {
        return -EINVAL;
    }
    if (!defy_wired_frame_crc_ok(frame)) {
        return -EBADMSG;
    }
    uint8_t kind = frame[HEADER_KIND_OFFSET];

    if (!defy_wired_frame_is(frame, 'W') || frame[HEADER_VERSION_OFFSET] != DEFY_WIRED_VERSION ||
        frame[KEY_LENGTH_OFFSET] != (kind == DEFY_WIRED_DATA ? DEFY_WIRED_BITMAP_SIZE : 0) ||
        frame[KEY_PAD_OFFSET] != 0 || !zero_to_crc(frame, KEY_RESERVED_OFFSET)) {
        return -EPROTO;
    }
    struct defy_wired_packet decoded = {
        .kind = kind,
        .side = frame[HEADER_SIDE_OFFSET],
        .flags = frame[HEADER_FLAGS_OFFSET],
        .session = defy_wired_get32(frame + HEADER_SESSION_OFFSET),
        .sequence = defy_wired_get32(frame + KEY_SEQUENCE_OFFSET),
    };
    memcpy(decoded.bitmap, frame + KEY_BITMAP_OFFSET, sizeof(decoded.bitmap));
    if (!packet_valid(&decoded)) {
        return -EPROTO;
    }
    *packet = decoded;
    return 0;
}

void defy_wired_encode(const struct defy_wired_packet *packet,
                       uint8_t frame[DEFY_WIRED_FRAME_SIZE]) {
    defy_wired_frame_start(frame, 'W');
    frame[HEADER_VERSION_OFFSET] = DEFY_WIRED_VERSION;
    frame[HEADER_KIND_OFFSET] = packet->kind;
    frame[HEADER_SIDE_OFFSET] = packet->side;
    frame[HEADER_FLAGS_OFFSET] = packet->flags;
    frame[KEY_LENGTH_OFFSET] = packet->kind == DEFY_WIRED_DATA ? DEFY_WIRED_BITMAP_SIZE : 0;
    defy_wired_put32(frame + HEADER_SESSION_OFFSET, packet->session);
    defy_wired_put32(frame + KEY_SEQUENCE_OFFSET, packet->sequence);
    memcpy(frame + KEY_BITMAP_OFFSET, packet->bitmap, DEFY_WIRED_BITMAP_SIZE);
    defy_wired_frame_seal(frame);
}

void defy_wired_control_put(const struct defy_wired_control *control,
                            uint8_t frame[DEFY_WIRED_FRAME_SIZE]) {
    defy_wired_frame_start(frame, 'L');
    frame[HEADER_VERSION_OFFSET] = control->version;
    frame[HEADER_KIND_OFFSET] = control->opcode;
    frame[HEADER_SIDE_OFFSET] = control->side;
    frame[HEADER_FLAGS_OFFSET] = control->flag;
    defy_wired_put32(frame + HEADER_SESSION_OFFSET, control->session);
}

int defy_wired_control_check(const uint8_t *frame, size_t length,
                             struct defy_wired_control *control) {
    if (frame == NULL || control == NULL || length != DEFY_WIRED_FRAME_SIZE) {
        return -EINVAL;
    }
    if (!defy_wired_frame_crc_ok(frame)) {
        return -EBADMSG;
    }
    struct defy_wired_control header = {
        .version = frame[HEADER_VERSION_OFFSET],
        .opcode = frame[HEADER_KIND_OFFSET],
        .side = frame[HEADER_SIDE_OFFSET],
        .flag = frame[HEADER_FLAGS_OFFSET],
        .session = defy_wired_get32(frame + HEADER_SESSION_OFFSET),
    };
    if (!defy_wired_frame_is(frame, 'L') || header.side > DEFY_WIRED_RIGHT ||
        header.session == 0 || frame[CONTROL_RESERVED_OFFSET] != 0 ||
        frame[CONTROL_RESERVED_OFFSET + 1] != 0) {
        return -EPROTO;
    }
    *control = header;
    return 0;
}

int defy_wired_battery_show_encode(const struct defy_wired_battery_show_command *command,
                                   uint8_t frame[DEFY_WIRED_FRAME_SIZE]) {
    if (command == NULL || frame == NULL || command->side > DEFY_WIRED_RIGHT ||
        command->session == 0) {
        return -EINVAL;
    }
    const struct defy_wired_control control = {
        .version = DEFY_CONTROL_BATTERY_SHOW_VERSION,
        .opcode = DEFY_CONTROL_BATTERY_SHOW,
        .side = command->side,
        .session = command->session,
    };
    defy_wired_control_put(&control, frame);
    defy_wired_frame_seal(frame);
    return 0;
}

int defy_wired_battery_show_decode(const uint8_t *frame, const struct defy_wired_control *control,
                                   struct defy_wired_battery_show_command *command) {
    if (frame == NULL || control == NULL || command == NULL) {
        return -EINVAL;
    }
    if (control->version != DEFY_CONTROL_BATTERY_SHOW_VERSION ||
        control->opcode != DEFY_CONTROL_BATTERY_SHOW || control->flag != 0 ||
        !zero_to_crc(frame, DEFY_WIRED_CONTROL_PAYLOAD_OFFSET)) {
        return -EPROTO;
    }
    *command = (struct defy_wired_battery_show_command){
        .side = control->side,
        .session = control->session,
    };
    return 0;
}

static bool lighting_valid(const struct defy_wired_lighting_command *command) {
    return command != NULL &&
           (command->opcode == DEFY_CONTROL_LIGHTING_SET ||
            command->opcode == DEFY_CONTROL_LIGHTING_ACK) &&
           command->side <= DEFY_WIRED_RIGHT && command->session != 0 &&
           defy_lighting_state_valid(&command->state);
}

int defy_wired_lighting_encode(const struct defy_wired_lighting_command *command,
                               uint8_t frame[DEFY_WIRED_FRAME_SIZE]) {
    if (frame == NULL || !lighting_valid(command)) {
        return -EINVAL;
    }
    const struct defy_wired_control control = {
        .version = DEFY_LIGHTING_VERSION,
        .opcode = command->opcode,
        .side = command->side,
        .flag = command->state.enabled,
        .session = command->session,
    };
    defy_wired_control_put(&control, frame);
    defy_wired_put32(frame + LIGHTING_BOOT_OFFSET, command->state.boot);
    defy_wired_put32(frame + LIGHTING_REVISION_OFFSET, command->state.revision);
    defy_lighting_colours_put(&command->state, frame + LIGHTING_COLOURS_OFFSET);
    defy_lighting_groups_put(&command->state, frame + LIGHTING_GROUPS_OFFSET);
    defy_wired_frame_seal(frame);
    return 0;
}

int defy_wired_lighting_decode(const uint8_t *frame, const struct defy_wired_control *control,
                               struct defy_wired_lighting_command *command) {
    if (frame == NULL || control == NULL || command == NULL) {
        return -EINVAL;
    }
    if (control->version != DEFY_LIGHTING_VERSION || control->flag > 1 ||
        !zero_to_crc(frame, LIGHTING_END_OFFSET)) {
        return -EPROTO;
    }
    struct defy_wired_lighting_command decoded = {
        .opcode = control->opcode,
        .side = control->side,
        .session = control->session,
        .state = {.boot = defy_wired_get32(frame + LIGHTING_BOOT_OFFSET),
                  .revision = defy_wired_get32(frame + LIGHTING_REVISION_OFFSET),
                  .enabled = control->flag != 0},
    };
    defy_lighting_colours_get(frame + LIGHTING_COLOURS_OFFSET, &decoded.state);
    defy_lighting_groups_get(frame + LIGHTING_GROUPS_OFFSET, &decoded.state);
    if (!lighting_valid(&decoded)) {
        return -EPROTO;
    }
    *command = decoded;
    return 0;
}

static bool lighting_group_valid(const struct defy_wired_lighting_group_command *command) {
    return command != NULL && command->side <= DEFY_WIRED_RIGHT && command->session != 0 &&
           defy_lighting_group_valid(&command->group);
}

int defy_wired_lighting_group_encode(const struct defy_wired_lighting_group_command *command,
                                     uint8_t frame[DEFY_WIRED_FRAME_SIZE]) {
    if (frame == NULL || !lighting_group_valid(command)) {
        return -EINVAL;
    }
    const struct defy_wired_control control = {
        .version = DEFY_LIGHTING_VERSION,
        .opcode = DEFY_CONTROL_LIGHTING_GROUP,
        .side = command->side,
        .session = command->session,
    };
    defy_wired_control_put(&control, frame);
    defy_wired_put32(frame + GROUP_BOOT_OFFSET, command->group.boot);
    defy_lighting_group_fields_put(&command->group, frame + GROUP_FIELDS_OFFSET);
    defy_wired_frame_seal(frame);
    return 0;
}

int defy_wired_lighting_group_decode(const uint8_t *frame, const struct defy_wired_control *control,
                                     struct defy_wired_lighting_group_command *command) {
    if (frame == NULL || control == NULL || command == NULL) {
        return -EINVAL;
    }
    if (control->version != DEFY_LIGHTING_VERSION ||
        control->opcode != DEFY_CONTROL_LIGHTING_GROUP || control->flag != 0 ||
        !zero_to_crc(frame, GROUP_END_OFFSET)) {
        return -EPROTO;
    }
    struct defy_wired_lighting_group_command decoded = {
        .side = control->side,
        .session = control->session,
        .group = {.boot = defy_wired_get32(frame + GROUP_BOOT_OFFSET)},
    };
    defy_lighting_group_fields_get(frame + GROUP_FIELDS_OFFSET, &decoded.group);
    if (!lighting_group_valid(&decoded)) {
        return -EPROTO;
    }
    *command = decoded;
    return 0;
}

_Static_assert(DEFY_WIRED_HALF_VERSION_OFFSET + DEFY_HALF_VERSION_SIZE == DEFY_WIRED_CRC_OFFSET,
               "the version report fills a 'DV' frame up to its CRC");

void defy_wired_half_version_encode(const uint8_t record[DEFY_HALF_VERSION_SIZE],
                                    uint8_t frame[DEFY_WIRED_FRAME_SIZE]) {
    defy_wired_frame_start(frame, 'V');
    memcpy(frame + DEFY_WIRED_HALF_VERSION_OFFSET, record, DEFY_HALF_VERSION_SIZE);
    defy_wired_frame_seal(frame);
}

int defy_wired_half_version_decode(const uint8_t *frame, size_t length,
                                   uint8_t record[DEFY_HALF_VERSION_SIZE]) {
    if (frame == NULL || record == NULL || length != DEFY_WIRED_FRAME_SIZE) {
        return -EINVAL;
    }
    if (!defy_wired_frame_crc_ok(frame)) {
        return -EBADMSG;
    }
    if (!defy_wired_frame_is(frame, 'V')) {
        return -EPROTO;
    }
    memcpy(record, frame + DEFY_WIRED_HALF_VERSION_OFFSET, DEFY_HALF_VERSION_SIZE);
    return 0;
}

static void tx_start_session(struct defy_wired_tx *tx) {
    tx->head = 0;
    tx->count = 1;
    tx->next_sequence = 1;
    tx->has_acknowledged = false;
    tx->queue[0].sequence = 0;
    memcpy(tx->queue[0].bitmap, tx->bitmap, DEFY_WIRED_BITMAP_SIZE);
}

void defy_wired_tx_init(struct defy_wired_tx *tx, uint8_t side, uint32_t session) {
    memset(tx, 0, sizeof(*tx));
    tx->side = side;
    tx->session = session == 0 ? 1 : session;
    tx_start_session(tx);
}

void defy_wired_tx_resync(struct defy_wired_tx *tx) {
    tx->session++;
    if (tx->session == 0) {
        tx->session = 1;
    }
    tx_start_session(tx);
}

static int tx_update(struct defy_wired_tx *tx, const uint8_t bitmap[DEFY_WIRED_BITMAP_SIZE]) {
    if (!defy_wired_bitmap_valid(tx->side, bitmap)) {
        return -EINVAL;
    }
    if (memcmp(tx->bitmap, bitmap, DEFY_WIRED_BITMAP_SIZE) == 0) {
        return 0;
    }
    memcpy(tx->bitmap, bitmap, DEFY_WIRED_BITMAP_SIZE);
    if (tx->count == DEFY_WIRED_QUEUE_CAPACITY) {
        defy_wired_tx_resync(tx);
        return -ENOSPC;
    }
    if (tx->next_sequence == 0) {
        defy_wired_tx_resync(tx);
        return 0;
    }
    size_t tail = (tx->head + tx->count) % DEFY_WIRED_QUEUE_CAPACITY;
    tx->queue[tail].sequence = tx->next_sequence++;
    memcpy(tx->queue[tail].bitmap, bitmap, DEFY_WIRED_BITMAP_SIZE);
    tx->count++;
    return 0;
}

int defy_wired_tx_set_key(struct defy_wired_tx *tx, uint16_t position, bool pressed) {
    if (!defy_wired_position_owned(tx->side, position)) {
        return -EINVAL;
    }
    uint8_t bitmap[DEFY_WIRED_BITMAP_SIZE];
    memcpy(bitmap, tx->bitmap, sizeof(bitmap));
    if (pressed) {
        bitmap[position / 8] |= (uint8_t)(1u << (position % 8));
    } else {
        bitmap[position / 8] &= (uint8_t)~(1u << (position % 8));
    }
    return tx_update(tx, bitmap);
}

void defy_wired_tx_frame(const struct defy_wired_tx *tx,
                         uint8_t frame[DEFY_WIRED_FRAME_SIZE]) {
    const struct defy_wired_snapshot *snapshot =
        tx->count != 0 ? &tx->queue[tx->head] : &tx->acknowledged;
    struct defy_wired_packet packet = {
        .kind = DEFY_WIRED_DATA,
        .side = tx->side,
        .flags = snapshot->sequence == 0 ? DEFY_WIRED_FLAG_SYNC : 0,
        .session = tx->session,
        .sequence = snapshot->sequence,
    };
    memcpy(packet.bitmap, snapshot->bitmap, DEFY_WIRED_BITMAP_SIZE);
    defy_wired_encode(&packet, frame);
}

bool defy_wired_tx_ack(struct defy_wired_tx *tx, const struct defy_wired_packet *ack) {
    if (!packet_valid(ack) || ack->kind != DEFY_WIRED_ACK || ack->side != tx->side ||
        ack->session != tx->session) {
        return false;
    }
    if (ack->flags == DEFY_WIRED_FLAG_RESYNC) {
        defy_wired_tx_resync(tx);
        return true;
    }
    if (tx->count != 0 && ack->sequence == tx->queue[tx->head].sequence) {
        tx->acknowledged = tx->queue[tx->head];
        tx->has_acknowledged = true;
        tx->head = (tx->head + 1) % DEFY_WIRED_QUEUE_CAPACITY;
        tx->count--;
        return true;
    }
    return tx->has_acknowledged && ack->sequence == tx->acknowledged.sequence;
}

void defy_wired_rx_init(struct defy_wired_rx *rx) { memset(rx, 0, sizeof(*rx)); }

int defy_wired_rx_accept(struct defy_wired_rx *rx, const struct defy_wired_packet *packet,
                         struct defy_wired_packet *ack) {
    if (!packet_valid(packet) || packet->kind != DEFY_WIRED_DATA || ack == NULL) {
        return -EINVAL;
    }
    *ack = (struct defy_wired_packet){
        .kind = DEFY_WIRED_ACK,
        .side = packet->side,
        .session = packet->session,
        .sequence = packet->sequence,
    };
    bool same_session = rx->connected && rx->side == packet->side &&
                        rx->session == packet->session;
    if (same_session && packet->sequence == rx->sequence &&
        memcmp(packet->bitmap, rx->bitmap, DEFY_WIRED_BITMAP_SIZE) == 0) {
        return DEFY_WIRED_DUPLICATE;
    }
    bool new_session = !same_session && packet->flags == DEFY_WIRED_FLAG_SYNC;
    bool next = same_session && packet->sequence != 0 &&
                packet->sequence == rx->sequence + 1;
    if (!new_session && !next) {
        ack->flags = DEFY_WIRED_FLAG_RESYNC;
        return DEFY_WIRED_RESYNC;
    }
    rx->connected = true;
    rx->side = packet->side;
    rx->session = packet->session;
    rx->sequence = packet->sequence;
    memcpy(rx->bitmap, packet->bitmap, DEFY_WIRED_BITMAP_SIZE);
    return DEFY_WIRED_UPDATED;
}
