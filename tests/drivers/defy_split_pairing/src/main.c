/* SPDX-License-Identifier: MIT */

#include <errno.h>
#include <string.h>

#include <zephyr/ztest.h>

#include "../../../../drivers/split/defy_split_pairing.c"

#define KEY_0 "ble/peripheral_addresses/0"
#define KEY_1 "ble/peripheral_addresses/1"

/* The test's settings store: what settings_load() reads, and what saves and deletes change. */
static struct {
    char name[40];
    uint8_t value[16];
    size_t length;
    bool used;
} entries[8];
static unsigned int saves;
static unsigned int deletes;
static int save_error;
static int delete_error[2];

/* ZMK's own "ble" handler, which the split slots' keys must not reach. */
static char zmk_seen[4][40];
static size_t zmk_seen_count;

static bt_addr_le_t bonds[4];
static size_t bond_count;
static bt_addr_le_t unpaired[4];
static size_t unpair_count;
static int unpair_error;
static void (*unpair_hook)(void);
static unsigned int hold_off_clears;

static const bt_addr_le_t left = {.type = BT_ADDR_LE_RANDOM, .a = {{1, 2, 3, 4, 5, 0xc1}}};
static const bt_addr_le_t right = {.type = BT_ADDR_LE_RANDOM, .a = {{1, 2, 3, 4, 5, 0xc2}}};
static const bt_addr_le_t other = {.type = BT_ADDR_LE_RANDOM, .a = {{1, 2, 3, 4, 5, 0xc3}}};
static const bt_addr_le_t host = {.type = BT_ADDR_LE_PUBLIC, .a = {{9, 8, 7, 6, 5, 4}}};

static int entry_index(const char *name) {
    for (size_t i = 0; i < ARRAY_SIZE(entries); i++) {
        if (entries[i].used && strcmp(entries[i].name, name) == 0) {
            return i;
        }
    }
    return -ENOENT;
}

static void put_entry(const char *name, const void *value, size_t length) {
    int index = entry_index(name);
    for (size_t i = 0; index < 0 && i < ARRAY_SIZE(entries); i++) {
        if (!entries[i].used) {
            index = i;
        }
    }
    zassert_true(index >= 0);
    zassert_true(length <= sizeof(entries[index].value));
    strcpy(entries[index].name, name);
    memcpy(entries[index].value, value, length);
    entries[index].length = length;
    entries[index].used = true;
}

static bool stored(const char *name, const bt_addr_le_t *addr) {
    int index = entry_index(name);
    return index >= 0 && entries[index].length == sizeof(*addr) &&
           memcmp(entries[index].value, addr, sizeof(*addr)) == 0;
}

static ssize_t read_entry(void *cb_arg, void *data, size_t length) {
    const __typeof__(entries[0]) *entry = cb_arg;
    size_t count = MIN(length, entry->length);
    memcpy(data, entry->value, count);
    return count;
}

static int store_load(struct settings_store *cs, const struct settings_load_arg *arg) {
    ARG_UNUSED(cs);
    for (size_t i = 0; i < ARRAY_SIZE(entries); i++) {
        if (entries[i].used) {
            settings_call_set_handler(entries[i].name, entries[i].length, read_entry, &entries[i],
                                      arg);
        }
    }
    return 0;
}

static int store_save(struct settings_store *cs, const char *name, const char *value,
                      size_t length) {
    ARG_UNUSED(cs);
    if (!value || !length) {
        deletes++;
        if (strcmp(name, KEY_0) == 0 && delete_error[0]) {
            return delete_error[0];
        }
        if (strcmp(name, KEY_1) == 0 && delete_error[1]) {
            return delete_error[1];
        }
        int index = entry_index(name);
        if (index >= 0) {
            entries[index].used = false;
        }
        return 0;
    }
    saves++;
    if (save_error) {
        return save_error;
    }
    put_entry(name, value, length);
    return 0;
}

static const struct settings_store_itf store_itf = {
    .csi_load = store_load,
    .csi_save = store_save,
};
static struct settings_store store = {.cs_itf = &store_itf};

int settings_backend_init(void) {
    settings_src_register(&store);
    settings_dst_register(&store);
    return 0;
}

static int zmk_ble_set(const char *name, size_t length, settings_read_cb read_cb, void *cb_arg) {
    ARG_UNUSED(length);
    ARG_UNUSED(read_cb);
    ARG_UNUSED(cb_arg);
    zassert_true(zmk_seen_count < ARRAY_SIZE(zmk_seen));
    strcpy(zmk_seen[zmk_seen_count++], name);
    return 0;
}

static struct settings_handler zmk_ble_handler = {.name = "ble", .h_set = zmk_ble_set};

void bt_foreach_bond(uint8_t id, void (*func)(const struct bt_bond_info *info, void *user_data),
                     void *user_data) {
    zassert_equal(id, BT_ID_DEFAULT);
    for (size_t i = 0; i < bond_count; i++) {
        struct bt_bond_info info = {.addr = bonds[i]};
        func(&info, user_data);
    }
}

