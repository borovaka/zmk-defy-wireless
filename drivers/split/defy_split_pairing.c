/* SPDX-License-Identifier: MIT */

/*
 * The Neuron's split slots. ZMK's split central reserves a slot for a half
 * on its first connection attempt, before any bond exists, through
 * zmk_ble_put_peripheral_addr(). ZMK's ble.c keeps those addresses in a
 * private table that only a restart reloads. This module takes the table
 * over, so that the half updater can report it and clear it in place:
 *
 * - The wrap of zmk_ble_put_peripheral_addr(), linked with --wrap, keeps
 *   ZMK's semantics and its settings keys, ble/peripheral_addresses/<slot>.
 * - The settings handler for that subtree is more specific than ZMK's "ble"
 *   handler, so Zephyr hands these keys to this module instead. ZMK's own
 *   table then stays empty; only the wrapped function read it.
 *
 * docs/half-updater.md describes PAIRING and RESET PAIRING.
 */

#include <errno.h>
#include <stdio.h>
#include <string.h>

#include <zephyr/bluetooth/addr.h>
#include <zephyr/bluetooth/bluetooth.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/settings/settings.h>

#include <zmk/ble.h>

#include <dygma/defy_split_pairing.h>
#include <dygma/defy_wrap.h>

#include "defy_split_internal.h"

LOG_MODULE_REGISTER(defy_split_pairing, CONFIG_ZMK_LOG_LEVEL);

BUILD_ASSERT(ZMK_SPLIT_BLE_PERIPHERAL_COUNT == DEFY_SPLIT_PAIRING_SLOTS,
             "PAIRING reports one split slot per half");

/* ZMK's settings subtree for the slots. */
#define SLOT_ROOT "ble/peripheral_addresses"
#define SLOT_KEY_SIZE sizeof(SLOT_ROOT "/0")

BUILD_ASSERT(DEFY_SPLIT_PAIRING_SLOTS <= 10, "each slot's key ends in one digit");

/* Each slot's address, BT_ADDR_LE_ANY while the slot is empty. */
static bt_addr_le_t slots[DEFY_SPLIT_PAIRING_SLOTS];
static struct k_spinlock slots_lock;
/*
 * Keeps each change to a slot together with the change to its stored address,
 * so that the settings follow the table. The wrap runs on the Bluetooth
 * receive thread, the clear on the updater thread. No holder calls into the
 * Bluetooth host, which could wait for the receive thread.
 */
static K_MUTEX_DEFINE(store_lock);

static void slot_key(char name[SLOT_KEY_SIZE], size_t slot) {
    snprintf(name, SLOT_KEY_SIZE, SLOT_ROOT "/%u", (unsigned int)slot);
}

static void copy_slots(bt_addr_le_t copy[DEFY_SPLIT_PAIRING_SLOTS]) {
    k_spinlock_key_t key = k_spin_lock(&slots_lock);
    memcpy(copy, slots, sizeof(slots));
    k_spin_unlock(&slots_lock, key);
}

/* Called with slots_lock held. */
static int find_slot(const bt_addr_le_t *addr) {
    for (size_t i = 0; i < ARRAY_SIZE(slots); i++) {
        if (bt_addr_le_eq(&slots[i], addr)) {
            return i;
        }
    }
    return -ENOMEM;
}

/*
 * ZMK's zmk_ble_put_peripheral_addr(): the slot that holds the address, else
 * the first empty one, which then stores it, else -ENOMEM.
 */
int __wrap_zmk_ble_put_peripheral_addr(const bt_addr_le_t *addr) {
    k_mutex_lock(&store_lock, K_FOREVER);
    k_spinlock_key_t key = k_spin_lock(&slots_lock);
    int slot = find_slot(addr);
    bool added = false;
    if (slot < 0) {
        slot = find_slot(BT_ADDR_LE_ANY);
        if (slot >= 0) {
            bt_addr_le_copy(&slots[slot], addr);
            added = true;
        }
    }
    k_spin_unlock(&slots_lock, key);
    if (added && IS_ENABLED(CONFIG_SETTINGS)) {
        char name[SLOT_KEY_SIZE];
        slot_key(name, slot);
        int err = settings_save_one(name, addr, sizeof(*addr));
        if (err) {
            LOG_WRN("Cannot save split slot %d: %d", slot, err);
        }
    }
    k_mutex_unlock(&store_lock);
    return slot;
}

