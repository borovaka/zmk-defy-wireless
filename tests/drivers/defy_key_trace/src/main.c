/* SPDX-License-Identifier: MIT */

#include <string.h>

#include <zephyr/kernel.h>
#include <zephyr/sys/byteorder.h>
#include <zephyr/ztest.h>

#include <zmk/event_manager.h>
#include <zmk/events/keycode_state_changed.h>

static int64_t ticks;
static int64_t test_ticks(void) { return ticks; }

/* The trace clock then reads the test's ticks as microseconds. */
#define k_uptime_ticks test_ticks
#undef k_ticks_to_us_floor64
#define k_ticks_to_us_floor64(value) ((uint64_t)(value))
#undef ZMK_SUBSCRIPTION
#define ZMK_SUBSCRIPTION(module, event)
#include "../../../../diagnostics/defy_key_trace.c"
#undef k_uptime_ticks
#undef k_ticks_to_us_floor64

#define HEADER DEFY_KEY_TRACE_HEADER_SIZE
#define RECORD DEFY_KEY_TRACE_RECORD_SIZE
#define PAYLOAD 260u
#define PER_FRAME ((PAYLOAD - HEADER) / RECORD)

const struct zmk_event_type zmk_event_zmk_keycode_state_changed = {
    .name = "zmk_keycode_state_changed",
};
static const struct zmk_event_type other_event = {.name = "other"};

struct zmk_keycode_state_changed *as_zmk_keycode_state_changed(const zmk_event_t *event) {
    return event->event == &zmk_event_zmk_keycode_state_changed
               ? &((struct zmk_keycode_state_changed_event *)event)->data
               : NULL;
}

static uint8_t out[PAYLOAD];

struct readout {
    uint8_t count;
    uint32_t written;
    uint32_t first;
    uint32_t now;
};

static struct readout read_trace(uint32_t from, bool clear, size_t capacity) {
    memset(out, 0xa5, sizeof(out));
    size_t length = defy_key_trace_read(from, clear, out, capacity);
    zassert_true(length >= HEADER);
    zassert_equal(out[0], 1);
    zassert_equal(out[1], RECORD);
    zassert_equal(out[2], DEFY_KEY_TRACE_CAPACITY);
    zassert_equal(length, HEADER + out[3] * RECORD);
    return (struct readout){
        .count = out[3],
        .written = sys_get_le32(out + 4),
        .first = sys_get_le32(out + 8),
        .now = sys_get_le32(out + 12),
    };
}

static const uint8_t *record_at(size_t index) { return out + HEADER + index * RECORD; }

static void add_at(int64_t at, uint8_t kind, uint8_t flags, uint16_t value, uint32_t extra) {
    ticks = at;
    defy_key_trace_add(kind, flags, value, extra);
}

static void before(void *fixture) {
    ARG_UNUSED(fixture);
    ticks = 0;
    defy_key_trace_read(0, true, out, sizeof(out));
}

ZTEST_SUITE(defy_key_trace, NULL, NULL, before, NULL, NULL);

ZTEST(defy_key_trace, test_empty_trace_reads_only_a_header) {
    ticks = 4321;
    struct readout trace = read_trace(0, false, PAYLOAD);
    zassert_equal(trace.count, 0);
    zassert_equal(trace.written, 0);
    zassert_equal(trace.first, 0);
    zassert_equal(trace.now, 4321);
    zassert_equal(out[HEADER], 0xa5);
}

ZTEST(defy_key_trace, test_records_read_oldest_first_with_every_field) {
    add_at(1000, DEFY_KEY_TRACE_WIRED_POLL, DEFY_KEY_TRACE_ACTIVE | 1, 0xbeef, 980);
    add_at(1031, DEFY_KEY_TRACE_SNAPSHOT, DEFY_KEY_TRACE_ACTIVE | 1, 7, 1000);
    add_at(1061, DEFY_KEY_TRACE_POSITION, DEFY_KEY_TRACE_ACTIVE | 1, 7, 7);
    add_at(1092, DEFY_KEY_TRACE_KEYCODE, DEFY_KEY_TRACE_ACTIVE, 0x07, 0x35);
    ticks = 2000;
    struct readout trace = read_trace(0, false, PAYLOAD);
    zassert_equal(trace.count, 4);
    zassert_equal(trace.written, 4);
    zassert_equal(trace.first, 0);
    zassert_equal(trace.now, 2000);
    static const uint8_t expected[4][RECORD] = {
        {0xe8, 0x03, 0, 0, 4, 0x09, 0xef, 0xbe, 0xd4, 0x03, 0, 0},
        {0x07, 0x04, 0, 0, 1, 0x09, 0x07, 0x00, 0xe8, 0x03, 0, 0},
        {0x25, 0x04, 0, 0, 2, 0x09, 0x07, 0x00, 0x07, 0x00, 0, 0},
        {0x44, 0x04, 0, 0, 3, 0x08, 0x07, 0x00, 0x35, 0x00, 0, 0},
    };
    zassert_mem_equal(out + HEADER, expected, sizeof(expected));
}

ZTEST(defy_key_trace, test_clock_wraps_at_32_bits) {
    add_at(BIT64(32) - 3, DEFY_KEY_TRACE_POSITION, 0, 1, 0);
    add_at(BIT64(32) + 5, DEFY_KEY_TRACE_POSITION, 0, 2, 0);
    struct readout trace = read_trace(0, false, PAYLOAD);
    zassert_equal(trace.now, 5);
    zassert_equal(sys_get_le32(record_at(0)), UINT32_MAX - 2);
    zassert_equal(sys_get_le32(record_at(1)), 5);
}

