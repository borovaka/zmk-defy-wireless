/* SPDX-License-Identifier: MIT */

#include <assert.h>
#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "../../../apps/defy_loader/src/boot_mode.c"
#include "../../../apps/defy_loader/src/uf2.c"

#define FLASH_SIZE 0x80000u
#define WORD 4u
#define MAX_OPS 8192

/* The nRF52833's flash: an erase sets a page to 0xff, a write may only clear bits, and a
 * word takes at most two writes between erases. */
struct op {
    enum { OP_ERASE, OP_WRITE } kind;
    uint32_t address;
    uint32_t length;
    /* A write of zeros only. */
    bool zeros;
};

static uint8_t flash[FLASH_SIZE];
static uint8_t writes[FLASH_SIZE / WORD];
static struct op ops[MAX_OPS];
static size_t op_count;
/* Which call fails: an erase or a write that returns an error, a read whose first byte
 * comes back wrong, or a write that returns 0 but leaves its last byte unprogrammed. */
static int erase_error_at;
static int write_error_at;
static int corrupt_read_at;
static int stuck_write_at;
static int erases;
static int write_calls;
static int reads;

static void check_range(uint32_t address, size_t length) {
    assert(address >= DEFY_LOADER_ZMK_START);
    assert(length <= DEFY_LOADER_ZMK_END - address);
}

static void record(int kind, uint32_t address, uint32_t length, bool zeros) {
    assert(op_count < MAX_OPS);
    ops[op_count++] =
        (struct op){.kind = kind, .address = address, .length = length, .zeros = zeros};
}

static int fake_erase(void *context, uint32_t address) {
    assert(context == &flash);
    check_range(address, DEFY_LOADER_PAGE_SIZE);
    assert(address % DEFY_LOADER_PAGE_SIZE == 0);
    if (++erases == erase_error_at) {
        return -EIO;
    }
    record(OP_ERASE, address, DEFY_LOADER_PAGE_SIZE, false);
    memset(flash + address, 0xff, DEFY_LOADER_PAGE_SIZE);
    memset(writes + address / WORD, 0, DEFY_LOADER_PAGE_SIZE / WORD);
    return 0;
}

static int fake_write(void *context, uint32_t address, const void *data, size_t length) {
    assert(context == &flash);
    check_range(address, length);
    assert(address % WORD == 0 && length % WORD == 0 && length > 0);
    if (++write_calls == write_error_at) {
        return -EIO;
    }
    const uint8_t *bytes = data;
    bool zeros = true;
    for (size_t i = 0; i < length; i++) {
        zeros = zeros && bytes[i] == 0;
    }
    record(OP_WRITE, address, (uint32_t)length, zeros);
    size_t programmed = write_calls == stuck_write_at ? length - 1 : length;
    for (size_t i = 0; i < programmed; i++) {
        flash[address + i] &= bytes[i];
    }
    for (uint32_t word = address / WORD; word < (address + length) / WORD; word++) {
        assert(writes[word] < 2);
        writes[word]++;
    }
    return 0;
}

static int fake_read(void *context, uint32_t address, void *data, size_t length) {
    assert(context == &flash);
    check_range(address, length);
    memcpy(data, flash + address, length);
    if (++reads == corrupt_read_at) {
        ((uint8_t *)data)[0] ^= 1;
    }
    return 0;
}

static const struct defy_uf2_flash fake = {
    .erase = fake_erase,
    .write = fake_write,
    .read = fake_read,
    .context = flash,
};

static struct defy_uf2_file file;

static void reset_flash(void) {
    /* Whatever ZMK held before, written once: not erased, so a missing erase shows. */
    for (size_t i = 0; i < FLASH_SIZE; i++) {
        flash[i] = (uint8_t)(i * 7 + 3);
    }
    memset(writes, 1, sizeof(writes));
    op_count = 0;
    erase_error_at = write_error_at = corrupt_read_at = stuck_write_at = 0;
    erases = write_calls = reads = 0;
    defy_uf2_file_init(&file);
}

static void put32(uint8_t *bytes, uint32_t value) {
    for (int i = 0; i < 4; i++) {
        bytes[i] = (uint8_t)(value >> (8 * i));
    }
}

static uint32_t get32(const uint8_t *bytes) {
    return (uint32_t)bytes[0] | (uint32_t)bytes[1] << 8 | (uint32_t)bytes[2] << 16 |
           (uint32_t)bytes[3] << 24;
}

/* A block as Zephyr's uf2conv.py writes it. */
static void make_block(uint8_t *sector, uint32_t address, uint32_t block_no, uint32_t num_blocks,
                       const uint8_t *data) {
    memset(sector, 0, DEFY_UF2_BLOCK_SIZE);
    put32(sector, 0x0a324655u);
    put32(sector + 4, 0x9e5d5157u);
    put32(sector + 8, 0x00002000u);
    put32(sector + 12, address);
    put32(sector + 16, DEFY_UF2_PAYLOAD_SIZE);
    put32(sector + 20, block_no);
    put32(sector + 24, num_blocks);
    put32(sector + 28, DEFY_UF2_FAMILY);
    memcpy(sector + 32, data, DEFY_UF2_PAYLOAD_SIZE);
    put32(sector + 508, 0x0ab16f30u);
}

