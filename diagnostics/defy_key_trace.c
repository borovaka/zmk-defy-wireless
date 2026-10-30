/* SPDX-License-Identifier: MIT */

#include <zephyr/kernel.h>
#include <zephyr/sys/byteorder.h>
#include <zephyr/sys/util.h>

#include <dygma/defy_key_trace.h>
#include <zmk/event_manager.h>
#include <zmk/events/keycode_state_changed.h>

BUILD_ASSERT(IS_POWER_OF_TWO(DEFY_KEY_TRACE_CAPACITY));

struct trace_record {
    uint32_t time;
    uint8_t kind;
    uint8_t flags;
    uint16_t value;
    uint32_t extra;
};

BUILD_ASSERT(sizeof(struct trace_record) == DEFY_KEY_TRACE_RECORD_SIZE);

static struct k_spinlock trace_lock;
static struct trace_record records[DEFY_KEY_TRACE_CAPACITY];
/* Records written since the last clear; record n lives at n % capacity. */
static uint32_t written;

uint32_t defy_key_trace_now(void) { return (uint32_t)k_ticks_to_us_floor64(k_uptime_ticks()); }

void defy_key_trace_add(uint8_t kind, uint8_t flags, uint16_t value, uint32_t extra) {
    k_spinlock_key_t key = k_spin_lock(&trace_lock);
    /* Read under the lock, so the ring's order is also the clock's. */
    records[written % DEFY_KEY_TRACE_CAPACITY] = (struct trace_record){
        .time = defy_key_trace_now(), .kind = kind, .flags = flags,
        .value = value, .extra = extra,
    };
    written++;
    k_spin_unlock(&trace_lock, key);
}

size_t defy_key_trace_read(uint32_t from, bool clear, uint8_t *out, size_t capacity) {
    if (capacity < DEFY_KEY_TRACE_HEADER_SIZE) {
        return 0;
    }
    k_spinlock_key_t key = k_spin_lock(&trace_lock);
    if (clear) {
        written = 0;
    }
    uint32_t oldest = written > DEFY_KEY_TRACE_CAPACITY ? written - DEFY_KEY_TRACE_CAPACITY : 0;
    uint32_t first = CLAMP(from, oldest, written);
    size_t count = MIN(written - first,
                       (capacity - DEFY_KEY_TRACE_HEADER_SIZE) / DEFY_KEY_TRACE_RECORD_SIZE);
    out[0] = DEFY_KEY_TRACE_VERSION;
    out[1] = DEFY_KEY_TRACE_RECORD_SIZE;
    out[2] = DEFY_KEY_TRACE_CAPACITY;
    out[3] = count;
    sys_put_le32(written, out + 4);
    sys_put_le32(first, out + 8);
    sys_put_le32(defy_key_trace_now(), out + 12);
    uint8_t *cursor = out + DEFY_KEY_TRACE_HEADER_SIZE;
    for (size_t i = 0; i < count; i++, cursor += DEFY_KEY_TRACE_RECORD_SIZE) {
        const struct trace_record *record = &records[(first + i) % DEFY_KEY_TRACE_CAPACITY];
        sys_put_le32(record->time, cursor);
        cursor[4] = record->kind;
        cursor[5] = record->flags;
        sys_put_le16(record->value, cursor + 6);
        sys_put_le32(record->extra, cursor + 8);
    }
    k_spin_unlock(&trace_lock, key);
    return DEFY_KEY_TRACE_HEADER_SIZE + count * DEFY_KEY_TRACE_RECORD_SIZE;
}

static int keycode_changed(const zmk_event_t *event) {
    const struct zmk_keycode_state_changed *changed = as_zmk_keycode_state_changed(event);
    if (changed) {
        defy_key_trace_add(DEFY_KEY_TRACE_KEYCODE, changed->state ? DEFY_KEY_TRACE_ACTIVE : 0,
                           changed->usage_page, changed->keycode);
    }
    return ZMK_EV_EVENT_BUBBLE;
}

/*
 * Only observes. ZMK calls listeners in link order, not by name, and this
 * module links before ZMK's own sources, so it sees each keycode event before
 * any ZMK listener, hid_listener included.
 */
ZMK_LISTENER(defy_key_trace, keycode_changed);
ZMK_SUBSCRIPTION(defy_key_trace, zmk_keycode_state_changed);
