/* SPDX-License-Identifier: MIT */

#include <zmk/events/position_state_changed.h>

int keymap_position(const zmk_event_t *event);

/* Its own source, linked after main.c, as ZMK's listeners link after the module's. */
ZMK_LISTENER(chooser_test_keymap, keymap_position);
ZMK_SUBSCRIPTION(chooser_test_keymap, zmk_position_state_changed);