/* What the drive does with one sector the host writes. */
static enum defy_uf2_result write_sector(const uint8_t *sector) {
    struct defy_uf2_block block;
    if (defy_uf2_parse(sector, &block) != DEFY_UF2_BLOCK) {
        return DEFY_UF2_IGNORED;
    }
    return defy_uf2_file_add(&file, &block, &fake);
}

/* An image of size bytes from ZMK's start, with a vector table that start.c accepts. */
static uint8_t *make_image(uint32_t size, uint32_t seed) {
    uint8_t *image = malloc(size);
    assert(image != NULL);
    for (uint32_t i = 0; i < size; i++) {
        image[i] = (uint8_t)((i + seed) * 2654435761u >> 24);
    }
    put32(image, 0x20008000u);
    put32(image + 4, DEFY_LOADER_ZMK_START + 0x101u);
    return image;
}

static uint32_t blocks_for(uint32_t size) {
    return (size + DEFY_UF2_PAYLOAD_SIZE - 1) / DEFY_UF2_PAYLOAD_SIZE;
}

static enum defy_uf2_result send_block(const uint8_t *image, uint32_t size, uint32_t block_no) {
    uint8_t data[DEFY_UF2_PAYLOAD_SIZE] = {0};
    uint32_t offset = block_no * DEFY_UF2_PAYLOAD_SIZE;
    uint32_t length = size - offset < DEFY_UF2_PAYLOAD_SIZE ? size - offset : DEFY_UF2_PAYLOAD_SIZE;
    memcpy(data, image + offset, length);
    uint8_t sector[DEFY_UF2_BLOCK_SIZE];
    make_block(sector, DEFY_LOADER_ZMK_START + offset, block_no, blocks_for(size), data);
    return write_sector(sector);
}

static void assert_image(const uint8_t *image, uint32_t size) {
    uint32_t padded = blocks_for(size) * DEFY_UF2_PAYLOAD_SIZE;
    assert(memcmp(flash + DEFY_LOADER_ZMK_START, image, size) == 0);
    for (uint32_t i = size; i < padded; i++) {
        assert(flash[DEFY_LOADER_ZMK_START + i] == 0);
    }
}

static void assert_first_page_erased(void) {
    for (uint32_t b = 0; b < DEFY_LOADER_PAGE_SIZE; b++) {
        assert(flash[DEFY_LOADER_ZMK_START + b] == 0xff);
    }
}

/* start.c's check of ZMK's vector table. */
static bool vectors_valid(void) {
    return defy_loader_zmk_valid(get32(flash + DEFY_LOADER_ZMK_START),
                                 get32(flash + DEFY_LOADER_ZMK_START + 4));
}

static bool is_zero_vectors(const struct op *op) {
    return op->kind == OP_WRITE && op->address == DEFY_LOADER_ZMK_START && op->length == 8 &&
           op->zeros;
}

/* ZMK's first page is erased first and written last, its vector table last of all, and
 * every page is erased once before its first write. Only zeros over the old vector
 * table may come before the first page's erase. */
static void assert_order(void) {
    size_t first = op_count > 0 && is_zero_vectors(&ops[0]) ? 1 : 0;
    assert(op_count >= first + 3);
    assert(ops[first].kind == OP_ERASE && ops[first].address == DEFY_LOADER_ZMK_START);
    struct op *tail = &ops[op_count - 2];
    assert(tail[0].kind == OP_WRITE && tail[0].address == DEFY_LOADER_ZMK_START + 8 &&
           tail[0].length == DEFY_LOADER_PAGE_SIZE - 8);
    assert(tail[1].kind == OP_WRITE && tail[1].address == DEFY_LOADER_ZMK_START &&
           tail[1].length == 8);
    uint8_t erased[DEFY_UF2_PAGES] = {0};
    for (size_t i = first; i < op_count; i++) {
        uint32_t page = (ops[i].address - DEFY_LOADER_ZMK_START) / DEFY_LOADER_PAGE_SIZE;
        if (ops[i].kind == OP_ERASE) {
            assert(!erased[page]);
            erased[page] = 1;
        } else {
            assert(erased[page]);
            if (page == 0) {
                assert(i >= op_count - 2);
            }
        }
    }
}

static void test_parse_accepts_a_zephyr_block(void) {
    uint8_t data[DEFY_UF2_PAYLOAD_SIZE];
    memset(data, 0x5a, sizeof(data));
    uint8_t sector[DEFY_UF2_BLOCK_SIZE];
    make_block(sector, 0xd700, 7, 9, data);
    struct defy_uf2_block block;
    assert(defy_uf2_parse(sector, &block) == DEFY_UF2_BLOCK);
    assert(block.address == 0xd700 && block.block_no == 7 && block.num_blocks == 9);
    assert(block.data == sector + 32);
}