ZTEST(defy_key_trace, test_ring_keeps_the_newest_64_and_pages_through_them) {
    for (uint32_t i = 0; i < 100; i++) {
        add_at(100 + i, DEFY_KEY_TRACE_POSITION, 0, i, i * 3);
    }
    uint32_t next = 0;
    uint32_t pages = 0;
    uint32_t seen = 0;
    while (true) {
        struct readout trace = read_trace(next, false, PAYLOAD);
        zassert_equal(trace.written, 100);
        zassert_equal(trace.first, pages ? next : 100 - DEFY_KEY_TRACE_CAPACITY);
        for (size_t i = 0; i < trace.count; i++) {
            uint32_t sequence = trace.first + i;
            zassert_equal(sys_get_le32(record_at(i)), 100 + sequence);
            zassert_equal(sys_get_le16(record_at(i) + 6), sequence);
            zassert_equal(sys_get_le32(record_at(i) + 8), sequence * 3);
        }
        seen += trace.count;
        next = trace.first + trace.count;
        if (!trace.count) {
            break;
        }
        zassert_equal(trace.count, MIN(PER_FRAME, 100 - trace.first));
        pages++;
    }
    zassert_equal(pages, 4);
    zassert_equal(seen, DEFY_KEY_TRACE_CAPACITY);
    zassert_equal(next, 100);
}

ZTEST(defy_key_trace, test_readout_fits_the_capacity_it_is_given) {
    for (uint32_t i = 0; i < DEFY_KEY_TRACE_CAPACITY; i++) {
        add_at(i, DEFY_KEY_TRACE_KEYCODE, 0, 7, i);
    }
    zassert_equal(PER_FRAME, 20);
    zassert_equal(defy_key_trace_read(0, false, out, PAYLOAD), 256);
    zassert_equal(out[3], 20);
    zassert_equal(defy_key_trace_read(0, false, out, HEADER - 1), 0);
    zassert_equal(read_trace(0, false, HEADER).count, 0);
    zassert_equal(read_trace(0, false, HEADER + RECORD - 1).count, 0);
    zassert_equal(read_trace(0, false, HEADER + RECORD).count, 1);
    memset(out, 0xa5, sizeof(out));
    defy_key_trace_read(0, false, out, HEADER + 2 * RECORD + 5);
    zassert_equal(out[3], 2);
    zassert_equal(out[HEADER + 2 * RECORD], 0xa5);
}

ZTEST(defy_key_trace, test_start_is_clamped_to_the_records_held) {
    for (uint32_t i = 0; i < 70; i++) {
        add_at(i, DEFY_KEY_TRACE_POSITION, 0, i, 0);
    }
    struct readout trace = read_trace(1000, false, PAYLOAD);
    zassert_equal(trace.first, 70);
    zassert_equal(trace.count, 0);
    trace = read_trace(3, false, PAYLOAD);
    zassert_equal(trace.first, 6);
    zassert_equal(sys_get_le16(record_at(0) + 6), 6);
    trace = read_trace(65, false, PAYLOAD);
    zassert_equal(trace.first, 65);
    zassert_equal(trace.count, 5);
    zassert_equal(sys_get_le16(record_at(4) + 6), 69);
}

ZTEST(defy_key_trace, test_clear_empties_the_ring_and_restarts_numbering) {
    for (uint32_t i = 0; i < 80; i++) {
        add_at(i, DEFY_KEY_TRACE_POSITION, 0, i, 0);
    }
    ticks = 500;
    struct readout trace = read_trace(0, true, PAYLOAD);
    zassert_equal(trace.written, 0);
    zassert_equal(trace.count, 0);
    zassert_equal(trace.now, 500);
    add_at(600, DEFY_KEY_TRACE_POSITION, 0, 0x1234, 0);
    trace = read_trace(0, false, PAYLOAD);
    zassert_equal(trace.written, 1);
    zassert_equal(trace.first, 0);
    zassert_equal(trace.count, 1);
    zassert_equal(sys_get_le16(record_at(0) + 6), 0x1234);
    zassert_equal(sys_get_le32(record_at(0)), 600);
}

static int raise_keycode(const struct zmk_event_type *type, uint16_t page, uint32_t keycode,
                         bool pressed) {
    struct zmk_keycode_state_changed_event event = {
        .header = {.event = type},
        .data = {.usage_page = page, .keycode = keycode, .state = pressed},
    };
    return keycode_changed(&event.header);
}

ZTEST(defy_key_trace, test_keycode_listener_records_and_never_captures) {
    ticks = 70;
    zassert_equal(raise_keycode(&zmk_event_zmk_keycode_state_changed, 0x07, 0x35, true),
                  ZMK_EV_EVENT_BUBBLE);
    ticks = 71;
    zassert_equal(raise_keycode(&zmk_event_zmk_keycode_state_changed, 0x0c, 0xe9, false),
                  ZMK_EV_EVENT_BUBBLE);
    zassert_equal(raise_keycode(&other_event, 0x07, 0x08, true), ZMK_EV_EVENT_BUBBLE);
    struct readout trace = read_trace(0, false, PAYLOAD);
    zassert_equal(trace.count, 2);
    static const uint8_t expected[2][RECORD] = {
        {70, 0, 0, 0, DEFY_KEY_TRACE_KEYCODE, DEFY_KEY_TRACE_ACTIVE, 0x07, 0, 0x35, 0, 0, 0},
        {71, 0, 0, 0, DEFY_KEY_TRACE_KEYCODE, 0, 0x0c, 0, 0xe9, 0, 0, 0},
    };
    zassert_mem_equal(out + HEADER, expected, sizeof(expected));
}
