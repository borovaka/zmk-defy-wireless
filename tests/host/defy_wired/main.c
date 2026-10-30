/* SPDX-License-Identifier: MIT */

#include <dygma/defy_wired_protocol.h>

#include <assert.h>
#include <errno.h>
#include <string.h>

/* The version report that 'DV' frames carry: its codec is plain C too. */
#include "../../../battery/defy_half_version_protocol.c"

static void frame_crc(uint8_t frame[DEFY_WIRED_FRAME_SIZE]) {
    uint16_t crc = defy_wired_crc16(frame, DEFY_WIRED_FRAME_SIZE - 2);
    frame[30] = (uint8_t)crc;
    frame[31] = (uint8_t)(crc >> 8);
}

/* A 'DL' frame's header, then the rest as the battery display or a lighting state. */
static int decode_battery_show(const uint8_t *frame, size_t length,
                               struct defy_wired_battery_show_command *command) {
    struct defy_wired_control control;
    int err = defy_wired_control_check(frame, length, &control);
    return err ? err : defy_wired_battery_show_decode(frame, &control, command);
}

static int decode_lighting(const uint8_t *frame, size_t length,
                           struct defy_wired_lighting_command *command) {
    struct defy_wired_control control;
    int err = defy_wired_control_check(frame, length, &control);
    return err ? err : defy_wired_lighting_decode(frame, &control, command);
}

static struct defy_wired_packet outgoing(struct defy_wired_tx *tx) {
    uint8_t frame[DEFY_WIRED_FRAME_SIZE];
    struct defy_wired_packet packet;
    defy_wired_tx_frame(tx, frame);
    assert(defy_wired_decode(frame, sizeof(frame), &packet) == 0);
    return packet;
}

static int exchange(struct defy_wired_tx *tx, struct defy_wired_rx *rx) {
    struct defy_wired_packet packet = outgoing(tx), ack;
    int result = defy_wired_rx_accept(rx, &packet, &ack);
    assert(result >= 0);
    assert(defy_wired_tx_ack(tx, &ack));
    return result;
}

static void test_frame_envelope_and_fixed_frames(void) {
    /* Fixed frames pin the wire format of each kind the codec writes. */
    static const uint8_t expected[][DEFY_WIRED_FRAME_SIZE] = {
        {0x44, 0x57, 0x02, 0x01, 0x01, 0x00, 0x09, 0x00, 0xef, 0xcd, 0xab, 0x89, 0x04, 0x03, 0x02,
         0x01, 0x80, 0x00, 0x00, 0x02, 0x00, 0x00, 0x00, 0x00, 0x3c, 0x00, 0x00, 0x00, 0x00, 0x00,
         0xa8, 0xa0},
        {0x44, 0x57, 0x02, 0x02, 0x00, 0x02, 0x00, 0x00, 0x07, 0x00, 0x00, 0x00, 0xfe, 0xff, 0xff,
         0xff, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
         0xae, 0x8c},
        {0x44, 0x4c, 0x01, 0x01, 0x01, 0x00, 0x00, 0x00, 0x44, 0x33, 0x22, 0x11, 0x00, 0x00, 0x00,
         0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
         0x8f, 0xf2},
        {0x44, 0x4c, 0x03, 0x02, 0x00, 0x01, 0x00, 0x00, 0xd4, 0xc3, 0xb2, 0xa1, 0x88, 0x77, 0x66,
         0x55, 0x09, 0x00, 0x00, 0x00, 0x11, 0x12, 0x13, 0x21, 0x22, 0x23, 0x01, 0x02, 0x40, 0x00,
         0xd2, 0xf2},
        {0x44, 0x4c, 0x03, 0x03, 0x01, 0x00, 0x00, 0x00, 0x03, 0x00, 0x00, 0x00, 0x04, 0x00, 0x00,
         0x00, 0x00, 0x00, 0x00, 0x80, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
         0xda, 0x92},
        {0x44, 0x4c, 0x03, 0x06, 0x01, 0x00, 0x00, 0x00, 0x0d, 0x0c, 0x0b, 0x0a, 0x04, 0x03, 0x02,
         0x01, 0x07, 0x31, 0x32, 0x33, 0x78, 0x56, 0x34, 0x12, 0x04, 0x00, 0x00, 0x00, 0x00, 0x00,
         0x1c, 0xdc},
    };
    uint8_t frames[6][DEFY_WIRED_FRAME_SIZE];
    struct defy_wired_packet data = {
        .kind = DEFY_WIRED_DATA, .side = DEFY_WIRED_RIGHT, .session = 0x89abcdef,
        .sequence = 0x01020304, .bitmap = {[0] = 0x80, [3] = 0x02, [8] = 0x3c},
    };
    defy_wired_encode(&data, frames[0]);
    struct defy_wired_packet resync = {
        .kind = DEFY_WIRED_ACK, .side = DEFY_WIRED_LEFT, .flags = DEFY_WIRED_FLAG_RESYNC,
        .session = 7, .sequence = 0xfffffffe,
    };
    defy_wired_encode(&resync, frames[1]);
    struct defy_wired_battery_show_command show = {.side = DEFY_WIRED_RIGHT,
                                                   .session = 0x11223344};
    assert(defy_wired_battery_show_encode(&show, frames[2]) == 0);
    struct defy_wired_lighting_command set = {
        .opcode = DEFY_CONTROL_LIGHTING_SET, .side = DEFY_WIRED_LEFT, .session = 0xa1b2c3d4,
        .state = {.boot = 0x55667788, .revision = 9, .enabled = true,
                  .keys = {0x11, 0x12, 0x13}, .underglow = {0x21, 0x22, 0x23},
                  .groups = 0x0201, .level = 0x40},
    };
    assert(defy_wired_lighting_encode(&set, frames[3]) == 0);
    struct defy_wired_lighting_command ack = {
        .opcode = DEFY_CONTROL_LIGHTING_ACK, .side = DEFY_WIRED_RIGHT, .session = 3,
        .state = {.boot = 4, .revision = 0x80000000},
    };
    assert(defy_wired_lighting_encode(&ack, frames[4]) == 0);
    struct defy_wired_lighting_group_command group = {
        .side = DEFY_WIRED_RIGHT, .session = 0x0a0b0c0d,
        .group = {.boot = 0x01020304, .index = 7, .colour = {0x31, 0x32, 0x33},
                  .leds = 0x412345678},
    };
    assert(defy_wired_lighting_group_encode(&group, frames[5]) == 0);
    for (size_t i = 0; i < sizeof(expected) / sizeof(expected[0]); i++) {
        assert(memcmp(frames[i], expected[i], DEFY_WIRED_FRAME_SIZE) == 0);
        assert(defy_wired_frame_crc_ok(frames[i]));
    }
}

