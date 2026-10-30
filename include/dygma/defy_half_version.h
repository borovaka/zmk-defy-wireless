/* SPDX-License-Identifier: MIT */

#ifndef DYGMA_DEFY_HALF_VERSION_H
#define DYGMA_DEFY_HALF_VERSION_H

/*
 * A half's report of what it runs: its side, its program's CRC32 and seal
 * version, and the module version it was built from. It travels as a
 * Bluetooth characteristic and in 'DV' cable frames; the Neuron keeps the
 * latest per side for the half updater's HALF VERSIONS. docs/versions.md
 * describes it. The record functions are plain C, so the host tests build
 * them without Zephyr.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <dygma/defy_control.h>

#define DEFY_HALF_VERSION_FORMAT 1u
#define DEFY_HALF_VERSION_SIZE 28u
#define DEFY_HALF_VERSION_MODULE_SIZE 18u

/* HALF VERSIONS's payload: an 8-byte header, then one 36-byte record per side. */
#define DEFY_HALF_VERSIONS_VERSION 1u
#define DEFY_HALF_VERSIONS_HEADER 8u
#define DEFY_HALF_VERSIONS_RECORD 36u
#define DEFY_HALF_VERSIONS_SIZE (DEFY_HALF_VERSIONS_HEADER + 2u * DEFY_HALF_VERSIONS_RECORD)

struct defy_half_version {
    /* DEFY_WIRED_LEFT or DEFY_WIRED_RIGHT. */
    uint8_t side;
    /* From the half's seal; both 0 without a valid seal. */
    uint32_t program_crc;
    uint32_t seal_version;
    /* The module version, cut to DEFY_HALF_VERSION_MODULE_SIZE bytes. */
    char module[DEFY_HALF_VERSION_MODULE_SIZE + 1];
};

void defy_half_version_encode(const struct defy_half_version *version,
                              uint8_t record[DEFY_HALF_VERSION_SIZE]);
/* False for another format, a side other than left or right, or a module
 * version with a byte outside printable ASCII before its padding. */
bool defy_half_version_decode(const uint8_t record[DEFY_HALF_VERSION_SIZE],
                              struct defy_half_version *version);

/* A half's own report, from its seal and <dygma/defy_build_info.h>. */
void defy_half_version_local(struct defy_half_version *version);

/*
 * The Neuron's records. Over Bluetooth, slot is the split slot the record came
 * by. A record holds until the link it came by goes down for its side.
 */
void defy_half_version_received(enum defy_link link, uint8_t slot,
                                const struct defy_half_version *version);
void defy_half_version_wired_down(uint8_t side);
void defy_half_version_ble_down(uint8_t slot);
/* HALF VERSIONS's payload: DEFY_HALF_VERSIONS_SIZE bytes, or 0 when capacity
 * is smaller. */
size_t defy_half_versions(uint8_t *output, size_t capacity);

#endif
