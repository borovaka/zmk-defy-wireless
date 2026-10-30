/*
 * The Neuron's records of the halves' version reports, for the half updater's
 * HALF VERSIONS. Each side keeps the latest report over each link, with when
 * it arrived, until that link goes down for the side. HALF VERSIONS gives the
 * newer of the two, so a half on both links stays known when one of them
 * drops. docs/versions.md describes them. The cable worker, Bluetooth's
 * callbacks and the updater's thread call in.
 *
 * SPDX-License-Identifier: MIT
 */

#include <string.h>

#include <zephyr/kernel.h>
#include <zephyr/sys/byteorder.h>
#include <zephyr/sys/util.h>

#include <dygma/defy_control.h>
#include <dygma/defy_half_version.h>
#include <dygma/defy_wired_protocol.h>

BUILD_ASSERT(DEFY_HALF_VERSIONS_RECORD == 8 + DEFY_HALF_VERSION_SIZE,
             "a HALF VERSIONS record is 8 bytes, then the half's report");

struct held_version {
    struct defy_half_version version;
    int64_t arrived;
    /* The split slot a report over Bluetooth came by. */
    uint8_t slot;
    bool held;
};

/* Guards the records. */
static struct k_spinlock held_lock;
/* For each side, the latest report over each link, enum defy_link. */
static struct held_version held[2][2];

void defy_half_version_received(enum defy_link link, uint8_t slot,
                                const struct defy_half_version *version) {
    if (version == NULL || link > DEFY_LINK_BLE || version->side > DEFY_WIRED_RIGHT) {
        return;
    }
    k_spinlock_key_t key = k_spin_lock(&held_lock);
    held[version->side][link] = (struct held_version){
        .version = *version, .arrived = k_uptime_get(), .slot = slot, .held = true,
    };
    k_spin_unlock(&held_lock, key);
}

void defy_half_version_wired_down(uint8_t side) {
    if (side > DEFY_WIRED_RIGHT) {
        return;
    }
    k_spinlock_key_t key = k_spin_lock(&held_lock);
    held[side][DEFY_LINK_WIRED].held = false;
    k_spin_unlock(&held_lock, key);
}

void defy_half_version_ble_down(uint8_t slot) {
    k_spinlock_key_t key = k_spin_lock(&held_lock);
    for (size_t side = 0; side < ARRAY_SIZE(held); side++) {
        if (held[side][DEFY_LINK_BLE].slot == slot) {
            held[side][DEFY_LINK_BLE].held = false;
        }
    }
    k_spin_unlock(&held_lock, key);
}

size_t defy_half_versions(uint8_t *output, size_t capacity) {
    if (output == NULL || capacity < DEFY_HALF_VERSIONS_SIZE) {
        return 0;
    }
    struct held_version latest[ARRAY_SIZE(held)];
    uint8_t links[ARRAY_SIZE(held)] = {0};

    k_spinlock_key_t key = k_spin_lock(&held_lock);
    int64_t now = k_uptime_get();
    for (size_t side = 0; side < ARRAY_SIZE(held); side++) {
        latest[side] = (struct held_version){0};
        for (uint8_t link = DEFY_LINK_WIRED; link <= DEFY_LINK_BLE; link++) {
            const struct held_version *record = &held[side][link];
            if (record->held && (!latest[side].held || record->arrived > latest[side].arrived)) {
                latest[side] = *record;
                links[side] = BIT(link);
            }
        }
    }
    k_spin_unlock(&held_lock, key);

    memset(output, 0, DEFY_HALF_VERSIONS_SIZE);
    output[0] = DEFY_HALF_VERSIONS_VERSION;
    output[1] = ARRAY_SIZE(held);
    sys_put_le16(DEFY_HALF_VERSIONS_RECORD, output + 2);
    sys_put_le32((uint32_t)now, output + 4);
    for (size_t side = 0; side < ARRAY_SIZE(held); side++) {
        uint8_t *entry = output + DEFY_HALF_VERSIONS_HEADER + side * DEFY_HALF_VERSIONS_RECORD;
        entry[0] = side;
        entry[1] = links[side];
        /* 0xFFFFFFFF means no record, so a record's age stops short of it. */
        uint32_t age = latest[side].held
                           ? MIN((uint64_t)UINT32_MAX - 1, (uint64_t)(now - latest[side].arrived))
                           : UINT32_MAX;
        sys_put_le32(age, entry + 4);
        if (latest[side].held) {
            defy_half_version_encode(&latest[side].version, entry + 8);
        }
    }
    return DEFY_HALF_VERSIONS_SIZE;
}