static void test_codec_and_ownership(void) {
    assert(defy_wired_crc16((const uint8_t *)"123456789", 9) == 0x29b1);
    unsigned int counts[2] = {0};
    for (uint16_t position = 0; position < DEFY_WIRED_KEY_COUNT; position++) {
        bool left = defy_wired_position_owned(DEFY_WIRED_LEFT, position);
        bool right = defy_wired_position_owned(DEFY_WIRED_RIGHT, position);
        assert(left != right);
        counts[left ? 0 : 1]++;
    }
    assert(counts[0] == 35 && counts[1] == 35);
    assert(!defy_wired_position_owned(DEFY_WIRED_LEFT, 70));
    assert(!defy_wired_position_owned(2, 0));
    struct defy_wired_tx tx;
    defy_wired_tx_init(&tx, DEFY_WIRED_LEFT, 0x12345678);
    struct defy_wired_packet packet = outgoing(&tx);
    assert(packet.session == 0x12345678 && packet.sequence == 0);
    assert(packet.side == DEFY_WIRED_LEFT && packet.flags == DEFY_WIRED_FLAG_SYNC);
    uint8_t original[DEFY_WIRED_FRAME_SIZE], altered[DEFY_WIRED_FRAME_SIZE];
    defy_wired_encode(&packet, original);
    memcpy(altered, original, sizeof(altered));
    altered[2] = 1;
    frame_crc(altered);
    assert(defy_wired_decode(altered, sizeof(altered), &packet) == -EPROTO);
    for (size_t bit = 0; bit < DEFY_WIRED_FRAME_SIZE * 8; bit++) {
        memcpy(altered, original, sizeof(altered));
        altered[bit / 8] ^= (uint8_t)(1u << (bit % 8));
        assert(defy_wired_decode(altered, sizeof(altered), &packet) == -EBADMSG);
    }
    const size_t invalid_fields[] = {0, 1, 2, 3, 4, 5, 6, 7, 25, 26, 27, 28, 29};
    for (size_t i = 0; i < sizeof(invalid_fields) / sizeof(invalid_fields[0]); i++) {
        memcpy(altered, original, sizeof(altered));
        altered[invalid_fields[i]] = 0xff;
        frame_crc(altered);
        assert(defy_wired_decode(altered, sizeof(altered), &packet) == -EPROTO);
    }
    memcpy(altered, original, sizeof(altered));
    memset(altered + 8, 0, 4);
    frame_crc(altered);
    assert(defy_wired_decode(altered, sizeof(altered), &packet) == -EPROTO);
    for (uint16_t position = 0; position < 72; position++) {
        memcpy(altered, original, sizeof(altered));
        altered[16 + position / 8] |= (uint8_t)(1u << (position % 8));
        frame_crc(altered);
        int expected = defy_wired_position_owned(DEFY_WIRED_LEFT, position) ? 0 : -EPROTO;
        assert(defy_wired_decode(altered, sizeof(altered), &packet) == expected);
    }
    assert(defy_wired_decode(original, sizeof(original) - 1, &packet) == -EINVAL);
    assert(defy_wired_decode(original, sizeof(original) + 1, &packet) == -EINVAL);
    assert(defy_wired_decode(NULL, sizeof(original), &packet) == -EINVAL);
    assert(defy_wired_decode(original, sizeof(original), NULL) == -EINVAL);
    packet = (struct defy_wired_packet){.kind = DEFY_WIRED_ACK, .session = 1};
    defy_wired_encode(&packet, altered);
    assert(defy_wired_decode(altered, sizeof(altered), &packet) == 0);
    altered[16] = 1;
    frame_crc(altered);
    assert(defy_wired_decode(altered, sizeof(altered), &packet) == -EPROTO);
}

