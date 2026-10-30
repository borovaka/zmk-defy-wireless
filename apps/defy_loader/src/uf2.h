/*
 * UF2 blocks for ZMK's range, and the state of the file they come from.
 *
 * A UF2 file is a run of 512-byte blocks, each carrying 256 bytes for one
 * flash address. The host writes the file's blocks to the drive in any order,
 * among the FAT, directory and metadata sectors of its own. defy_uf2_parse()
 * picks out the blocks the loader writes: block n of a file carries the
 * 256 bytes at ZMK's start plus n * 256, as Zephyr's UF2 output does.
 * defy_uf2_file_add() writes them: each 4 KiB page is erased before its first
 * write in a file, and ZMK's first page, which holds its vector table, is
 * erased when the file starts and written last, once every block of the file
 * has arrived. Until then ZMK's vector table stays erased, so an interrupted
 * copy leaves the drive showing at the next start.
 *
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "layout.h"

#define DEFY_UF2_BLOCK_SIZE 512u
#define DEFY_UF2_PAYLOAD_SIZE 256u
/* Zephyr's UF2 family ID for the nRF52833. */
#define DEFY_UF2_FAMILY 0x621e937au
/* A file has at most one block for each 256 bytes of ZMK's range. */
#define DEFY_UF2_MAX_BLOCKS ((DEFY_LOADER_ZMK_END - DEFY_LOADER_ZMK_START) / DEFY_UF2_PAYLOAD_SIZE)
#define DEFY_UF2_PAGES ((DEFY_LOADER_ZMK_END - DEFY_LOADER_ZMK_START) / DEFY_LOADER_PAGE_SIZE)

struct defy_uf2_block {
    uint32_t address;
    uint32_t block_no;
    uint32_t num_blocks;
    /* DEFY_UF2_PAYLOAD_SIZE bytes, inside the sector the block came from. */
    const uint8_t *data;
};

enum defy_uf2_check {
    /* A block for ZMK's range, filled in. */
    DEFY_UF2_BLOCK,
    /* No UF2 start magics: a FAT, directory or metadata sector. */
    DEFY_UF2_NOT_UF2,
    /* A UF2 block the loader does not write. */
    DEFY_UF2_REFUSED,
};

/*
 * Checks a sector of DEFY_UF2_BLOCK_SIZE bytes. A block passes with all three
 * magics, the family ID flag and DEFY_UF2_FAMILY, neither the not-main-flash
 * nor the file-container flag, a 256-byte payload, at most DEFY_UF2_MAX_BLOCKS
 * blocks, its number below that count, and the address ZMK's start plus its
 * number times 256. block is filled in only for DEFY_UF2_BLOCK.
 */
enum defy_uf2_check defy_uf2_parse(const uint8_t *sector, struct defy_uf2_block *block);

/*
 * The flash, by absolute address. Each returns 0 or a negative error. erase
 * takes a page-aligned address and erases that one page. The writer calls
 * them only for addresses inside ZMK's range.
 */
struct defy_uf2_flash {
    int (*erase)(void *context, uint32_t address);
    int (*write)(void *context, uint32_t address, const void *data, size_t length);
    int (*read)(void *context, uint32_t address, void *data, size_t length);
    void *context;
};

enum defy_uf2_file_state {
    /* No block yet. */
    DEFY_UF2_IDLE,
    DEFY_UF2_RECEIVING,
    /* Every block, and ZMK's first page, written and read back. */
    DEFY_UF2_DONE,
    /* A flash operation failed; the file's other blocks are ignored. */
    DEFY_UF2_ABANDONED,
};

enum defy_uf2_result {
    /* Nothing written: a block already stored with the same data, a block of an
     * abandoned file other than its first, or any block after the file completed. */
    DEFY_UF2_IGNORED,
    /* Written and read back, or kept in RAM for ZMK's first page. */
    DEFY_UF2_STORED,
    /* The file's last block: ZMK's first page is written and read back. */
    DEFY_UF2_COMPLETE,
    /* A flash operation failed, and the file is abandoned. */
    DEFY_UF2_FAILED,
};

struct defy_uf2_file {
    enum defy_uf2_file_state state;
    /* The file's numBlocks, which identifies it. */
    uint32_t num_blocks;
    /* How many of its blocks have been stored. */
    uint32_t received;
    /* Bit n: block n stored. */
    uint8_t blocks[(DEFY_UF2_MAX_BLOCKS + 7) / 8];
    /* Bit n: page n of ZMK's range erased since the file started. */
    uint8_t erased[(DEFY_UF2_PAGES + 7) / 8];
    /* ZMK's first page, written last. */
    uint8_t first_page[DEFY_LOADER_PAGE_SIZE];
};

void defy_uf2_file_init(struct defy_uf2_file *file);

/*
 * Adds a block that defy_uf2_parse() passed. The file starts over, with its
 * page and block state cleared and ZMK's first page erased, on:
 * - a block whose numBlocks differs from the file's;
 * - a block already stored whose data differs from what is stored, which
 *   comes from another file of the same size;
 * - block 0 of an abandoned file, which a copy of the file again sends.
 * Before that erase, ZMK's vector table is written with zeros, so that power
 * lost during the erase cannot leave one that start.c accepts. Once the file
 * is DEFY_UF2_DONE, every block is ignored.
 */
enum defy_uf2_result defy_uf2_file_add(struct defy_uf2_file *file,
                                       const struct defy_uf2_block *block,
                                       const struct defy_uf2_flash *flash);
