/* SPDX-License-Identifier: MIT */

#ifndef DYGMA_DEFY_WIRED_FRAME_H
#define DYGMA_DEFY_WIRED_FRAME_H

/*
 * What every 32-byte cable frame shares: the magic, 'D' and a letter for the
 * kind, 'W' for keys, 'L' for the control channel, 'B' for battery telemetry
 * and 'V' for the version report, then the frame's own fields, little-endian,
 * then a CRC16/CCITT-FALSE over bytes 0..29 in bytes 30..31. Plain C, so the
 * host tests build it without Zephyr.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#define DEFY_WIRED_FRAME_SIZE 32u
#define DEFY_WIRED_CRC_OFFSET 30u

_Static_assert(DEFY_WIRED_CRC_OFFSET + 2u == DEFY_WIRED_FRAME_SIZE,
               "the CRC ends the frame");

static inline uint16_t defy_wired_get16(const uint8_t *data) {
    return (uint16_t)(data[0] | data[1] << 8);
}

static inline uint32_t defy_wired_get32(const uint8_t *data) {
    return defy_wired_get16(data) | (uint32_t)defy_wired_get16(data + 2) << 16;
}

static inline void defy_wired_put16(uint8_t *data, uint16_t value) {
    data[0] = (uint8_t)value;
    data[1] = (uint8_t)(value >> 8);
}

static inline void defy_wired_put32(uint8_t *data, uint32_t value) {
    defy_wired_put16(data, (uint16_t)value);
    defy_wired_put16(data + 2, (uint16_t)(value >> 16));
}

/* Zeroes the frame and writes its magic: 'D', then kind. */
static inline void defy_wired_frame_start(uint8_t frame[DEFY_WIRED_FRAME_SIZE], char kind) {
    memset(frame, 0, DEFY_WIRED_FRAME_SIZE);
    frame[0] = 'D';
    frame[1] = (uint8_t)kind;
}

/* Whether the frame's magic is 'D', then kind. */
static inline bool defy_wired_frame_is(const uint8_t *frame, char kind) {
    return frame[0] == 'D' && frame[1] == (uint8_t)kind;
}

/* Polynomial 0x1021, initial value 0xffff, neither input nor output reflected. */
static inline uint16_t defy_wired_crc16(const uint8_t *data, size_t length) {
    uint16_t crc = 0xffffu;
    for (size_t i = 0; i < length; i++) {
        crc ^= (uint16_t)data[i] << 8;
        for (uint8_t bit = 0; bit < 8; bit++) {
            crc = (uint16_t)((crc & 0x8000u) ? (crc << 1) ^ 0x1021u : crc << 1);
        }
    }
    return crc;
}

/* Writes the CRC of bytes 0..29 into bytes 30..31. */
static inline void defy_wired_frame_seal(uint8_t frame[DEFY_WIRED_FRAME_SIZE]) {
    defy_wired_put16(frame + DEFY_WIRED_CRC_OFFSET, defy_wired_crc16(frame, DEFY_WIRED_CRC_OFFSET));
}

/* Whether bytes 30..31 hold the CRC of bytes 0..29. */
static inline bool defy_wired_frame_crc_ok(const uint8_t frame[DEFY_WIRED_FRAME_SIZE]) {
    return defy_wired_crc16(frame, DEFY_WIRED_CRC_OFFSET) ==
           defy_wired_get16(frame + DEFY_WIRED_CRC_OFFSET);
}

#endif
