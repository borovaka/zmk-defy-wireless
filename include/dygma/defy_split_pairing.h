/* SPDX-License-Identifier: MIT */

#ifndef DYGMA_DEFY_SPLIT_PAIRING_H
#define DYGMA_DEFY_SPLIT_PAIRING_H

/*
 * The Neuron's side of the split pairing: the split slot of each half, which
 * holds the half's address from its first connection attempt on, and the
 * bond for that address. docs/half-updater.md describes how the half updater
 * reports and resets it.
 */

#include <stddef.h>
#include <stdint.h>

/* PAIRING's reply: the version, the slot count, then each slot's flags. */
#define DEFY_SPLIT_PAIRING_VERSION 1u
#define DEFY_SPLIT_PAIRING_SLOTS 2u
#define DEFY_SPLIT_PAIRING_SIZE (2u + DEFY_SPLIT_PAIRING_SLOTS)
/* The slot holds a half's address. */
#define DEFY_SPLIT_PAIRING_ADDRESS 0x01u
/* The Neuron holds a bond for that address. */
#define DEFY_SPLIT_PAIRING_BONDED 0x02u

/* Writes PAIRING's reply and returns its size, or 0 if it does not fit. */
size_t defy_split_pairing_status(uint8_t *out, size_t capacity);

/*
 * Removes the bond for each slot's address, then empties both slots and
 * deletes their stored addresses. A failed bond removal leaves the slots as
 * they were. A slot whose stored address cannot be deleted keeps its address.
 * Call it only while both halves are held in their bootloaders, as RESET
 * PAIRING does, so that neither can reconnect and take a slot while the slots
 * are cleared.
 */
int defy_split_pairing_clear(void);

#endif