static void test_retained_transitions_and_duplicates(void) {
    struct defy_wired_tx tx;
    struct defy_wired_rx rx;
    defy_wired_tx_init(&tx, DEFY_WIRED_LEFT, 1);
    defy_wired_rx_init(&rx);
    assert(exchange(&tx, &rx) == DEFY_WIRED_UPDATED);
    assert(tx.count == 0 && rx.connected);
    assert(defy_wired_tx_set_key(&tx, 14, true) == 0);
    assert(defy_wired_tx_set_key(&tx, 14, true) == 0);
    assert(defy_wired_tx_set_key(&tx, 14, false) == 0);
    assert(tx.count == 2);
    struct defy_wired_packet down = outgoing(&tx), ack;
    assert(defy_wired_rx_accept(&rx, &down, &ack) == DEFY_WIRED_UPDATED);
    assert(rx.bitmap[1] == 0x40);
    for (size_t retry = 0; retry < 20; retry++) {
        struct defy_wired_packet repeated = outgoing(&tx);
        assert(repeated.sequence == down.sequence);
        assert(defy_wired_rx_accept(&rx, &repeated, &ack) == DEFY_WIRED_DUPLICATE);
    }
    assert(defy_wired_tx_ack(&tx, &ack));
    assert(tx.count == 1);
    assert(defy_wired_tx_ack(&tx, &ack));
    assert(tx.count == 1);
    assert(exchange(&tx, &rx) == DEFY_WIRED_UPDATED);
    assert(rx.bitmap[1] == 0 && tx.count == 0);
    for (size_t heartbeat = 0; heartbeat < 20; heartbeat++) {
        assert(exchange(&tx, &rx) == DEFY_WIRED_DUPLICATE);
    }
    assert(defy_wired_tx_set_key(&tx, 7, true) == -EINVAL);
    assert(defy_wired_tx_set_key(&tx, 70, true) == -EINVAL);
    assert(tx.count == 0);
}

static void test_every_key_and_swapped_sockets(void) {
    struct defy_wired_tx tx[2];
    struct defy_wired_rx rx[2];
    for (uint8_t side = 0; side < 2; side++) {
        defy_wired_tx_init(&tx[side], side, side + 1);
        defy_wired_rx_init(&rx[side]);
    }
    for (uint8_t side = 0; side < 2; side++) {
        struct defy_wired_rx *socket = &rx[1 - side];
        assert(exchange(&tx[side], socket) == DEFY_WIRED_UPDATED);
        assert(socket->side == side);
        for (uint16_t position = 0; position < DEFY_WIRED_KEY_COUNT; position++) {
            if (!defy_wired_position_owned(side, position)) {
                continue;
            }
            assert(defy_wired_tx_set_key(&tx[side], position, true) == 0);
            assert(exchange(&tx[side], socket) == DEFY_WIRED_UPDATED);
            assert(socket->bitmap[position / 8] == (1u << (position % 8)));
            assert(defy_wired_tx_set_key(&tx[side], position, false) == 0);
            assert(exchange(&tx[side], socket) == DEFY_WIRED_UPDATED);
            assert(socket->bitmap[position / 8] == 0);
        }
    }
}

