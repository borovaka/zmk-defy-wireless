/*
 * Which halves are cabled to the Neuron, and where the Neuron is, from the
 * halves' RUN lines and USB (status/defy_neuron_dock.c).
 *
 * SPDX-License-Identifier: MIT
 */

#ifndef DYGMA_DEFY_NEURON_DOCK_H
#define DYGMA_DEFY_NEURON_DOCK_H

#include <stddef.h>
#include <stdint.h>

#define DEFY_NEURON_CABLE_LEFT (1u << 0)
#define DEFY_NEURON_CABLE_RIGHT (1u << 1)
#define DEFY_NEURON_DOCK_STATUS_VERSION 1u
#define DEFY_NEURON_DOCK_STATUS_SIZE 8u

enum defy_neuron_place {
    DEFY_NEURON_ALONE,
    DEFY_NEURON_CHARGER,
    /*
     * Exactly one cable line high and no computer. The dock raises the right
     * half's line, and a single cable of either half on a charger reads the
     * same, as in stock firmware.
     */
    DEFY_NEURON_DOCKED,
    DEFY_NEURON_COMPUTER,
};

/* The halves whose cables are in, debounced: DEFY_NEURON_CABLE_LEFT and _RIGHT. */
uint8_t defy_neuron_cables(void);
enum defy_neuron_place defy_neuron_place(void);
/* The updater's dock snapshot. Returns its length, or 0 if out is too small. */
size_t defy_neuron_dock_status(uint8_t *out, size_t capacity);

#endif
