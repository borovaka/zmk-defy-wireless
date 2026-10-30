/* SPDX-License-Identifier: MIT */

#ifndef DYGMA_DEFY_PROFILE_CHOOSER_H
#define DYGMA_DEFY_PROFILE_CHOOSER_H

#include <dygma/defy_feedback.h>
#include <zmk/events/position_state_changed.h>

void defy_profile_chooser_toggle(void);
bool defy_profile_chooser_process(const struct zmk_position_state_changed *event);
void defy_profile_chooser_feedback(struct defy_host_feedback *state);

#endif
