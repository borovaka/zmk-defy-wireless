/* SPDX-License-Identifier: MIT */

#include <zmk/events/position_state_changed.h>

int consume_position(const zmk_event_t *event);

/* Its own source, linked after main.c, as ZMK's listeners link after the module's. */
ZMK_LISTENER(lighting_test_consumer, consume_position);
ZMK_SUBSCRIPTION(lighting_test_consumer, zmk_position_state_changed);
