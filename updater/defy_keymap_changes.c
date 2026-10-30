/* SPDX-License-Identifier: MIT */

#include <errno.h>
#include <stdio.h>

#include <zephyr/settings/settings.h>
#include <zephyr/sys/util.h>
#include <zephyr/toolchain.h>
#include <zmk/keymap.h>
#include <zmk/matrix.h>
#if IS_ENABLED(CONFIG_ZMK_STUDIO)
#include <zmk/studio/core.h>
#endif

#include <dygma/defy_keymap_changes.h>

/*
 * ZMK's keymap.c saves Studio's changes under this settings subtree: each binding as
 * l/<layer>/<position>, each layer name as l_n/<layer> and the layer order as layer_order, the
 * numbers as %d writes them.
 */
#define KEYMAP_SUBTREE "keymap"
#define KEYMAP_NAME_SIZE (SETTINGS_MAX_NAME_LEN + 1)

/* The keys found under KEYMAP_SUBTREE, all collected before any is deleted. */
struct keymap_keys {
    uint8_t bindings[ZMK_KEYMAP_LAYERS_LEN][DIV_ROUND_UP(ZMK_KEYMAP_LEN, 8)];
    uint8_t names[DIV_ROUND_UP(ZMK_KEYMAP_LAYERS_LEN, 8)];
    bool layer_order;
    /* Keys in another form, or beyond this keymap's layers and positions, as an image with a
     * larger keymap leaves. */
    unsigned int others;
};

/* A number below limit, as %d writes it. Returns the character after it, or NULL. */
static const char *key_number(const char *text, uint32_t limit, uint32_t *value) {
    const char *digit = text;
    uint32_t number = 0;
    while (*digit >= '0' && *digit <= '9') {
        number = number * 10 + (*digit++ - '0');
        if (number >= limit) {
            return NULL;
        }
    }
    if (digit == text || (text[0] == '0' && digit - text > 1)) {
        return NULL;
    }
    *value = number;
    return digit;
}

static bool mark_keymap_key(struct keymap_keys *keys, const char *key) {
    const char *next;
    uint32_t layer;
    uint32_t position;
    if (settings_name_steq(key, "layer_order", &next) && !next) {
        keys->layer_order = true;
        return true;
    }
    if (settings_name_steq(key, "l_n", &next) && next) {
        next = key_number(next, ZMK_KEYMAP_LAYERS_LEN, &layer);
        if (!next || *next) {
            return false;
        }
        WRITE_BIT(keys->names[layer / 8], layer % 8, 1);
        return true;
    }
    if (settings_name_steq(key, "l", &next) && next) {
        next = key_number(next, ZMK_KEYMAP_LAYERS_LEN, &layer);
        next = next && *next == '/' ? key_number(next + 1, ZMK_KEYMAP_LEN, &position) : NULL;
        if (!next || *next) {
            return false;
        }
        WRITE_BIT(keys->bindings[layer][position / 8], position % 8, 1);
        return true;
    }
    return false;
}

static int collect_keymap_key(const char *key, size_t length, settings_read_cb read_cb,
                              void *cb_arg, void *param) {
    ARG_UNUSED(length);
    ARG_UNUSED(read_cb);
    ARG_UNUSED(cb_arg);
    struct keymap_keys *keys = param;
    if (!mark_keymap_key(keys, key)) {
        keys->others++;
    }
    return 0;
}

/* Takes the first key and stops the load there. */
static int take_keymap_key(const char *key, size_t length, settings_read_cb read_cb,
                           void *cb_arg, void *param) {
    ARG_UNUSED(length);
    ARG_UNUSED(read_cb);
    ARG_UNUSED(cb_arg);
    snprintf(param, KEYMAP_NAME_SIZE, KEYMAP_SUBTREE "/%s", key);
    return 1;
}

static int delete_key(int result, const char *name) {
    int err = settings_delete(name);
    return result ? result : err;
}

/* Deletes every key under KEYMAP_SUBTREE and returns the first error. Out of line, so that the
 * bitmap is off the stack while ZMK reloads the keymap. */
static __noinline int delete_keymap_keys(void) {
    struct keymap_keys keys = {0};
    char name[KEYMAP_NAME_SIZE];
    int result = settings_load_subtree_direct(KEYMAP_SUBTREE, collect_keymap_key, &keys);
    if (result) {
        return result;
    }
    if (keys.layer_order) {
        result = delete_key(result, KEYMAP_SUBTREE "/layer_order");
    }
    for (int layer = 0; layer < ZMK_KEYMAP_LAYERS_LEN; layer++) {
        if (keys.names[layer / 8] & BIT(layer % 8)) {
            snprintf(name, sizeof(name), KEYMAP_SUBTREE "/l_n/%d", layer);
            result = delete_key(result, name);
        }
        for (int position = 0; position < ZMK_KEYMAP_LEN; position++) {
            if (keys.bindings[layer][position / 8] & BIT(position % 8)) {
                snprintf(name, sizeof(name), KEYMAP_SUBTREE "/l/%d/%d", layer, position);
                result = delete_key(result, name);
            }
        }
    }
    /* The others by name, one load each. With the keys above gone, each load finds one. */
    for (unsigned int i = 0; !result && i < keys.others; i++) {
        name[0] = '\0';
        settings_load_subtree_direct(KEYMAP_SUBTREE, take_keymap_key, name);
        if (!name[0]) {
            break;
        }
        result = settings_delete(name);
    }
    return result;
}

/*
 * ZMK's zmk_keymap_reset_settings() deletes a saved binding only when the binding in use differs
 * from the built-in one within its first 10 bytes, so it can miss a change to a parameter, such
 * as a mod-tap's tap key. So the keys go first, and ZMK's function only reloads the built-in
 * keymap and layer order.
 */
int defy_keymap_changes_clear(void) {
#if IS_ENABLED(CONFIG_ZMK_STUDIO)
    /* While Studio is unlocked, its thread may change the keymap at any time. */
    if (zmk_studio_core_get_lock_state() != ZMK_STUDIO_CORE_LOCK_STATE_LOCKED) {
        return -EBUSY;
    }
#endif
    int err = delete_keymap_keys();
    int reload = zmk_keymap_reset_settings();
    return err ? err : reload;
}
