/*
 * The loader's start decision, made by start.c before anything else runs,
 * from GPREGRET (<dygma/defy_loader.h>) and ZMK's vector table.
 *
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>

enum defy_loader_start {
    /* ZMK's vector table is valid and its unconfirmed starts are below the limit:
     * start ZMK. */
    DEFY_LOADER_START_ZMK,
    /* GPREGRET holds the drive request: show the drive. */
    DEFY_LOADER_DRIVE_REQUESTED,
    /* No valid ZMK: show the drive. */
    DEFY_LOADER_DRIVE_NO_ZMK,
    /* ZMK started DEFY_LOADER_START_LIMIT times in a row without confirming: show
     * the drive. */
    DEFY_LOADER_DRIVE_ZMK_FAILED,
};

/*
 * True if stack and reset, the first two words at ZMK's start, can start ZMK:
 * a word-aligned stack pointer inside RAM, and a Thumb reset vector inside
 * ZMK's range.
 */
bool defy_loader_zmk_valid(uint32_t stack, uint32_t reset);

/* What to do with GPREGRET's value and ZMK's first two words. */
enum defy_loader_start defy_loader_start_mode(uint32_t gpregret, uint32_t stack, uint32_t reset);

/*
 * GPREGRET's value for a start in mode, from its value before: one more
 * unconfirmed start for ZMK, the limit kept while ZMK has failed, and 0 when
 * the drive shows for any other reason. With the limit kept, drive mode tells
 * a failed ZMK from a requested drive, and a reset after it, other than one
 * that clears GPREGRET, shows the drive again.
 */
uint32_t defy_loader_start_gpregret(enum defy_loader_start mode, uint32_t gpregret);