int bt_unpair(uint8_t id, const bt_addr_le_t *addr) {
    zassert_equal(id, BT_ID_DEFAULT);
    zassert_not_null(addr);
    zassert_false(bt_addr_le_eq(addr, BT_ADDR_LE_ANY));
    /* The host may wait for the receive thread, which may wait for this lock. */
    zassert_is_null(store_lock.owner);
    zassert_true(unpair_count < ARRAY_SIZE(unpaired));
    unpaired[unpair_count++] = *addr;
    if (unpair_hook) {
        unpair_hook();
    }
    if (unpair_error) {
        return unpair_error;
    }
    for (size_t i = 0; i < bond_count; i++) {
        if (bt_addr_le_eq(&bonds[i], addr)) {
            bonds[i] = bonds[--bond_count];
            break;
        }
    }
    return 0;
}

void defy_hybrid_ble_clear_hold_off(void) { hold_off_clears++; }

static void status(uint8_t first, uint8_t second) {
    uint8_t out[DEFY_SPLIT_PAIRING_SIZE];
    zassert_equal(defy_split_pairing_status(out, sizeof(out)), 4);
    const uint8_t expected[] = {1, 2, first, second};
    zassert_mem_equal(out, expected, sizeof(expected));
}

static void *setup(void) {
    zassert_ok(settings_subsys_init());
    zassert_ok(settings_register(&zmk_ble_handler));
    return NULL;
}

static void before(void *fixture) {
    ARG_UNUSED(fixture);
    memset(slots, 0, sizeof(slots));
    memset(entries, 0, sizeof(entries));
    saves = deletes = 0;
    save_error = 0;
    memset(delete_error, 0, sizeof(delete_error));
    zmk_seen_count = 0;
    bond_count = unpair_count = 0;
    unpair_error = 0;
    unpair_hook = NULL;
    hold_off_clears = 0;
}

ZTEST(defy_split_pairing, test_load_takes_the_slot_keys_from_zmk) {
    const uint8_t short_value[3] = {1, 2, 3};
    const uint8_t profile[4] = {0};
    put_entry(KEY_1, &right, sizeof(right));
    put_entry(KEY_0, short_value, sizeof(short_value));
    put_entry("ble/peripheral_addresses/2", &other, sizeof(other));
    put_entry("ble/peripheral_addresses/10", &other, sizeof(other));
    put_entry("ble/profiles/0", profile, sizeof(profile));
    zassert_ok(settings_load());
    zassert_true(bt_addr_le_eq(&slots[0], BT_ADDR_LE_ANY));
    zassert_true(bt_addr_le_eq(&slots[1], &right));
    zassert_equal(zmk_seen_count, 1);
    zassert_str_equal(zmk_seen[0], "profiles/0");
    status(0, DEFY_SPLIT_PAIRING_ADDRESS);
}

ZTEST(defy_split_pairing, test_new_address_takes_and_saves_the_first_empty_slot) {
    zassert_equal(__wrap_zmk_ble_put_peripheral_addr(&left), 0);
    zassert_true(stored(KEY_0, &left));
    zassert_equal(saves, 1);
    zassert_equal(__wrap_zmk_ble_put_peripheral_addr(&left), 0);
    zassert_equal(saves, 1);
    zassert_equal(__wrap_zmk_ble_put_peripheral_addr(&right), 1);
    zassert_true(stored(KEY_1, &right));
    zassert_equal(__wrap_zmk_ble_put_peripheral_addr(&other), -ENOMEM);
    zassert_equal(saves, 2);
    zassert_true(stored(KEY_0, &left));
    zassert_true(bt_addr_le_eq(&slots[0], &left));
    zassert_true(bt_addr_le_eq(&slots[1], &right));
}

ZTEST(defy_split_pairing, test_known_address_keeps_its_slot_past_an_empty_one) {
    put_entry(KEY_1, &right, sizeof(right));
    zassert_ok(settings_load());
    zassert_equal(__wrap_zmk_ble_put_peripheral_addr(&right), 1);
    zassert_equal(saves, 0);
    zassert_equal(__wrap_zmk_ble_put_peripheral_addr(&left), 0);
    zassert_true(stored(KEY_0, &left));
}

ZTEST(defy_split_pairing, test_failed_save_still_reserves_the_slot) {
    save_error = -EIO;
    zassert_equal(__wrap_zmk_ble_put_peripheral_addr(&left), 0);
    zassert_equal(saves, 1);
    zassert_equal(entry_index(KEY_0), -ENOENT);
    zassert_equal(__wrap_zmk_ble_put_peripheral_addr(&left), 0);
    zassert_equal(saves, 1);
}

