/*
 * The Neuron's status LED (status/defy_neuron_status.c, docs/lighting.md).
 *
 * SPDX-License-Identifier: MIT
 */

#ifndef DYGMA_DEFY_NEURON_STATUS_H
#define DYGMA_DEFY_NEURON_STATUS_H

/* Shows the current state on the LED, from the system work queue. */
void defy_neuron_status_update(void);

#endif