static void test_parse_ignores_sectors_without_magics(void) {
    uint8_t sector[DEFY_UF2_BLOCK_SIZE];
    struct defy_uf2_block block = {.address = 0x1234};
    memset(sector, 0, sizeof(sector));
    assert(defy_uf2_parse(sector, &block) == DEFY_UF2_NOT_UF2);
    memset(sector, 0xff, sizeof(sector));
    assert(defy_uf2_parse(sector, &block) == DEFY_UF2_NOT_UF2);
    /* An AppleDouble header, as macOS writes beside a copied file. */
    memset(sector, 0, sizeof(sector));
    memcpy(sector, "\x00\x05\x16\x07\x00\x02\x00\x00Mac OS X        ", 24);
    assert(defy_uf2_parse(sector, &block) == DEFY_UF2_NOT_UF2);
    /* Each start magic alone. */
    uint8_t data[DEFY_UF2_PAYLOAD_SIZE] = {0};
    make_block(sector, DEFY_LOADER_ZMK_START, 0, 1, data);
    sector[0] ^= 1;
    assert(defy_uf2_parse(sector, &block) == DEFY_UF2_NOT_UF2);
    make_block(sector, DEFY_LOADER_ZMK_START, 0, 1, data);
    sector[7] ^= 0x80;
    assert(defy_uf2_parse(sector, &block) == DEFY_UF2_NOT_UF2);
    assert(block.address == 0x1234);
}

static void assert_refused(uint8_t *sector) {
    struct defy_uf2_block block = {.address = 0x1234};
    assert(defy_uf2_parse(sector, &block) == DEFY_UF2_REFUSED);
    assert(block.address == 0x1234);
}

static void test_parse_refuses_malformed_blocks(void) {
    uint8_t data[DEFY_UF2_PAYLOAD_SIZE] = {0};
    uint8_t sector[DEFY_UF2_BLOCK_SIZE];
    /* No family ID, not main flash, and a file container, alone or with the family ID. */
    static const uint32_t bad_flags[] = {0, 0x00000001u, 0x00002001u, 0x00003000u, 0x00001000u};
    for (size_t i = 0; i < sizeof(bad_flags) / sizeof(bad_flags[0]); i++) {
        make_block(sector, DEFY_LOADER_ZMK_START, 0, 1, data);
        put32(sector + 8, bad_flags[i]);
        assert_refused(sector);
    }
    /* The nRF52840's, the RP2040's and a near miss. */
    static const uint32_t bad_families[] = {0, 0xada52840u, 0xe48bff56u, 0x621e937bu};
    for (size_t i = 0; i < sizeof(bad_families) / sizeof(bad_families[0]); i++) {
        make_block(sector, DEFY_LOADER_ZMK_START, 0, 1, data);
        put32(sector + 28, bad_families[i]);
        assert_refused(sector);
    }
    static const uint32_t bad_sizes[] = {0, 4, 255, 257, 476, 512, 0xffffffffu};
    for (size_t i = 0; i < sizeof(bad_sizes) / sizeof(bad_sizes[0]); i++) {
        make_block(sector, DEFY_LOADER_ZMK_START, 0, 1, data);
        put32(sector + 16, bad_sizes[i]);
        assert_refused(sector);
    }
    make_block(sector, DEFY_LOADER_ZMK_START, 0, 1, data);
    put32(sector + 508, 0);
    assert_refused(sector);
    /* Counts: none, more than ZMK's range holds, and block numbers at or past the count. */
    make_block(sector, DEFY_LOADER_ZMK_START, 0, 0, data);
    assert_refused(sector);
    make_block(sector, DEFY_LOADER_ZMK_START, 0, DEFY_UF2_MAX_BLOCKS + 1, data);
    assert_refused(sector);
    make_block(sector, DEFY_LOADER_ZMK_START, 0, 0xffffffffu, data);
    assert_refused(sector);
    make_block(sector, DEFY_LOADER_ZMK_START + 5 * 256, 5, 5, data);
    assert_refused(sector);
    make_block(sector, DEFY_LOADER_ZMK_START, 0xffffffffu, 1, data);
    assert_refused(sector);
    uint32_t last = DEFY_UF2_MAX_BLOCKS - 1;
    make_block(sector, DEFY_LOADER_ZMK_START + last * 256, last, DEFY_UF2_MAX_BLOCKS, data);
    struct defy_uf2_block block;
    assert(defy_uf2_parse(sector, &block) == DEFY_UF2_BLOCK);
}

/* Every address, with the block number that would put it in place: only 256-aligned
 * ones inside 0xD000..0x6C000 pass. */
