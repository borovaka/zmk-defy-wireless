/* SPDX-License-Identifier: MIT */

#include <errno.h>
#include <string.h>

#include "uf2.h"

#define MAGIC_START0 0x0a324655u
#define MAGIC_START1 0x9e5d5157u
#define MAGIC_END 0x0ab16f30u

#define FLAG_NOT_MAIN_FLASH 0x00000001u
#define FLAG_FILE_CONTAINER 0x00001000u
#define FLAG_FAMILY_ID 0x00002000u

/* Field offsets in a block. */
#define OFFSET_FLAGS 8
#define OFFSET_ADDRESS 12
#define OFFSET_PAYLOAD_SIZE 16
#define OFFSET_BLOCK_NO 20
#define OFFSET_NUM_BLOCKS 24
#define OFFSET_FAMILY 28
#define OFFSET_DATA 32
#define OFFSET_MAGIC_END (DEFY_UF2_BLOCK_SIZE - 4)

/* ZMK's vector table: its initial stack pointer and reset vector. */
#define VECTORS_SIZE 8u
/* Read-back happens in pieces of this size. */
#define VERIFY_CHUNK 32u

_Static_assert(DEFY_LOADER_ZMK_START % DEFY_LOADER_PAGE_SIZE == 0, "ZMK starts on a page");
_Static_assert(DEFY_LOADER_ZMK_END % DEFY_LOADER_PAGE_SIZE == 0, "ZMK ends on a page");
_Static_assert(DEFY_LOADER_PAGE_SIZE % DEFY_UF2_PAYLOAD_SIZE == 0, "Blocks tile a page");

static uint32_t get_le32(const uint8_t *bytes) {
    return (uint32_t)bytes[0] | (uint32_t)bytes[1] << 8 | (uint32_t)bytes[2] << 16 |
           (uint32_t)bytes[3] << 24;
}

/* Block block_no of num_blocks belongs at address, inside ZMK's range. */
static bool block_fits(uint32_t address, uint32_t block_no, uint32_t num_blocks) {
    return num_blocks != 0 && num_blocks <= DEFY_UF2_MAX_BLOCKS && block_no < num_blocks &&
           address == DEFY_LOADER_ZMK_START + block_no * DEFY_UF2_PAYLOAD_SIZE;
}

enum defy_uf2_check defy_uf2_parse(const uint8_t *sector, struct defy_uf2_block *block) {
    if (get_le32(sector) != MAGIC_START0 || get_le32(sector + 4) != MAGIC_START1) {
        return DEFY_UF2_NOT_UF2;
    }
    uint32_t flags = get_le32(sector + OFFSET_FLAGS);
    uint32_t address = get_le32(sector + OFFSET_ADDRESS);
    uint32_t block_no = get_le32(sector + OFFSET_BLOCK_NO);
    uint32_t num_blocks = get_le32(sector + OFFSET_NUM_BLOCKS);
    if (get_le32(sector + OFFSET_MAGIC_END) != MAGIC_END || !(flags & FLAG_FAMILY_ID) ||
        (flags & (FLAG_NOT_MAIN_FLASH | FLAG_FILE_CONTAINER)) ||
        get_le32(sector + OFFSET_FAMILY) != DEFY_UF2_FAMILY ||
        get_le32(sector + OFFSET_PAYLOAD_SIZE) != DEFY_UF2_PAYLOAD_SIZE ||
        !block_fits(address, block_no, num_blocks)) {
        return DEFY_UF2_REFUSED;
    }
    *block = (struct defy_uf2_block){
        .address = address,
        .block_no = block_no,
        .num_blocks = num_blocks,
        .data = sector + OFFSET_DATA,
    };
    return DEFY_UF2_BLOCK;
}

static bool bit_get(const uint8_t *bits, uint32_t index) {
    return bits[index / 8] & (1u << (index % 8));
}

static void bit_set(uint8_t *bits, uint32_t index) { bits[index / 8] |= 1u << (index % 8); }

static int verify(const struct defy_uf2_flash *flash, uint32_t address, const uint8_t *data,
                  size_t length) {
    uint8_t chunk[VERIFY_CHUNK];
    for (size_t offset = 0; offset < length; offset += sizeof(chunk)) {
        size_t size = length - offset < sizeof(chunk) ? length - offset : sizeof(chunk);
        int err = flash->read(flash->context, address + offset, chunk, size);
        if (err) {
            return err;
        }
        if (memcmp(chunk, data + offset, size) != 0) {
            return -EIO;
        }
    }
    return 0;
}

/*
 * Erases ZMK's first page. Its vector table is first written with zeros, unless it
 * reads as erased or as zeros already. Power lost during the erase can leave the
 * page's words partly erased: an old vector table would still pass start.c's check,
 * while zeros fail it and would need bits to come up into a valid stack pointer and
 * reset vector together. Erased words stay erased. The nRF52833 allows two writes to
 * a word between erases: the vector table's own and this one.
 */
