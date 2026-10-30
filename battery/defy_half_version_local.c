/*
 * A half's own version report: its side, its program's CRC32 and version from
 * the seal that starts its package, and the module version it was built from.
 * The cable and the control service send it; docs/versions.md describes it.
 *
 * SPDX-License-Identifier: MIT
 */

#include <string.h>

#include <zephyr/storage/flash_map.h>
#include <zephyr/sys/util.h>

#include <dygma/defy_build_info.h>
#include <dygma/defy_half.h>
#include <dygma/defy_half_package.h>
#include <dygma/defy_half_version.h>

BUILD_ASSERT(FIXED_PARTITION_OFFSET(factory_seal) == DEFY_HALF_FLASH_START,
             "the seal starts the half's package");

/* The seal's program CRC32 and version: false without a valid seal. */
static bool read_seal(struct defy_half_seal *parsed) {
    const struct flash_area *area;
    uint8_t seal[DEFY_HALF_SEAL_SIZE];

    if (flash_area_open(FIXED_PARTITION_ID(factory_seal), &area) < 0) {
        return false;
    }
    int err = flash_area_read(area, 0, seal, sizeof(seal));

    flash_area_close(area);
    return err == 0 && defy_half_seal_parse(seal, parsed);
}

void defy_half_version_local(struct defy_half_version *version) {
    struct defy_half_seal seal;
    bool sealed = read_seal(&seal);

    *version = (struct defy_half_version){
        .side = DEFY_HALF_ID,
        .program_crc = sealed ? seal.program_crc : 0,
        .seal_version = sealed ? seal.program_version : 0,
    };
    memcpy(version->module, DEFY_BUILD_MODULE_VERSION,
           MIN(sizeof(DEFY_BUILD_MODULE_VERSION) - 1, DEFY_HALF_VERSION_MODULE_SIZE));
}
