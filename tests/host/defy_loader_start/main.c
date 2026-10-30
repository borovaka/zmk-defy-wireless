/* SPDX-License-Identifier: MIT */

#include <assert.h>
#include <stddef.h>
#include <stdint.h>

#include "../../../apps/defy_loader/src/boot_mode.c"

static const uint32_t stack = 0x20008000u, reset = 0x2b8f5u;

static void test_vector_table(void) {
    /* What ZMK's build puts at 0xD000: its main stack's top and z_arm_reset. */
    assert(defy_loader_zmk_valid(0x20008d58u, 0x0002b8f5u));
    /* The stack pointer: inside RAM, its top included, and word-aligned. */
    assert(!defy_loader_zmk_valid(0x20000000u, 0xd101u));
    assert(defy_loader_zmk_valid(0x20000004u, 0xd101u));
    assert(defy_loader_zmk_valid(0x20020000u, 0xd101u));
    assert(!defy_loader_zmk_valid(0x20020004u, 0xd101u));
    assert(!defy_loader_zmk_valid(0x20008002u, 0xd101u));
    assert(!defy_loader_zmk_valid(0x20008001u, 0xd101u));
    assert(!defy_loader_zmk_valid(0x1fffff00u, 0xd101u));
    assert(!defy_loader_zmk_valid(0x00000000u, 0xd101u));
    /* The reset vector: Thumb, inside 0xD000..0x6C000. */
    assert(defy_loader_zmk_valid(0x20008000u, 0xd001u));
    assert(defy_loader_zmk_valid(0x20008000u, 0x6bfffu));
    assert(!defy_loader_zmk_valid(0x20008000u, 0xd000u));
    assert(!defy_loader_zmk_valid(0x20008000u, 0x2b8f4u));
    assert(!defy_loader_zmk_valid(0x20008000u, 0xcfffu));
    assert(!defy_loader_zmk_valid(0x20008000u, 0x6c001u));
    /* Erased flash, an interrupted copy, and the zeros written before ZMK's first page
     * is erased. */
    assert(!defy_loader_zmk_valid(0xffffffffu, 0xffffffffu));
    assert(!defy_loader_zmk_valid(0x20008000u, 0xffffffffu));
    assert(!defy_loader_zmk_valid(0xffffffffu, 0xd101u));
    assert(!defy_loader_zmk_valid(0, 0));
}

static void test_starts(void) {
    assert(defy_loader_starts(0) == 0);
    assert(defy_loader_starts(DEFY_LOADER_DRIVE_MAGIC) == 0);
    assert(defy_loader_starts(DEFY_LOADER_STARTS_BASE) == 0);
    for (uint32_t n = 1; n <= DEFY_LOADER_START_LIMIT; n++) {
        assert(defy_loader_starts(DEFY_LOADER_STARTS(n)) == n);
    }
    assert(defy_loader_starts(DEFY_LOADER_STARTS(DEFY_LOADER_START_LIMIT) + 1) == 0);
    assert(defy_loader_starts(0xffu) == 0);
    assert(defy_loader_starts(0x1c1u) == 0);
    /* The values that count, and no others. */
    uint32_t counted = 0;
    for (uint32_t value = 0; value < 0x10000; value++) {
        counted += defy_loader_starts(value) != 0;
    }
    assert(counted == DEFY_LOADER_START_LIMIT);
}

static void test_start_mode(void) {
    /* Every 8-bit value: only 0x57 requests the drive, even with a valid ZMK, and only the
     * limit stops a valid ZMK. Without a valid ZMK the count does not matter. */
    uint32_t limit = DEFY_LOADER_STARTS(DEFY_LOADER_START_LIMIT);
    for (uint32_t value = 0; value < 0x100; value++) {
        enum defy_loader_start mode = defy_loader_start_mode(value, stack, reset);
        assert(mode == (value == 0x57    ? DEFY_LOADER_DRIVE_REQUESTED
                        : value == limit ? DEFY_LOADER_DRIVE_ZMK_FAILED
                                         : DEFY_LOADER_START_ZMK));
        mode = defy_loader_start_mode(value, stack, 0xffffffffu);
        assert(mode == (value == 0x57 ? DEFY_LOADER_DRIVE_REQUESTED : DEFY_LOADER_DRIVE_NO_ZMK));
    }
}