static void test_parse_refuses_every_address_outside_zmk(void) {
    uint8_t data[DEFY_UF2_PAYLOAD_SIZE] = {0};
    uint8_t sector[DEFY_UF2_BLOCK_SIZE];
    uint32_t accepted = 0;
    for (uint32_t address = 0; address < 0x81000u; address += 0x40) {
        uint32_t block_no = address >= 0xd000 ? (address - 0xd000) / 256 : 0;
        make_block(sector, address, block_no, DEFY_UF2_MAX_BLOCKS, data);
        struct defy_uf2_block block;
        enum defy_uf2_check check = defy_uf2_parse(sector, &block);
        int inside = address % 256 == 0 && address >= 0xd000 && address <= 0x6bf00;
        assert(check == (inside ? DEFY_UF2_BLOCK : DEFY_UF2_REFUSED));
        accepted += inside;
    }
    assert(accepted == DEFY_UF2_MAX_BLOCKS);
}

/* A block's address must be ZMK's start plus 256 times its number, as in Zephyr's UF2
 * output, so a file built for another place is refused before anything is erased. */
static void test_parse_refuses_blocks_out_of_place(void) {
    uint8_t data[DEFY_UF2_PAYLOAD_SIZE] = {0};
    uint8_t sector[DEFY_UF2_BLOCK_SIZE];
    /* One block off either way, and a block 0 further up. */
    make_block(sector, 0xd100, 0, 2, data);
    assert_refused(sector);
    make_block(sector, 0xd000, 1, 2, data);
    assert_refused(sector);
    make_block(sector, 0xe000, 0, 32, data);
    assert_refused(sector);
    make_block(sector, 0xe000, 15, 32, data);
    assert_refused(sector);
}

static void test_whole_image_in_order(uint32_t size) {
    reset_flash();
    uint8_t *image = make_image(size, size);
    uint32_t count = blocks_for(size);
    for (uint32_t i = 0; i < count; i++) {
        enum defy_uf2_result result = send_block(image, size, i);
        assert(result == (i + 1 == count ? DEFY_UF2_COMPLETE : DEFY_UF2_STORED));
        if (i + 1 < count) {
            /* Until the last block, ZMK's vector table stays erased. */
            for (uint32_t b = 0; b < 8; b++) {
                assert(flash[DEFY_LOADER_ZMK_START + b] == 0xff);
            }
        }
    }
    assert(file.state == DEFY_UF2_DONE);
    assert_image(image, size);
    assert_order();
    /* The old vector table was zeroed before its page's erase. */
    assert(is_zero_vectors(&ops[0]));
    /* Nothing outside the image's pages changed. */
    uint32_t end = DEFY_LOADER_ZMK_START +
                   (count * DEFY_UF2_PAYLOAD_SIZE + DEFY_LOADER_PAGE_SIZE - 1) /
                       DEFY_LOADER_PAGE_SIZE * DEFY_LOADER_PAGE_SIZE;
    for (uint32_t i = 0; i < DEFY_LOADER_ZMK_START; i++) {
        assert(flash[i] == (uint8_t)(i * 7 + 3));
    }
    for (uint32_t i = end; i < FLASH_SIZE; i++) {
        assert(flash[i] == (uint8_t)(i * 7 + 3));
    }
    free(image);
}

static void test_shuffled_and_reversed_orders(void) {
    const uint32_t size = 281544;
    uint8_t *image = make_image(size, 1);
    uint32_t count = blocks_for(size);
    uint32_t *order = malloc(count * sizeof(*order));
    assert(order != NULL);
    for (int pass = 0; pass < 2; pass++) {
        reset_flash();
        for (uint32_t i = 0; i < count; i++) {
            order[i] = pass == 0 ? count - 1 - i : i;
        }
        if (pass == 1) {
            uint32_t state = 12345;
            for (uint32_t i = count - 1; i > 0; i--) {
                state = state * 1103515245u + 12345u;
                uint32_t j = state % (i + 1);
                uint32_t swap = order[i];
                order[i] = order[j];
                order[j] = swap;
            }
        }
        for (uint32_t i = 0; i < count; i++) {
            enum defy_uf2_result result = send_block(image, size, order[i]);
            assert(result == (i + 1 == count ? DEFY_UF2_COMPLETE : DEFY_UF2_STORED));
        }
        assert_image(image, size);
        assert_order();
    }
    free(order);
    free(image);
}

