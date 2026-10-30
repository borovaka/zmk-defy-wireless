/* SPDX-License-Identifier: MIT */

#ifndef DYGMA_DEFY_KEYMAP_CHANGES_H
#define DYGMA_DEFY_KEYMAP_CHANGES_H

/*
 * The half updater's CLEAR KEYMAP CHANGES, in updater/defy_keymap_changes.c
 * with CONFIG_ZMK_KEYMAP_SETTINGS_STORAGE: deletes the keymap changes ZMK
 * Studio saved, then reloads the built-in keymap. -EBUSY while Studio is
 * unlocked; otherwise the first error, and the reload runs either way.
 */
int defy_keymap_changes_clear(void);

#endif