static void test_disconnect_and_restarts(void) {
    struct defy_wired_tx tx;
    struct defy_wired_rx rx;
    defy_wired_tx_init(&tx, DEFY_WIRED_LEFT, 9);
    defy_wired_rx_init(&rx);
    assert(exchange(&tx, &rx) == DEFY_WIRED_UPDATED);
    assert(defy_wired_tx_set_key(&tx, 0, true) == 0);
    assert(exchange(&tx, &rx) == DEFY_WIRED_UPDATED);
    struct defy_wired_packet previous = outgoing(&tx), stale_ack;
    assert(defy_wired_rx_accept(&rx, &previous, &stale_ack) == DEFY_WIRED_DUPLICATE);
    assert(rx.bitmap[0] == 1);
    defy_wired_rx_init(&rx);
    assert(!rx.connected && rx.bitmap[0] == 0);
    assert(exchange(&tx, &rx) == DEFY_WIRED_RESYNC);
    assert(tx.session == 10 && !rx.connected);
    assert(!defy_wired_tx_ack(&tx, &stale_ack));
    assert(exchange(&tx, &rx) == DEFY_WIRED_UPDATED);
    assert(rx.bitmap[0] == 1);
    assert(defy_wired_tx_set_key(&tx, 0, false) == 0);
    assert(exchange(&tx, &rx) == DEFY_WIRED_UPDATED);
    assert(rx.bitmap[0] == 0);
    uint32_t same_session = tx.session;
    defy_wired_tx_init(&tx, DEFY_WIRED_LEFT, same_session);
    assert(exchange(&tx, &rx) == DEFY_WIRED_RESYNC);
    assert(tx.session == same_session + 1);
    assert(exchange(&tx, &rx) == DEFY_WIRED_UPDATED);
    assert(defy_wired_tx_set_key(&tx, 1, true) == 0);
    assert(exchange(&tx, &rx) == DEFY_WIRED_UPDATED);
    defy_wired_tx_init(&tx, DEFY_WIRED_LEFT, 1234);
    assert(exchange(&tx, &rx) == DEFY_WIRED_UPDATED);
    assert(rx.session == 1234 && rx.bitmap[0] == 0);

    assert(defy_wired_tx_set_key(&tx, 0, true) == 0);
    defy_wired_tx_resync(&tx);
    assert(exchange(&tx, &rx) == DEFY_WIRED_UPDATED);
    assert(rx.sequence == 0 && rx.bitmap[0] == 1);
    defy_wired_tx_init(&tx, DEFY_WIRED_LEFT, rx.session);
    assert(exchange(&tx, &rx) == DEFY_WIRED_RESYNC);
    assert(exchange(&tx, &rx) == DEFY_WIRED_UPDATED);
    assert(rx.bitmap[0] == 0);
}

static void test_bad_sequences_and_acks(void) {
    struct defy_wired_tx tx;
    struct defy_wired_rx rx;
    defy_wired_tx_init(&tx, DEFY_WIRED_RIGHT, 16);
    defy_wired_rx_init(&rx);
    assert(exchange(&tx, &rx) == DEFY_WIRED_UPDATED);
    struct defy_wired_packet data = outgoing(&tx), ack;
    data.sequence = 7;
    data.flags = 0;
    assert(defy_wired_rx_accept(&rx, &data, &ack) == DEFY_WIRED_RESYNC);
    assert(ack.flags == DEFY_WIRED_FLAG_RESYNC && rx.sequence == 0);
    struct defy_wired_packet wrong = ack;
    wrong.session++;
    assert(!defy_wired_tx_ack(&tx, &wrong));
    wrong = ack;
    wrong.side = DEFY_WIRED_LEFT;
    assert(!defy_wired_tx_ack(&tx, &wrong));
    assert(defy_wired_tx_ack(&tx, &ack));
    uint32_t session = tx.session;
    assert(!defy_wired_tx_ack(&tx, &ack));
    assert(tx.session == session);
    assert(exchange(&tx, &rx) == DEFY_WIRED_UPDATED);
    data = outgoing(&tx);
    assert(defy_wired_rx_accept(&rx, &data, &ack) == DEFY_WIRED_DUPLICATE);
    ack.sequence = 999;
    assert(!defy_wired_tx_ack(&tx, &ack));
    data.kind = DEFY_WIRED_ACK;
    assert(defy_wired_rx_accept(&rx, &data, &ack) == -EINVAL);
}

static void test_queue_overflow_and_wrap(void) {
    struct defy_wired_tx tx;
    struct defy_wired_rx rx;
    defy_wired_tx_init(&tx, DEFY_WIRED_LEFT, UINT32_MAX);
    defy_wired_rx_init(&rx);
    for (size_t i = 0; i < DEFY_WIRED_QUEUE_CAPACITY - 1; i++) {
        assert(defy_wired_tx_set_key(&tx, 0, i % 2 == 0) == 0);
    }
    assert(tx.count == DEFY_WIRED_QUEUE_CAPACITY && tx.bitmap[0] == 1);
    assert(defy_wired_tx_set_key(&tx, 0, false) == -ENOSPC);
    assert(tx.count == 1 && tx.session == 1 && tx.bitmap[0] == 0);
    assert(exchange(&tx, &rx) == DEFY_WIRED_UPDATED);
    tx.next_sequence = UINT32_MAX;
    rx.sequence = UINT32_MAX - 1;
    assert(defy_wired_tx_set_key(&tx, 0, true) == 0);
    assert(exchange(&tx, &rx) == DEFY_WIRED_UPDATED);
    assert(rx.sequence == UINT32_MAX);
    assert(defy_wired_tx_set_key(&tx, 0, false) == 0);
    assert(tx.session == 2 && tx.count == 1);
    assert(exchange(&tx, &rx) == DEFY_WIRED_UPDATED);
    assert(rx.sequence == 0 && rx.bitmap[0] == 0);
}