static void test_repeated_blocks_are_ignored(void) {
    reset_flash();
    const uint32_t size = 0x3000;
    uint8_t *image = make_image(size, 2);
    uint32_t count = blocks_for(size);
    assert(send_block(image, size, 20) == DEFY_UF2_STORED);
    size_t before = op_count;
    int reads_before = reads;
    /* The same block again: compared with the flash, nothing written, nothing counted. */
    assert(send_block(image, size, 20) == DEFY_UF2_IGNORED);
    assert(op_count == before && reads > reads_before);
    /* Likewise inside ZMK's first page, compared with the copy in RAM. */
    assert(send_block(image, size, 3) == DEFY_UF2_STORED);
    before = op_count;
    assert(send_block(image, size, 3) == DEFY_UF2_IGNORED);
    assert(op_count == before);
    /* Another block number for an address already written never reaches the writer. */
    uint8_t data[DEFY_UF2_PAYLOAD_SIZE] = {0};
    uint8_t sector[DEFY_UF2_BLOCK_SIZE];
    make_block(sector, DEFY_LOADER_ZMK_START + 20 * 256, 21, count, data);
    assert(write_sector(sector) == DEFY_UF2_IGNORED);
    assert(op_count == before);
    for (uint32_t i = 0; i < count; i++) {
        enum defy_uf2_result result = send_block(image, size, i);
        if (i == 3 || i == 20) {
            assert(result == DEFY_UF2_IGNORED);
        } else {
            assert(result == (i + 1 == count ? DEFY_UF2_COMPLETE : DEFY_UF2_STORED));
        }
    }
    assert_image(image, size);
    assert_order();
    free(image);
}

/* Half of file A, then file B of the same block count without a restart of the loader:
 * B's block 0 differs from A's, so B starts the file over and completes alone, where
 * the two would otherwise merge into one image under a valid vector table. */
static void test_another_file_of_the_same_size_starts_over(void) {
    reset_flash();
    const uint32_t size = 0x8000;
    uint8_t *a = make_image(size, 20);
    uint8_t *b = make_image(size, 21);
    uint32_t count = blocks_for(size);
    for (uint32_t i = 0; i < count / 2; i++) {
        assert(send_block(a, size, i) == DEFY_UF2_STORED);
    }
    for (uint32_t i = 0; i < count; i++) {
        enum defy_uf2_result result = send_block(b, size, i);
        assert(result == (i + 1 == count ? DEFY_UF2_COMPLETE : DEFY_UF2_STORED));
    }
    assert_image(b, size);

    /* B shuffled after half of A. Blocks of B that joined A's file before one of A's
     * blocks came again with B's data are dropped when the file starts over, so a copy
     * may not complete; it then leaves ZMK's vector table erased, and the next copy
     * completes B. Either way, no image mixes the two files. */
    uint32_t *order = malloc(count * sizeof(*order));
    assert(order != NULL);
    for (uint32_t seed = 1; seed <= 20; seed++) {
        reset_flash();
        for (uint32_t i = 0; i < count / 2; i++) {
            assert(send_block(a, size, i) == DEFY_UF2_STORED);
        }
        for (uint32_t i = 0; i < count; i++) {
            order[i] = i;
        }
        uint32_t state = seed;
        for (uint32_t i = count - 1; i > 0; i--) {
            state = state * 1103515245u + 12345u;
            uint32_t j = (state >> 8) % (i + 1);
            uint32_t swap = order[i];
            order[i] = order[j];
            order[j] = swap;
        }
        for (int copy = 0; copy < 2 && file.state != DEFY_UF2_DONE; copy++) {
            for (uint32_t i = 0; i < count; i++) {
                assert(send_block(b, size, order[i]) != DEFY_UF2_FAILED);
            }
            if (file.state != DEFY_UF2_DONE) {
                assert(copy == 0 && file.state == DEFY_UF2_RECEIVING);
                assert_first_page_erased();
            }
        }
        assert(file.state == DEFY_UF2_DONE);
        assert_image(b, size);
    }
    free(order);
    free(a);
    free(b);
}

/* B equals A up to block 40. B's first 40 blocks match A's and are ignored, and block
 * 40 starts the file over, so this copy of B cannot complete: ZMK's vector table stays
 * erased. A second copy of B completes it. */
static void test_shared_prefix_completes_on_a_second_copy(void) {
    reset_flash();
    const uint32_t size = 0x8000;
    const uint32_t shared = 40;
    uint8_t *a = make_image(size, 22);
    uint8_t *b = make_image(size, 22);
    uint32_t count = blocks_for(size);
    for (uint32_t i = shared * DEFY_UF2_PAYLOAD_SIZE; i < size; i++) {
        b[i] ^= 0x5a;
    }
    for (uint32_t i = 0; i < count / 2; i++) {
        assert(send_block(a, size, i) == DEFY_UF2_STORED);
    }
    for (uint32_t i = 0; i < count; i++) {
        enum defy_uf2_result result = send_block(b, size, i);
        assert(result == (i < shared ? DEFY_UF2_IGNORED : DEFY_UF2_STORED));
    }
    assert(file.state == DEFY_UF2_RECEIVING);
    assert_first_page_erased();
    for (uint32_t i = 0; i < count; i++) {
        enum defy_uf2_result result = send_block(b, size, i);
        assert(result == (i + 1 < shared    ? DEFY_UF2_STORED
                          : i + 1 == shared ? DEFY_UF2_COMPLETE
                                            : DEFY_UF2_IGNORED));
    }
    assert_image(b, size);
    free(a);
    free(b);
}

