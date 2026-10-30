/*
 * The drive's FAT16 volume, labelled DEFY, generated on each read: a boot
 * sector, two FATs, a root directory with INFO_UF2.TXT and INDEX.HTM, and
 * free clusters for the host's copy of a UF2 file. Nothing a host writes
 * changes it; uf2.c takes the UF2 blocks out of those writes.
 *
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include <stdint.h>

/* The loader's version, in INFO_UF2.TXT and in the drive's SCSI INQUIRY data. */
#define DEFY_LOADER_VERSION "1.1"
/* INFO_UF2.TXT's Board-ID, which the zmk variant's uf2 runner matches
 * (boards/dygma/defy_neuron/board.cmake): chip, board and revision, in the form
 * Adafruit's and Microsoft's UF2 bootloaders use. */
#define DEFY_LOADER_BOARD_ID "nRF52833-DefyNeuron-v1"

#define DEFY_FAT_SECTOR_SIZE 512u
/* 4 MiB. Its 8095 one-sector clusters make it FAT16, and all but two are free: a UF2 file
 * of ZMK's whole range takes 1520. */
#define DEFY_FAT_SECTOR_COUNT 8192u

/* Fills buffer, DEFY_FAT_SECTOR_SIZE bytes, with the sector; zeros past the end. */
void defy_fat_read(uint32_t sector, uint8_t *buffer);
