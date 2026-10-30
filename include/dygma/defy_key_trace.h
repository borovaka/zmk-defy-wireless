/*
 * The Neuron's key trace, diagnostics/defy_key_trace.c: a RAM ring of the last
 * 64 steps from a half's frame to a ZMK keycode, read through the half
 * updater's opcode 11. Without CONFIG_DEFY_KEY_TRACE the calls compile to
 * nothing.
 *
 * SPDX-License-Identifier: MIT
 */

#ifndef DYGMA_DEFY_KEY_TRACE_H
#define DYGMA_DEFY_KEY_TRACE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <zephyr/sys/util.h>

/*
 * Readout, little-endian: a 16-byte header of version 1, record size 12,
 * capacity 64, the record count in this reply, then the number of records
 * written since the last clear, the sequence number of the first record here,
 * and the trace clock at readout. Each record is the trace clock (uint32), its
 * kind and flags (uint8 each), a value (uint16) and an extra word (uint32).
 * The trace clock counts microseconds of uptime and wraps after 71 minutes.
 */
#define DEFY_KEY_TRACE_VERSION 1u
#define DEFY_KEY_TRACE_CAPACITY 64u
#define DEFY_KEY_TRACE_HEADER_SIZE 16u
#define DEFY_KEY_TRACE_RECORD_SIZE 12u

enum defy_key_trace_kind {
    /* A snapshot put into the hybrid central's queue. value: its queue order.
     * extra: the trace clock when its frame was received, at millisecond steps. */
    DEFY_KEY_TRACE_SNAPSHOT = 1,
    /* A position event from publish_bitmap. value: the position. extra: the
     * queue order of the snapshot being applied, or 0. */
    DEFY_KEY_TRACE_POSITION = 2,
    /* A ZMK keycode event. value: the usage page. extra: the keycode. */
    DEFY_KEY_TRACE_KEYCODE = 3,
    /* A cable frame that changed a half's keys, when the wired worker took it.
     * value: the frame's sequence, low 16 bits. extra: the trace clock when
     * its SPI transfer completed. */
    DEFY_KEY_TRACE_WIRED_POLL = 4,
};

/* Bits 0-1: the side, 0 left, 1 right, 2 unknown. */
#define DEFY_KEY_TRACE_SIDE 0x03u
#define DEFY_KEY_TRACE_BLE BIT(2)
/* Pressed, or a snapshot of a connected half. */
#define DEFY_KEY_TRACE_ACTIVE BIT(3)
/* The snapshot queue was full, so the snapshot was not queued. */
#define DEFY_KEY_TRACE_DROPPED BIT(4)
/* Bluetooth slot 1, or the second cable port. */
#define DEFY_KEY_TRACE_SOURCE BIT(5)

#if IS_ENABLED(CONFIG_DEFY_KEY_TRACE)
uint32_t defy_key_trace_now(void);
void defy_key_trace_add(uint8_t kind, uint8_t flags, uint16_t value, uint32_t extra);
/* Writes the readout from record sequence number `from` on, after clearing
 * the ring if asked. Returns its length, or 0 if out is too small. */
size_t defy_key_trace_read(uint32_t from, bool clear, uint8_t *out, size_t capacity);
#else
static inline uint32_t defy_key_trace_now(void) { return 0; }
static inline void defy_key_trace_add(uint8_t kind, uint8_t flags, uint16_t value,
                                      uint32_t extra) {
    ARG_UNUSED(kind);
    ARG_UNUSED(flags);
    ARG_UNUSED(value);
    ARG_UNUSED(extra);
}
#endif

#endif