static void test_other_num_blocks_starts_over(void) {
    reset_flash();
    const uint32_t size_a = 0x9000, size_b = 0x6100;
    uint8_t *a = make_image(size_a, 4);
    uint8_t *b = make_image(size_b, 5);
    /* Half of A, including its first page and the pages B will reuse. */
    for (uint32_t i = 0; i < blocks_for(size_a) / 2; i++) {
        assert(send_block(a, size_a, i) == DEFY_UF2_STORED);
    }
    size_t a_ops = op_count;
    int a_erases = erases;
    /* B, out of order: its first block starts over and erases ZMK's first page again. */
    uint32_t count = blocks_for(size_b);
    assert(send_block(b, size_b, count - 1) == DEFY_UF2_STORED);
    assert(ops[a_ops].kind == OP_ERASE && ops[a_ops].address == DEFY_LOADER_ZMK_START);
    for (uint32_t i = 0; i + 1 < count; i++) {
        enum defy_uf2_result result = send_block(b, size_b, i);
        assert(result == (i + 2 == count ? DEFY_UF2_COMPLETE : DEFY_UF2_STORED));
    }
    assert_image(b, size_b);
    /* Every page B wrote was erased again after A's writes. */
    assert(erases - a_erases == (int)((size_b + DEFY_LOADER_PAGE_SIZE - 1) / DEFY_LOADER_PAGE_SIZE));
    /* A's blocks after the restart start A over again, which B's completion forbids. */
    assert(send_block(a, size_a, 0) == DEFY_UF2_IGNORED);
    free(a);
    free(b);
}

static void test_restart_drops_the_first_page_kept_in_ram(void) {
    reset_flash();
    const uint32_t size_a = 0x2000, size_b = 0x200;
    uint8_t *a = make_image(size_a, 6);
    uint8_t *b = make_image(size_b, 7);
    for (uint32_t i = 0; i < 16; i++) {
        assert(send_block(a, size_a, i) == DEFY_UF2_STORED);
    }
    assert(send_block(b, size_b, 0) == DEFY_UF2_STORED);
    assert(send_block(b, size_b, 1) == DEFY_UF2_COMPLETE);
    assert_image(b, size_b);
    /* The rest of the first page is erased, not A's bytes. */
    for (uint32_t i = size_b; i < DEFY_LOADER_PAGE_SIZE; i++) {
        assert(flash[DEFY_LOADER_ZMK_START + i] == 0xff);
    }
    free(a);
    free(b);
}

static void test_metadata_sectors_are_ignored(void) {
    reset_flash();
    const uint32_t size = 0x1800;
    uint8_t *image = make_image(size, 8);
    uint32_t count = blocks_for(size);
    uint8_t junk[DEFY_UF2_BLOCK_SIZE];
    for (uint32_t i = 0; i < count; i++) {
        /* Between the blocks, sectors a host writes for its own files. */
        memset(junk, (int)i, sizeof(junk));
        assert(write_sector(junk) == DEFY_UF2_IGNORED);
        memset(junk, 0, sizeof(junk));
        memcpy(junk, ".fseventsd ", 11);
        assert(write_sector(junk) == DEFY_UF2_IGNORED);
        enum defy_uf2_result result = send_block(image, size, i);
        assert(result == (i + 1 == count ? DEFY_UF2_COMPLETE : DEFY_UF2_STORED));
    }
    assert_image(image, size);
    assert_order();
    /* Before any block, junk touches nothing. */
    reset_flash();
    for (int i = 0; i < 64; i++) {
        memset(junk, i * 37, sizeof(junk));
        assert(write_sector(junk) == DEFY_UF2_IGNORED);
    }
    assert(op_count == 0 && reads == 0 && file.state == DEFY_UF2_IDLE);
    free(image);
}

static void test_completed_file_ignores_everything(void) {
    reset_flash();
    const uint32_t size = 0x400;
    uint8_t *image = make_image(size, 9);
    uint8_t *other = make_image(0x800, 10);
    for (uint32_t i = 0; i < 4; i++) {
        send_block(image, size, i);
    }
    assert(file.state == DEFY_UF2_DONE);
    size_t done = op_count;
    assert(send_block(image, size, 0) == DEFY_UF2_IGNORED);
    for (uint32_t i = 0; i < 8; i++) {
        assert(send_block(other, 0x800, i) == DEFY_UF2_IGNORED);
    }
    assert(op_count == done);
    assert_image(image, size);
    free(image);
    free(other);
}