static void test_transactional_receive(void) {
    struct defy_wired_tx tx;
    struct defy_wired_rx committed;
    defy_wired_tx_init(&tx, DEFY_WIRED_LEFT, 1);
    defy_wired_rx_init(&committed);
    assert(exchange(&tx, &committed) == DEFY_WIRED_UPDATED);
    assert(defy_wired_tx_set_key(&tx, 0, true) == 0);
    struct defy_wired_packet data = outgoing(&tx), ack;
    struct defy_wired_rx candidate = committed;
    assert(defy_wired_rx_accept(&candidate, &data, &ack) == DEFY_WIRED_UPDATED);
    assert(committed.bitmap[0] == 0 && candidate.bitmap[0] == 1);
    candidate = committed;
    assert(defy_wired_rx_accept(&candidate, &data, &ack) == DEFY_WIRED_UPDATED);
    committed = candidate;
    assert(defy_wired_tx_ack(&tx, &ack));
    assert(exchange(&tx, &committed) == DEFY_WIRED_DUPLICATE);
}

static uint32_t random_next(uint32_t *seed) {
    *seed = *seed * 1664525u + 1013904223u;
    return *seed;
}

static void test_pipelined_loss_and_corruption(void) {
    struct defy_wired_tx tx;
    struct defy_wired_rx rx;
    defy_wired_tx_init(&tx, DEFY_WIRED_LEFT, 21);
    defy_wired_rx_init(&rx);
    uint8_t preloaded[DEFY_WIRED_FRAME_SIZE] = {0};
    uint8_t expected[128][DEFY_WIRED_BITMAP_SIZE] = {{0}};
    size_t expected_count = 1, received_count = 0;
    uint32_t seed = 1;
    for (size_t step = 0; step < 4000; step++) {
        if (step < 2000 && step % 17 == 0) {
            assert(defy_wired_tx_set_key(&tx, 0, (expected_count % 2) != 0) == 0);
            memcpy(expected[expected_count++], tx.bitmap, DEFY_WIRED_BITMAP_SIZE);
        }
        uint8_t master_tx[DEFY_WIRED_FRAME_SIZE], master_rx[DEFY_WIRED_FRAME_SIZE];
        defy_wired_tx_frame(&tx, master_tx);
        memcpy(master_rx, preloaded, sizeof(master_rx));
        if (random_next(&seed) % 5 == 0) {
            master_tx[random_next(&seed) % sizeof(master_tx)] ^= 0x80;
        }
        if (random_next(&seed) % 4 == 0) {
            master_rx[random_next(&seed) % sizeof(master_rx)] ^= 0x01;
        }
        struct defy_wired_packet data, ack;
        if (defy_wired_decode(master_tx, sizeof(master_tx), &data) == 0) {
            int result = defy_wired_rx_accept(&rx, &data, &ack);
            assert(result == DEFY_WIRED_UPDATED || result == DEFY_WIRED_DUPLICATE);
            if (result == DEFY_WIRED_UPDATED) {
                assert(received_count < expected_count);
                assert(memcmp(rx.bitmap, expected[received_count++], DEFY_WIRED_BITMAP_SIZE) == 0);
            }
            defy_wired_encode(&ack, preloaded);
        }
        if (defy_wired_decode(master_rx, sizeof(master_rx), &ack) == 0) {
            assert(defy_wired_tx_ack(&tx, &ack));
        }
    }
    assert(expected_count > 100 && received_count == expected_count && tx.count == 0);
}

