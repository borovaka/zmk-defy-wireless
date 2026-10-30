/* SPDX-License-Identifier: MIT */

#include <dygma/defy_loader.h>

#include "boot_mode.h"
#include "layout.h"

/* GPREGRET holds 8 bits, and the drive request is not a count of starts. */
_Static_assert(DEFY_LOADER_STARTS(DEFY_LOADER_START_LIMIT) <= 0xffu, "The count fits GPREGRET");
_Static_assert(DEFY_LOADER_DRIVE_MAGIC != 0 &&
                   (DEFY_LOADER_DRIVE_MAGIC <= DEFY_LOADER_STARTS_BASE ||
                    DEFY_LOADER_DRIVE_MAGIC > DEFY_LOADER_STARTS(DEFY_LOADER_START_LIMIT)),
               "The drive request differs from every count");

bool defy_loader_zmk_valid(uint32_t stack, uint32_t reset) {
    uint32_t entry = reset & ~1u;
    return stack % 4 == 0 && stack > DEFY_LOADER_RAM_START && stack <= DEFY_LOADER_RAM_END &&
           (reset & 1u) != 0 && entry >= DEFY_LOADER_ZMK_START && entry < DEFY_LOADER_ZMK_END;
}

enum defy_loader_start defy_loader_start_mode(uint32_t gpregret, uint32_t stack, uint32_t reset) {
    if (gpregret == DEFY_LOADER_DRIVE_MAGIC) {
        return DEFY_LOADER_DRIVE_REQUESTED;
    }
    if (!defy_loader_zmk_valid(stack, reset)) {
        return DEFY_LOADER_DRIVE_NO_ZMK;
    }
    return defy_loader_starts(gpregret) < DEFY_LOADER_START_LIMIT ? DEFY_LOADER_START_ZMK
                                                                  : DEFY_LOADER_DRIVE_ZMK_FAILED;
}

uint32_t defy_loader_start_gpregret(enum defy_loader_start mode, uint32_t gpregret) {
    switch (mode) {
    case DEFY_LOADER_START_ZMK:
        return DEFY_LOADER_STARTS(defy_loader_starts(gpregret) + 1);
    case DEFY_LOADER_DRIVE_ZMK_FAILED:
        return DEFY_LOADER_STARTS(DEFY_LOADER_START_LIMIT);
    default:
        return 0;
    }
}