static void test_interrupted_copy_leaves_vectors_erased(void) {
    reset_flash();
    const uint32_t size = 0x20000;
    uint8_t *image = make_image(size, 11);
    uint32_t count = blocks_for(size);
    for (uint32_t i = 0; i + 1 < count; i++) {
        assert(send_block(image, size, i) == DEFY_UF2_STORED);
    }
    assert_first_page_erased();
    /* The copy again after a restart of the loader completes from scratch. ZMK's vector
     * table is erased, so nothing is written before its page's erase. */
    defy_uf2_file_init(&file);
    op_count = 0;
    for (uint32_t i = 0; i < count; i++) {
        enum defy_uf2_result result = send_block(image, size, i);
        assert(result == (i + 1 == count ? DEFY_UF2_COMPLETE : DEFY_UF2_STORED));
    }
    assert_image(image, size);
    assert_order();
    assert(ops[0].kind == OP_ERASE);
    free(image);
}

static void test_single_block_file(void) {
    /* One block, ZMK's vector table: written at once, vectors last. */
    reset_flash();
    uint8_t *image = make_image(256, 12);
    assert(send_block(image, 256, 0) == DEFY_UF2_COMPLETE);
    assert_image(image, 256);
    assert_order();
    assert(erases == 1);
    for (uint32_t i = 256; i < DEFY_LOADER_PAGE_SIZE; i++) {
        assert(flash[DEFY_LOADER_ZMK_START + i] == 0xff);
    }
    free(image);
}

static void test_full_range_file(void) {
    reset_flash();
    const uint32_t size = DEFY_LOADER_ZMK_END - DEFY_LOADER_ZMK_START;
    uint8_t *image = make_image(size, 13);
    for (uint32_t i = 0; i < DEFY_UF2_MAX_BLOCKS; i++) {
        enum defy_uf2_result result = send_block(image, size, i);
        assert(result == (i + 1 == DEFY_UF2_MAX_BLOCKS ? DEFY_UF2_COMPLETE : DEFY_UF2_STORED));
    }
    assert_image(image, size);
    assert_order();
    assert(erases == (int)DEFY_UF2_PAGES);
    for (uint32_t i = DEFY_LOADER_ZMK_END; i < FLASH_SIZE; i++) {
        assert(flash[i] == (uint8_t)(i * 7 + 3));
    }
    free(image);
}

/* Before ZMK's first page is erased, its old vector table is written with zeros, so an
 * erase cut short leaves none that start.c accepts. */
static void test_vectors_zeroed_before_the_first_erase(void) {
    reset_flash();
    uint8_t *old = make_image(DEFY_LOADER_PAGE_SIZE, 30);
    memcpy(flash + DEFY_LOADER_ZMK_START, old, DEFY_LOADER_PAGE_SIZE);
    assert(vectors_valid());
    uint8_t *image = make_image(0x2000, 31);
    /* The erase fails, standing in for power lost during it. */
    erase_error_at = 1;
    assert(send_block(image, 0x2000, 0) == DEFY_UF2_FAILED);
    assert(op_count == 1 && is_zero_vectors(&ops[0]));
    for (uint32_t b = 0; b < 8; b++) {
        assert(flash[DEFY_LOADER_ZMK_START + b] == 0);
    }
    assert(!vectors_valid());
    /* The file again, from block 0: the vector table reads as zeros, so it is not written
     * a third time, and the erase goes ahead. */
    op_count = 0;
    for (uint32_t i = 0; i < 32; i++) {
        enum defy_uf2_result result = send_block(image, 0x2000, i);
        assert(result == (i == 31 ? DEFY_UF2_COMPLETE : DEFY_UF2_STORED));
    }
    assert(ops[0].kind == OP_ERASE);
    assert_order();
    assert_image(image, 0x2000);
    free(old);
    free(image);
}