static void test_battery_show_codec(void) {
    struct defy_wired_battery_show_command command = {.side = DEFY_WIRED_RIGHT,
                                                  .session = 0x12345678};
    struct defy_wired_battery_show_command decoded;
    struct defy_wired_packet key_packet;
    uint8_t original[DEFY_WIRED_FRAME_SIZE], altered[DEFY_WIRED_FRAME_SIZE];
    assert(defy_wired_battery_show_encode(&command, original) == 0);
    assert(decode_battery_show(original, sizeof(original), &decoded) == 0);
    assert(decoded.side == command.side && decoded.session == command.session);
    assert(defy_wired_decode(original, sizeof(original), &key_packet) == -EPROTO);
    for (size_t field = 0; field < 30; field++) {
        if (field >= 8 && field < 12) {
            continue;
        }
        memcpy(altered, original, sizeof(altered));
        altered[field] = 0xff;
        frame_crc(altered);
        assert(decode_battery_show(altered, sizeof(altered), &decoded) == -EPROTO);
    }
    memcpy(altered, original, sizeof(altered));
    memset(altered + 8, 0, 4);
    frame_crc(altered);
    assert(decode_battery_show(altered, sizeof(altered), &decoded) == -EPROTO);
    command.side = 2;
    assert(defy_wired_battery_show_encode(&command, original) == -EINVAL);
    command.side = 0;
    command.session = 0;
    assert(defy_wired_battery_show_encode(&command, original) == -EINVAL);
}

static void test_battery_show_replaces_ack_without_losing_tap(void) {
    struct defy_wired_tx tx;
    struct defy_wired_rx rx;
    struct defy_wired_packet packet, ack;
    struct defy_wired_battery_show_command command = {.side = DEFY_WIRED_LEFT, .session = 42};
    uint8_t frame[DEFY_WIRED_FRAME_SIZE];
    defy_wired_tx_init(&tx, DEFY_WIRED_LEFT, 42);
    defy_wired_rx_init(&rx);
    assert(exchange(&tx, &rx) == DEFY_WIRED_UPDATED);
    assert(defy_wired_tx_set_key(&tx, 14, true) == 0);
    assert(defy_wired_tx_set_key(&tx, 14, false) == 0);
    packet = outgoing(&tx);
    assert(defy_wired_rx_accept(&rx, &packet, &ack) == DEFY_WIRED_UPDATED);
    assert(defy_wired_battery_show_encode(&command, frame) == 0);
    assert(defy_wired_decode(frame, sizeof(frame), &ack) == -EPROTO);
    assert(tx.count == 2 && rx.bitmap[1] == 0x40);
    assert(exchange(&tx, &rx) == DEFY_WIRED_DUPLICATE);
    assert(exchange(&tx, &rx) == DEFY_WIRED_UPDATED);
    assert(tx.count == 0 && rx.bitmap[1] == 0 && rx.sequence == 2);
}

static void test_lighting_codec(void) {
    struct defy_wired_lighting_command command = {
        .opcode = DEFY_CONTROL_LIGHTING_SET, .side = DEFY_WIRED_RIGHT, .session = 0x12345678,
        .state = {.boot = 0x90abcdef, .revision = 0xffffffff, .enabled = true,
                  .keys = {0xff, 0, 0x80}, .underglow = {1, 0xff, 0}, .groups = 3, .level = 0x10},
    };
    struct defy_wired_lighting_command decoded;
    struct defy_wired_battery_show_command show;
    struct defy_wired_packet keys;
    uint8_t original[DEFY_WIRED_FRAME_SIZE], altered[DEFY_WIRED_FRAME_SIZE];
    for (uint8_t opcode = DEFY_CONTROL_LIGHTING_SET; opcode <= DEFY_CONTROL_LIGHTING_ACK;
         opcode++) {
        command.opcode = opcode;
        assert(defy_wired_lighting_encode(&command, original) == 0);
        assert(decode_lighting(original, sizeof(original), &decoded) == 0);
        assert(decoded.opcode == opcode && decoded.side == command.side &&
               decoded.session == command.session &&
               defy_lighting_equal(&decoded.state, &command.state));
        assert(defy_wired_decode(original, sizeof(original), &keys) == -EPROTO);
        assert(decode_battery_show(original, sizeof(original), &show) == -EPROTO);
        /* The session, boot, revision, colours, groups and level, bytes 8 to 28, take other
         * values. */
        for (size_t field = 0; field < 30; field++) {
            if (field >= 8 && field < 29) {
                continue;
            }
            memcpy(altered, original, sizeof(altered));
            altered[field] = 0xff;
            frame_crc(altered);
            assert(decode_lighting(altered, sizeof(altered), &decoded) == -EPROTO);
        }
        for (size_t field = 8; field < 20; field += 4) {
            memcpy(altered, original, sizeof(altered));
            memset(altered + field, 0, 4);
            frame_crc(altered);
            assert(decode_lighting(altered, sizeof(altered), &decoded) == -EPROTO);
        }
    }
    command.state.enabled = false;
    command.opcode = DEFY_CONTROL_LIGHTING_SET;
    assert(defy_wired_lighting_encode(&command, original) == 0);
    assert(decode_lighting(original, sizeof(original), &decoded) == 0);
    assert(!decoded.state.enabled);
    command.opcode = DEFY_CONTROL_BATTERY_SHOW;
    assert(defy_wired_lighting_encode(&command, original) == -EINVAL);
}