ZTEST(defy_split_pairing, test_status_reports_each_slot_address_and_bond) {
    status(0, 0);
    zassert_equal(__wrap_zmk_ble_put_peripheral_addr(&left), 0);
    status(DEFY_SPLIT_PAIRING_ADDRESS, 0);
    zassert_equal(__wrap_zmk_ble_put_peripheral_addr(&right), 1);
    bonds[bond_count++] = host;
    bonds[bond_count++] = right;
    status(DEFY_SPLIT_PAIRING_ADDRESS, DEFY_SPLIT_PAIRING_ADDRESS | DEFY_SPLIT_PAIRING_BONDED);
    bonds[bond_count++] = left;
    status(3, 3);
    uint8_t out[DEFY_SPLIT_PAIRING_SIZE] = {0};
    zassert_equal(defy_split_pairing_status(out, sizeof(out) - 1), 0);
    zassert_equal(defy_split_pairing_status(NULL, sizeof(out)), 0);
    zassert_equal(out[0], 0);
}

ZTEST(defy_split_pairing, test_bond_without_a_slot_is_not_reported) {
    bonds[bond_count++] = left;
    status(0, 0);
}

ZTEST(defy_split_pairing, test_clear_removes_the_bonds_then_empties_the_slots) {
    zassert_equal(__wrap_zmk_ble_put_peripheral_addr(&left), 0);
    zassert_equal(__wrap_zmk_ble_put_peripheral_addr(&right), 1);
    bonds[bond_count++] = left;
    bonds[bond_count++] = host;
    bonds[bond_count++] = right;
    zassert_ok(defy_split_pairing_clear());
    zassert_equal(unpair_count, 2);
    zassert_true(bt_addr_le_eq(&unpaired[0], &left));
    zassert_true(bt_addr_le_eq(&unpaired[1], &right));
    zassert_equal(bond_count, 1);
    zassert_true(bt_addr_le_eq(&bonds[0], &host));
    zassert_equal(deletes, 2);
    zassert_equal(entry_index(KEY_0), -ENOENT);
    zassert_equal(entry_index(KEY_1), -ENOENT);
    zassert_equal(hold_off_clears, 1);
    status(0, 0);
    zassert_equal(__wrap_zmk_ble_put_peripheral_addr(&other), 0);
    zassert_true(stored(KEY_0, &other));
}

ZTEST(defy_split_pairing, test_clear_unpairs_only_the_addresses_held) {
    put_entry(KEY_1, &right, sizeof(right));
    zassert_ok(settings_load());
    zassert_ok(defy_split_pairing_clear());
    zassert_equal(unpair_count, 1);
    zassert_true(bt_addr_le_eq(&unpaired[0], &right));
    zassert_equal(deletes, 2);
    zassert_equal(entry_index(KEY_1), -ENOENT);
    status(0, 0);
    zassert_ok(defy_split_pairing_clear());
    zassert_equal(unpair_count, 1);
    zassert_equal(hold_off_clears, 2);
}

ZTEST(defy_split_pairing, test_failed_bond_removal_leaves_the_slots) {
    zassert_equal(__wrap_zmk_ble_put_peripheral_addr(&left), 0);
    zassert_equal(__wrap_zmk_ble_put_peripheral_addr(&right), 1);
    unpair_error = -EIO;
    zassert_equal(defy_split_pairing_clear(), -EIO);
    zassert_equal(unpair_count, 1);
    zassert_equal(deletes, 0);
    zassert_equal(hold_off_clears, 0);
    zassert_true(stored(KEY_0, &left));
    zassert_true(stored(KEY_1, &right));
    status(DEFY_SPLIT_PAIRING_ADDRESS, DEFY_SPLIT_PAIRING_ADDRESS);
}

ZTEST(defy_split_pairing, test_failed_delete_keeps_that_slot_for_the_next_clear) {
    zassert_equal(__wrap_zmk_ble_put_peripheral_addr(&left), 0);
    zassert_equal(__wrap_zmk_ble_put_peripheral_addr(&right), 1);
    delete_error[1] = -EIO;
    zassert_equal(defy_split_pairing_clear(), -EIO);
    zassert_equal(entry_index(KEY_0), -ENOENT);
    zassert_true(stored(KEY_1, &right));
    status(0, DEFY_SPLIT_PAIRING_ADDRESS);
    delete_error[1] = 0;
    zassert_ok(defy_split_pairing_clear());
    zassert_equal(unpair_count, 3);
    zassert_true(bt_addr_le_eq(&unpaired[2], &right));
    zassert_equal(entry_index(KEY_1), -ENOENT);
    status(0, 0);
}

static void reserve_during_unpair(void) {
    if (unpair_count == 1) {
        zassert_equal(__wrap_zmk_ble_put_peripheral_addr(&other), 1);
        zassert_true(stored(KEY_1, &other));
    }
}

ZTEST(defy_split_pairing, test_slot_reserved_during_the_clear_leaves_storage_matching) {
    zassert_equal(__wrap_zmk_ble_put_peripheral_addr(&left), 0);
    unpair_hook = reserve_during_unpair;
    zassert_ok(defy_split_pairing_clear());
    zassert_equal(unpair_count, 1);
    status(0, 0);
    zassert_equal(entry_index(KEY_0), -ENOENT);
    zassert_equal(entry_index(KEY_1), -ENOENT);
}

ZTEST_SUITE(defy_split_pairing, NULL, setup, before, NULL, NULL);