DEFY_WRAP_CHECK(zmk_ble_put_peripheral_addr);

struct bond_search {
    bt_addr_le_t slots[DEFY_SPLIT_PAIRING_SLOTS];
    uint8_t bonded;
};

static void find_bond(const struct bt_bond_info *info, void *user_data) {
    struct bond_search *search = user_data;
    for (size_t i = 0; i < ARRAY_SIZE(search->slots); i++) {
        if (bt_addr_le_eq(&search->slots[i], &info->addr)) {
            search->bonded |= BIT(i);
        }
    }
}

size_t defy_split_pairing_status(uint8_t *out, size_t capacity) {
    if (!out || capacity < DEFY_SPLIT_PAIRING_SIZE) {
        return 0;
    }
    struct bond_search search = {0};
    copy_slots(search.slots);
    bt_foreach_bond(BT_ID_DEFAULT, find_bond, &search);
    out[0] = DEFY_SPLIT_PAIRING_VERSION;
    out[1] = DEFY_SPLIT_PAIRING_SLOTS;
    for (size_t i = 0; i < ARRAY_SIZE(search.slots); i++) {
        bool used = !bt_addr_le_eq(&search.slots[i], BT_ADDR_LE_ANY);
        out[2 + i] = (used ? DEFY_SPLIT_PAIRING_ADDRESS : 0) |
                     (search.bonded & BIT(i) ? DEFY_SPLIT_PAIRING_BONDED : 0);
    }
    return DEFY_SPLIT_PAIRING_SIZE;
}

int defy_split_pairing_clear(void) {
    bt_addr_le_t stored[DEFY_SPLIT_PAIRING_SLOTS];
    copy_slots(stored);
    for (size_t i = 0; i < ARRAY_SIZE(stored); i++) {
        if (bt_addr_le_eq(&stored[i], BT_ADDR_LE_ANY)) {
            continue;
        }
        /* Also disconnects the half, if it is connected. */
        int err = bt_unpair(BT_ID_DEFAULT, &stored[i]);
        if (err) {
            LOG_ERR("Cannot remove the bond of split slot %u: %d", (unsigned int)i, err);
            return err;
        }
    }
    defy_hybrid_ble_clear_hold_off();

    int result = 0;
    k_mutex_lock(&store_lock, K_FOREVER);
    for (size_t i = 0; i < ARRAY_SIZE(slots); i++) {
        if (IS_ENABLED(CONFIG_SETTINGS)) {
            char name[SLOT_KEY_SIZE];
            slot_key(name, i);
            int err = settings_delete(name);
            if (err) {
                LOG_ERR("Cannot delete split slot %u: %d", (unsigned int)i, err);
                result = result ? result : err;
                continue;
            }
        }
        k_spinlock_key_t key = k_spin_lock(&slots_lock);
        bt_addr_le_copy(&slots[i], BT_ADDR_LE_ANY);
        k_spin_unlock(&slots_lock, key);
    }
    k_mutex_unlock(&store_lock);
    return result;
}

#if IS_ENABLED(CONFIG_SETTINGS)

/* Runs from settings_load(), which ZMK calls before its split central starts. */
static int slot_set(const char *name, size_t len, settings_read_cb read_cb, void *cb_arg) {
    const char *next;
    if (settings_name_next(name, &next) != 1 || next || name[0] < '0' ||
        name[0] >= '0' + DEFY_SPLIT_PAIRING_SLOTS) {
        return -ENOENT;
    }
    if (len != sizeof(bt_addr_le_t)) {
        return -EINVAL;
    }
    bt_addr_le_t addr;
    ssize_t count = read_cb(cb_arg, &addr, sizeof(addr));
    if (count != sizeof(addr)) {
        return count < 0 ? (int)count : -EINVAL;
    }
    k_spinlock_key_t key = k_spin_lock(&slots_lock);
    bt_addr_le_copy(&slots[name[0] - '0'], &addr);
    k_spin_unlock(&slots_lock, key);
    return 0;
}

SETTINGS_STATIC_HANDLER_DEFINE(defy_split_pairing, SLOT_ROOT, NULL, slot_set, NULL, NULL);

#endif