static int decode_group(const uint8_t *frame, size_t length,
                        struct defy_wired_lighting_group_command *command) {
    struct defy_wired_control control;
    int err = defy_wired_control_check(frame, length, &control);
    return err ? err : defy_wired_lighting_group_decode(frame, &control, command);
}

static void test_lighting_group_codec(void) {
    struct defy_wired_lighting_group_command command = {
        .side = DEFY_WIRED_LEFT, .session = 0x12345678,
        .group = {.boot = 0x90abcdef, .index = 15, .colour = {0xff, 0, 0x80},
                  .leds = 0x7ffffffff},
    };
    struct defy_wired_lighting_group_command decoded;
    struct defy_wired_lighting_command state;
    uint8_t original[DEFY_WIRED_FRAME_SIZE], altered[DEFY_WIRED_FRAME_SIZE];
    assert(defy_wired_lighting_group_encode(&command, original) == 0);
    assert(decode_group(original, sizeof(original), &decoded) == 0);
    assert(decoded.side == command.side && decoded.session == command.session &&
           memcmp(&decoded.group, &command.group, sizeof(command.group)) == 0);
    assert(decode_lighting(original, sizeof(original), &state) == -EPROTO);
    /* The session, boot, colour and the LEDs' first four bytes take other values; the index,
     * the LEDs' last byte, the header and the padding do not. */
    for (size_t field = 0; field < 30; field++) {
        if ((field >= 8 && field < 16) || (field >= 17 && field < 24)) {
            continue;
        }
        memcpy(altered, original, sizeof(altered));
        altered[field] = 0xff;
        frame_crc(altered);
        assert(decode_group(altered, sizeof(altered), &decoded) == -EPROTO);
    }
    memcpy(altered, original, sizeof(altered));
    memset(altered + 12, 0, 4);
    frame_crc(altered);
    assert(decode_group(altered, sizeof(altered), &decoded) == -EPROTO);
    command.group.index = 16;
    assert(defy_wired_lighting_group_encode(&command, original) == -EINVAL);
}

/* A fixed record and frame pin the version report's format. */
static const uint8_t version_record[DEFY_HALF_VERSION_SIZE] = {
    0x01, 0x01, 0x78, 0x56, 0x34, 0x12, 0x02, 0x00, 0x00, 0x00, 0x76, 0x30, 0x2e, 0x31,
    0x2e, 0x30, 0x2d, 0x33, 0x2d, 0x67, 0x32, 0x38, 0x32, 0x36, 0x63, 0x62, 0x32, 0x2d,
};
static const uint8_t version_frame[DEFY_WIRED_FRAME_SIZE] = {
    0x44, 0x56, 0x01, 0x01, 0x78, 0x56, 0x34, 0x12, 0x02, 0x00, 0x00, 0x00, 0x76, 0x30, 0x2e, 0x31,
    0x2e, 0x30, 0x2d, 0x33, 0x2d, 0x67, 0x32, 0x38, 0x32, 0x36, 0x63, 0x62, 0x32, 0x2d, 0x82, 0x09,
};

static bool versions_equal(const struct defy_half_version *a, const struct defy_half_version *b) {
    return a->side == b->side && a->program_crc == b->program_crc &&
           a->seal_version == b->seal_version && strcmp(a->module, b->module) == 0;
}

