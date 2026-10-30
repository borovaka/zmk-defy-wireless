/* SPDX-License-Identifier: MIT */

#include <dygma/defy_half_version.h>
#include <dygma/defy_wired_frame.h>
#include <dygma/defy_wired_protocol.h>

#include <string.h>

/* The record's fields, as docs/versions.md lays them out. */
#define FORMAT_OFFSET 0u
#define SIDE_OFFSET 1u
#define PROGRAM_CRC_OFFSET 2u
#define SEAL_VERSION_OFFSET 6u
#define MODULE_OFFSET 10u

_Static_assert(MODULE_OFFSET + DEFY_HALF_VERSION_MODULE_SIZE == DEFY_HALF_VERSION_SIZE,
               "the module version ends the record");

void defy_half_version_encode(const struct defy_half_version *version,
                              uint8_t record[DEFY_HALF_VERSION_SIZE]) {
    memset(record, 0, DEFY_HALF_VERSION_SIZE);
    record[FORMAT_OFFSET] = DEFY_HALF_VERSION_FORMAT;
    record[SIDE_OFFSET] = version->side;
    defy_wired_put32(record + PROGRAM_CRC_OFFSET, version->program_crc);
    defy_wired_put32(record + SEAL_VERSION_OFFSET, version->seal_version);
    for (size_t i = 0; i < DEFY_HALF_VERSION_MODULE_SIZE && version->module[i] != '\0'; i++) {
        record[MODULE_OFFSET + i] = (uint8_t)version->module[i];
    }
}

bool defy_half_version_decode(const uint8_t record[DEFY_HALF_VERSION_SIZE],
                              struct defy_half_version *version) {
    if (record == NULL || version == NULL || record[FORMAT_OFFSET] != DEFY_HALF_VERSION_FORMAT ||
        record[SIDE_OFFSET] > DEFY_WIRED_RIGHT) {
        return false;
    }
    struct defy_half_version decoded = {
        .side = record[SIDE_OFFSET],
        .program_crc = defy_wired_get32(record + PROGRAM_CRC_OFFSET),
        .seal_version = defy_wired_get32(record + SEAL_VERSION_OFFSET),
    };
    /* Printable ASCII, then zero bytes to the end. */
    bool padding = false;
    for (size_t i = 0; i < DEFY_HALF_VERSION_MODULE_SIZE; i++) {
        uint8_t byte = record[MODULE_OFFSET + i];
        if (byte == 0) {
            padding = true;
        } else if (padding || byte < 0x20 || byte > 0x7e) {
            return false;
        } else {
            decoded.module[i] = (char)byte;
        }
    }
    *version = decoded;
    return true;
}