/* GPREGRET through a run of starts, as start.c writes it, with ZMK's confirmation
 * (bootloader/defy_loader_confirm.c) in between where noted. */
static enum defy_loader_start start(uint32_t *gpregret, uint32_t stack_word,
                                    uint32_t reset_word) {
    enum defy_loader_start mode = defy_loader_start_mode(*gpregret, stack_word, reset_word);
    *gpregret = defy_loader_start_gpregret(mode, *gpregret);
    return mode;
}

static void confirm(uint32_t *gpregret) {
    if (defy_loader_starts(*gpregret) != 0) {
        *gpregret = 0;
    }
}

static void test_start_sequences(void) {
    /* A ZMK that fails before it confirms: three starts, then the drive, for as long as
     * resets keep GPREGRET. */
    uint32_t gpregret = 0;
    for (uint32_t n = 1; n <= DEFY_LOADER_START_LIMIT; n++) {
        assert(start(&gpregret, stack, reset) == DEFY_LOADER_START_ZMK);
        assert(gpregret == DEFY_LOADER_STARTS(n));
    }
    for (int i = 0; i < 3; i++) {
        assert(start(&gpregret, stack, reset) == DEFY_LOADER_DRIVE_ZMK_FAILED);
    }
    /* The loader clears GPREGRET before its resets into ZMK: three more starts. */
    gpregret = 0;
    assert(start(&gpregret, stack, reset) == DEFY_LOADER_START_ZMK);

    /* A ZMK that confirms is never stopped, however often it restarts afterwards. */
    gpregret = 0;
    for (int i = 0; i < 10; i++) {
        assert(start(&gpregret, stack, reset) == DEFY_LOADER_START_ZMK);
        confirm(&gpregret);
        assert(gpregret == 0);
    }

    /* One failure, then a confirmed run, then failures: the count starts again. */
    gpregret = 0;
    start(&gpregret, stack, reset);
    start(&gpregret, stack, reset);
    confirm(&gpregret);
    for (uint32_t n = 1; n <= DEFY_LOADER_START_LIMIT; n++) {
        assert(start(&gpregret, stack, reset) == DEFY_LOADER_START_ZMK);
    }
    assert(start(&gpregret, stack, reset) == DEFY_LOADER_DRIVE_ZMK_FAILED);

    /* &defy_drive's request replaces a count, and the confirmation leaves it alone. */
    gpregret = DEFY_LOADER_DRIVE_MAGIC;
    confirm(&gpregret);
    assert(gpregret == DEFY_LOADER_DRIVE_MAGIC);
    assert(start(&gpregret, stack, reset) == DEFY_LOADER_DRIVE_REQUESTED);
    assert(gpregret == 0);
    /* After the drive, ZMK starts with a fresh count. */
    assert(start(&gpregret, stack, reset) == DEFY_LOADER_START_ZMK);
    assert(gpregret == DEFY_LOADER_STARTS(1));

    /* No valid ZMK: the drive, with GPREGRET cleared, and the copied ZMK starts afresh. */
    gpregret = DEFY_LOADER_STARTS(DEFY_LOADER_START_LIMIT);
    assert(start(&gpregret, 0xffffffffu, 0xffffffffu) == DEFY_LOADER_DRIVE_NO_ZMK);
    assert(gpregret == 0);
    assert(start(&gpregret, stack, reset) == DEFY_LOADER_START_ZMK);
    assert(gpregret == DEFY_LOADER_STARTS(1));
}

int main(void) {
    test_vector_table();
    test_starts();
    test_start_mode();
    test_start_sequences();
    return 0;
}