static void test_half_version_record(void) {
    struct defy_half_version version = {
        .side = DEFY_WIRED_RIGHT, .program_crc = 0x12345678, .seal_version = 2,
        .module = "v0.1.0-3-g2826cb2-",
    };
    uint8_t record[DEFY_HALF_VERSION_SIZE];
    memset(record, 0xa5, sizeof(record));
    defy_half_version_encode(&version, record);
    assert(memcmp(record, version_record, sizeof(record)) == 0);
    struct defy_half_version decoded;
    memset(&decoded, 0xa5, sizeof(decoded));
    assert(defy_half_version_decode(record, &decoded));
    assert(versions_equal(&decoded, &version));
    assert(decoded.module[DEFY_HALF_VERSION_MODULE_SIZE] == '\0');

    /* A shorter module version is padded with zeros; no seal gives zeros. */
    struct defy_half_version short_version = {.side = DEFY_WIRED_LEFT, .module = "unknown"};
    defy_half_version_encode(&short_version, record);
    assert(record[0] == DEFY_HALF_VERSION_FORMAT && record[1] == DEFY_WIRED_LEFT);
    for (size_t i = 2; i < 10; i++) {
        assert(record[i] == 0);
    }
    assert(memcmp(record + 10, "unknown", 7) == 0);
    for (size_t i = 17; i < DEFY_HALF_VERSION_SIZE; i++) {
        assert(record[i] == 0);
    }
    memset(&decoded, 0xa5, sizeof(decoded));
    assert(defy_half_version_decode(record, &decoded));
    assert(versions_equal(&decoded, &short_version));
    struct defy_half_version empty = {.side = DEFY_WIRED_LEFT};
    defy_half_version_encode(&empty, record);
    assert(defy_half_version_decode(record, &decoded) && decoded.module[0] == '\0');

    /* Another format or side, a byte outside printable ASCII, or a byte after
     * the padding, leaves the output alone. */
    const struct {
        size_t offset;
        uint8_t value;
    } invalid[] = {
        {0, 0}, {0, 2}, {1, 2}, {1, 0xff}, {10, 0x1f}, {10, 0x7f}, {14, 0x80}, {27, '\n'},
    };
    for (size_t i = 0; i < sizeof(invalid) / sizeof(invalid[0]); i++) {
        memcpy(record, version_record, sizeof(record));
        record[invalid[i].offset] = invalid[i].value;
        decoded = short_version;
        assert(!defy_half_version_decode(record, &decoded));
        assert(versions_equal(&decoded, &short_version));
    }
    memcpy(record, version_record, sizeof(record));
    record[20] = 0;
    assert(!defy_half_version_decode(record, &decoded));
    record[27] = 0;
    assert(!defy_half_version_decode(record, &decoded));
    memset(record + 20, 0, 8);
    assert(defy_half_version_decode(record, &decoded));
    assert(strcmp(decoded.module, "v0.1.0-3-g") == 0);
    record[10] = ' ';
    record[11] = '~';
    assert(defy_half_version_decode(record, &decoded));
    assert(strncmp(decoded.module, " ~", 2) == 0);
}

static void test_half_version_frame(void) {
    uint8_t frame[DEFY_WIRED_FRAME_SIZE];
    uint8_t record[DEFY_HALF_VERSION_SIZE];
    memset(frame, 0xa5, sizeof(frame));
    defy_wired_half_version_encode(version_record, frame);
    assert(memcmp(frame, version_frame, sizeof(frame)) == 0);
    assert(defy_wired_frame_crc_ok(frame));
    memset(record, 0, sizeof(record));
    assert(defy_wired_half_version_decode(frame, sizeof(frame), record) == 0);
    assert(memcmp(record, version_record, sizeof(record)) == 0);

    /* A frame of another kind is refused and leaves the record alone. */
    memset(record, 0x5a, sizeof(record));
    memcpy(frame, version_frame, sizeof(frame));
    frame[1] = 'B';
    defy_wired_frame_seal(frame);
    assert(defy_wired_half_version_decode(frame, sizeof(frame), record) == -EPROTO);
    struct defy_wired_packet ack = {
        .kind = DEFY_WIRED_ACK, .side = DEFY_WIRED_LEFT, .session = 1,
    };
    defy_wired_encode(&ack, frame);
    assert(defy_wired_half_version_decode(frame, sizeof(frame), record) == -EPROTO);
    for (size_t i = 0; i < sizeof(record); i++) {
        assert(record[i] == 0x5a);
    }

    /* The cable codecs refuse a 'DV' frame, and the frame passes a record
     * through as it came: the record's own decoder refuses another format. */
    struct defy_wired_packet packet;
    struct defy_wired_control control;
    assert(defy_wired_decode(version_frame, sizeof(version_frame), &packet) == -EPROTO);
    assert(defy_wired_control_check(version_frame, sizeof(version_frame), &control) == -EPROTO);
    uint8_t future[DEFY_HALF_VERSION_SIZE];
    memcpy(future, version_record, sizeof(future));
    future[0] = 2;
    defy_wired_half_version_encode(future, frame);
    assert(defy_wired_half_version_decode(frame, sizeof(frame), record) == 0);
    assert(memcmp(record, future, sizeof(record)) == 0);
    struct defy_half_version decoded;
    assert(!defy_half_version_decode(record, &decoded));
}

int main(void) {
    test_frame_envelope_and_fixed_frames();
    test_codec_and_ownership();
    test_retained_transitions_and_duplicates();
    test_every_key_and_swapped_sockets();
    test_disconnect_and_restarts();
    test_bad_sequences_and_acks();
    test_queue_overflow_and_wrap();
    test_transactional_receive();
    test_pipelined_loss_and_corruption();
    test_battery_show_codec();
    test_battery_show_replaces_ack_without_losing_tap();
    test_lighting_codec();
    test_lighting_group_codec();
    test_half_version_record();
    test_half_version_frame();
    return 0;
}
