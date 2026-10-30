/*
 * A half's application package, as scripts/package_factory.py builds it and
 * Dygma's RP2040 bootloader checks it, and the flash it goes to.
 *
 * The package starts with a seal of seven little-endian words: format 1, the
 * seal's size, the CRC32 of the seal with this word zeroed, then the program's
 * start, size, CRC32 and version. The program starts with a page of metadata,
 * then the vector table. The metadata is the magic "DFYRP204" and six
 * little-endian words: format 1, the metadata's size, the role, the side, the
 * program's version and a reserved 0. The rest of the page is zero.
 *
 * SPDX-License-Identifier: MIT
 */

#ifndef DYGMA_DEFY_HALF_PACKAGE_H
#define DYGMA_DEFY_HALF_PACKAGE_H

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include <zephyr/sys/byteorder.h>
#include <zephyr/sys/crc.h>

#include <dygma/defy_wired_protocol.h>

/* The application area: the seal, then the program, up to ZMK's settings. */
#define DEFY_HALF_FLASH_START 0x5000u
#define DEFY_HALF_PROGRAM_START 0x6000u
#define DEFY_HALF_FLASH_END 0x1c0000u
/* The start of ZMK's settings: its eight 4 KiB NVS sectors, the half's Bluetooth identity and
 * bonds among them. Only a pairing reset erases them. */
#define DEFY_HALF_SETTINGS_START DEFY_HALF_FLASH_END
#define DEFY_HALF_SETTINGS_END 0x1c8000u
#define DEFY_HALF_PAGE_SIZE 256u
#define DEFY_HALF_SECTOR_SIZE 4096u

#define DEFY_HALF_SEAL_SIZE 28u
/* The metadata page, then the initial stack pointer and reset vector. */
#define DEFY_HALF_PROGRAM_MIN_SIZE (DEFY_HALF_PAGE_SIZE + 8u)

#define DEFY_HALF_METADATA_MAGIC "DFYRP204"
#define DEFY_HALF_METADATA_SIZE 32u
#define DEFY_HALF_METADATA_KEYBOARD 1u
/* The metadata's side ids, the reverse of enum defy_wired_side's. */
#define DEFY_HALF_METADATA_RIGHT 0u
#define DEFY_HALF_METADATA_LEFT 1u

/* The program's place, CRC32 and version, from the seal. */
struct defy_half_seal {
    uint32_t program_start;
    uint32_t program_size;
    uint32_t program_crc;
    uint32_t program_version;
};

/*
 * Checks a seal's format, size, CRC32 and program bounds. Fills *parsed and
 * returns true only for a valid seal.
 */
static inline bool defy_half_seal_parse(const uint8_t seal[DEFY_HALF_SEAL_SIZE],
                                        struct defy_half_seal *parsed) {
    uint8_t zeroed[DEFY_HALF_SEAL_SIZE];

    memcpy(zeroed, seal, sizeof(zeroed));
    sys_put_le32(0, zeroed + 8);

    const struct defy_half_seal fields = {
        .program_start = sys_get_le32(seal + 12),
        .program_size = sys_get_le32(seal + 16),
        .program_crc = sys_get_le32(seal + 20),
        .program_version = sys_get_le32(seal + 24),
    };

    if (sys_get_le32(seal) != 1 || sys_get_le32(seal + 4) != DEFY_HALF_SEAL_SIZE ||
        crc32_ieee(zeroed, sizeof(zeroed)) != sys_get_le32(seal + 8) ||
        fields.program_start != DEFY_HALF_PROGRAM_START ||
        fields.program_size < DEFY_HALF_PROGRAM_MIN_SIZE ||
        fields.program_size > DEFY_HALF_FLASH_END - DEFY_HALF_PROGRAM_START) {
        return false;
    }
    *parsed = fields;
    return true;
}

/* Checks that a metadata page is a keyboard program's for this side and version. */
static inline bool defy_half_metadata_matches(const uint8_t page[DEFY_HALF_METADATA_SIZE],
                                              enum defy_wired_side side, uint32_t version) {
    uint32_t side_id = side == DEFY_WIRED_LEFT ? DEFY_HALF_METADATA_LEFT : DEFY_HALF_METADATA_RIGHT;

    return memcmp(page, DEFY_HALF_METADATA_MAGIC, 8) == 0 && sys_get_le32(page + 8) == 1 &&
           sys_get_le32(page + 12) == DEFY_HALF_METADATA_SIZE &&
           sys_get_le32(page + 16) == DEFY_HALF_METADATA_KEYBOARD &&
           sys_get_le32(page + 20) == side_id && sys_get_le32(page + 24) == version &&
           sys_get_le32(page + 28) == 0;
}

#endif