static void test_flash_failures_abandon_the_file(void) {
    const uint32_t size = 0x3000;
    uint8_t *image = make_image(size, 14);
    uint8_t *other = make_image(0x2f00, 15);
    uint32_t count = blocks_for(size);

    /* The first page's erase fails: the file's other blocks are ignored, another file starts. */
    reset_flash();
    erase_error_at = 1;
    assert(send_block(image, size, 0) == DEFY_UF2_FAILED);
    assert(file.state == DEFY_UF2_ABANDONED);
    for (uint32_t i = 1; i < count; i++) {
        assert(send_block(image, size, i) == DEFY_UF2_IGNORED);
    }
    assert(op_count == 1 && is_zero_vectors(&ops[0]));
    for (uint32_t i = 0; i < blocks_for(0x2f00); i++) {
        enum defy_uf2_result result = send_block(other, 0x2f00, i);
        assert(result == (i + 1 == blocks_for(0x2f00) ? DEFY_UF2_COMPLETE : DEFY_UF2_STORED));
    }
    assert_image(other, 0x2f00);

    /* A page erase, a block write or a read-back fails part way: the file's later blocks
     * are ignored, and ZMK's first page stays erased. */
    for (int kind = 0; kind < 3; kind++) {
        reset_flash();
        if (kind == 0) {
            erase_error_at = 2;
        } else if (kind == 1) {
            write_error_at = 6;
        } else {
            corrupt_read_at = 40;
        }
        enum defy_uf2_result result = DEFY_UF2_STORED;
        uint32_t i = 0;
        for (; i < count && result == DEFY_UF2_STORED; i++) {
            result = send_block(image, size, i);
        }
        assert(result == DEFY_UF2_FAILED && file.state == DEFY_UF2_ABANDONED);
        for (; i < count; i++) {
            assert(send_block(image, size, i) == DEFY_UF2_IGNORED);
        }
        assert_first_page_erased();
    }

    /*
     * ZMK's first page, written last: its body's write, the body's read-back, the vector
     * table's write or its read-back fails, by an error or by a byte left unprogrammed.
     * The vector table is written only over a verified body, and the page ends erased.
     * The writes: zeros over the old vector table, one per block outside the first page,
     * the body, the vector table. The reads: the old vector table, 8 per block, then the
     * body's read-back in 32-byte pieces and the vector table's.
     */
    uint32_t outside = count - DEFY_LOADER_PAGE_SIZE / DEFY_UF2_PAYLOAD_SIZE;
    int body_write = (int)outside + 2;
    int vectors_write = body_write + 1;
    int body_read = (int)outside * 8 + 2;
    int vectors_read = body_read + (DEFY_LOADER_PAGE_SIZE - 8 + VERIFY_CHUNK - 1) / VERIFY_CHUNK;
    for (int kind = 0; kind < 6; kind++) {
        reset_flash();
        switch (kind) {
        case 0:
            write_error_at = body_write;
            break;
        case 1:
            stuck_write_at = body_write;
            break;
        case 2:
            corrupt_read_at = body_read;
            break;
        case 3:
            write_error_at = vectors_write;
            break;
        case 4:
            stuck_write_at = vectors_write;
            break;
        default:
            corrupt_read_at = vectors_read;
            break;
        }
        for (uint32_t i = 0; i + 1 < count; i++) {
            assert(send_block(image, size, i) == DEFY_UF2_STORED);
        }
        assert(send_block(image, size, count - 1) == DEFY_UF2_FAILED);
        assert(file.state == DEFY_UF2_ABANDONED);
        assert_first_page_erased();
        /* A body that failed its read-back never got a vector table. */
        bool vectors_written = false;
        for (size_t i = 1; i < op_count; i++) {
            vectors_written = vectors_written || (ops[i].kind == OP_WRITE &&
                                                  ops[i].address == DEFY_LOADER_ZMK_START &&
                                                  ops[i].length == 8 && !ops[i].zeros);
        }
        assert(vectors_written == (kind >= 4));
    }
    free(image);
    free(other);
}

/* After a flash error abandons a file, the same file copied again starts over at its
 * block 0 and completes, without a restart of the loader. Its other blocks before
 * block 0 stay ignored. */
static void test_abandoned_file_starts_over_at_block_zero(void) {
    reset_flash();
    const uint32_t size = 0x3000;
    uint8_t *image = make_image(size, 16);
    uint32_t count = blocks_for(size);
    write_error_at = 10;
    enum defy_uf2_result result = DEFY_UF2_STORED;
    for (uint32_t i = 0; i < count && result == DEFY_UF2_STORED; i++) {
        result = send_block(image, size, i);
    }
    assert(result == DEFY_UF2_FAILED && file.state == DEFY_UF2_ABANDONED);
    assert(send_block(image, size, 5) == DEFY_UF2_IGNORED);
    assert(send_block(image, size, 30) == DEFY_UF2_IGNORED);
    for (uint32_t i = 0; i < count; i++) {
        result = send_block(image, size, i);
        assert(result == (i + 1 == count ? DEFY_UF2_COMPLETE : DEFY_UF2_STORED));
    }
    assert_image(image, size);
    free(image);
}

int main(void) {
    test_parse_accepts_a_zephyr_block();
    test_parse_ignores_sectors_without_magics();
    test_parse_refuses_malformed_blocks();
    test_parse_refuses_every_address_outside_zmk();
    test_parse_refuses_blocks_out_of_place();
    test_whole_image_in_order(256 * 17);
    test_whole_image_in_order(281544);
    test_shuffled_and_reversed_orders();
    test_repeated_blocks_are_ignored();
    test_another_file_of_the_same_size_starts_over();
    test_shared_prefix_completes_on_a_second_copy();
    test_other_num_blocks_starts_over();
    test_restart_drops_the_first_page_kept_in_ram();
    test_metadata_sectors_are_ignored();
    test_completed_file_ignores_everything();
    test_interrupted_copy_leaves_vectors_erased();
    test_single_block_file();
    test_full_range_file();
    test_vectors_zeroed_before_the_first_erase();
    test_flash_failures_abandon_the_file();
    test_abandoned_file_starts_over_at_block_zero();
    return 0;
}
