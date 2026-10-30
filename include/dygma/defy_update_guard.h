/* SPDX-License-Identifier: MIT */

#ifndef DYGMA_DEFY_UPDATE_GUARD_H
#define DYGMA_DEFY_UPDATE_GUARD_H

#include <stdbool.h>

/* One owner at a time may hold the Neuron for an update: the half updater, or a restart into a
 * bootloader, by the 1200-baud entry or &defy_drive. */
enum defy_update_owner {
    DEFY_UPDATE_HALVES = 1,
    DEFY_UPDATE_NEURON,
};

bool defy_update_claim(enum defy_update_owner owner);
void defy_update_release(enum defy_update_owner owner);
bool defy_update_held(enum defy_update_owner owner);

#endif