static int erase_first_page(const struct defy_uf2_flash *flash) {
    static const uint8_t zeros[VECTORS_SIZE];
    uint8_t vectors[VECTORS_SIZE];
    int err = flash->read(flash->context, DEFY_LOADER_ZMK_START, vectors, sizeof(vectors));
    if (err) {
        return err;
    }
    bool erased = true;
    for (size_t i = 0; i < sizeof(vectors); i++) {
        erased = erased && vectors[i] == 0xff;
    }
    if (!erased && memcmp(vectors, zeros, sizeof(zeros)) != 0) {
        err = flash->write(flash->context, DEFY_LOADER_ZMK_START, zeros, sizeof(zeros));
    }
    return err ? err : flash->erase(flash->context, DEFY_LOADER_ZMK_START);
}

static enum defy_uf2_result abandon(struct defy_uf2_file *file) {
    file->state = DEFY_UF2_ABANDONED;
    return DEFY_UF2_FAILED;
}

/* Starts the file over, erasing ZMK's first page. False if that failed, which
 * abandons the file. */
static bool start(struct defy_uf2_file *file, uint32_t num_blocks,
                  const struct defy_uf2_flash *flash) {
    defy_uf2_file_init(file);
    file->num_blocks = num_blocks;
    if (erase_first_page(flash)) {
        file->state = DEFY_UF2_ABANDONED;
        return false;
    }
    file->state = DEFY_UF2_RECEIVING;
    bit_set(file->erased, 0);
    return true;
}

/*
 * Writes ZMK's first page from RAM: its body, read back, then its vector table, read
 * back. The vector table is written only over a verified body, and a failure erases
 * the page again.
 */
static int write_first_page(struct defy_uf2_file *file, const struct defy_uf2_flash *flash) {
    const uint32_t body = DEFY_LOADER_ZMK_START + VECTORS_SIZE;
    int err = flash->write(flash->context, body, file->first_page + VECTORS_SIZE,
                           DEFY_LOADER_PAGE_SIZE - VECTORS_SIZE);
    if (!err) {
        err = verify(flash, body, file->first_page + VECTORS_SIZE,
                     DEFY_LOADER_PAGE_SIZE - VECTORS_SIZE);
    }
    if (!err) {
        err = flash->write(flash->context, DEFY_LOADER_ZMK_START, file->first_page,
                           VECTORS_SIZE);
    }
    if (!err) {
        err = verify(flash, DEFY_LOADER_ZMK_START, file->first_page, VECTORS_SIZE);
    }
    if (err) {
        (void)erase_first_page(flash);
    }
    return err;
}

/* True if the block's data equals what the file stored for it. */
static bool stored_same(const struct defy_uf2_file *file, const struct defy_uf2_block *block,
                        const struct defy_uf2_flash *flash) {
    uint32_t offset = block->address - DEFY_LOADER_ZMK_START;
    if (offset < DEFY_LOADER_PAGE_SIZE) {
        return memcmp(file->first_page + offset, block->data, DEFY_UF2_PAYLOAD_SIZE) == 0;
    }
    return verify(flash, block->address, block->data, DEFY_UF2_PAYLOAD_SIZE) == 0;
}

void defy_uf2_file_init(struct defy_uf2_file *file) {
    memset(file, 0, sizeof(*file));
    file->state = DEFY_UF2_IDLE;
    memset(file->first_page, 0xff, sizeof(file->first_page));
}

enum defy_uf2_result defy_uf2_file_add(struct defy_uf2_file *file,
                                       const struct defy_uf2_block *block,
                                       const struct defy_uf2_flash *flash) {
    if (file->state == DEFY_UF2_DONE ||
        !block_fits(block->address, block->block_no, block->num_blocks)) {
        return DEFY_UF2_IGNORED;
    }
    bool restart = block->num_blocks != file->num_blocks;
    if (!restart && file->state == DEFY_UF2_ABANDONED) {
        if (block->block_no != 0) {
            return DEFY_UF2_IGNORED;
        }
        restart = true;
    }
    if (!restart && bit_get(file->blocks, block->block_no)) {
        if (stored_same(file, block, flash)) {
            return DEFY_UF2_IGNORED;
        }
        restart = true;
    }
    if (restart && !start(file, block->num_blocks, flash)) {
        return DEFY_UF2_FAILED;
    }

    uint32_t offset = block->address - DEFY_LOADER_ZMK_START;
    if (offset < DEFY_LOADER_PAGE_SIZE) {
        memcpy(file->first_page + offset, block->data, DEFY_UF2_PAYLOAD_SIZE);
    } else {
        uint32_t page = offset / DEFY_LOADER_PAGE_SIZE;
        if (!bit_get(file->erased, page)) {
            if (flash->erase(flash->context,
                             DEFY_LOADER_ZMK_START + page * DEFY_LOADER_PAGE_SIZE)) {
                return abandon(file);
            }
            bit_set(file->erased, page);
        }
        if (flash->write(flash->context, block->address, block->data, DEFY_UF2_PAYLOAD_SIZE) ||
            verify(flash, block->address, block->data, DEFY_UF2_PAYLOAD_SIZE)) {
            return abandon(file);
        }
    }
    bit_set(file->blocks, block->block_no);
    if (++file->received < file->num_blocks) {
        return DEFY_UF2_STORED;
    }
    if (write_first_page(file, flash)) {
        return abandon(file);
    }
    file->state = DEFY_UF2_DONE;
    return DEFY_UF2_COMPLETE;
}
