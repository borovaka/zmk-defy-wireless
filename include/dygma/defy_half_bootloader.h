/* SPDX-License-Identifier: MIT */

#ifndef DYGMA_DEFY_HALF_BOOTLOADER_H
#define DYGMA_DEFY_HALF_BOOTLOADER_H

#include <stdbool.h>
#include <stdint.h>

#include <zephyr/device.h>
#include <zephyr/drivers/gpio.h>

#include <dygma/defy_half_package.h>
#include <dygma/defy_wired_protocol.h>

/*
 * The halves' factory I2C bootloaders. reset_all() resets both halves into
 * them and begin() opens one and checks its INFO. The reads, erases, writes
 * and validation need their half begun and fail with -EACCES otherwise; a
 * failed transfer leaves the half to begin again. abort() and recover()
 * release the halves through their RUN lines.
 */

/* Both halves, by enum defy_wired_side. */
#define DEFY_HALF_BOOTLOADER_SIDES (DEFY_WIRED_RIGHT + 1)

struct defy_half_bootloader_info {
    uint32_t device_id;
    uint32_t bootloader_version;
    uint32_t hardware_version;
    uint32_t program_version;
    uint32_t flash_start;
    uint32_t validation_start;
    uint32_t program_start;
    uint32_t flash_available;
    uint32_t erase_alignment;
    uint32_t write_alignment;
    uint32_t max_transmission;
};

struct defy_half_bootloader_config {
    const struct device *bus;
    struct gpio_dt_spec reset[DEFY_HALF_BOOTLOADER_SIDES];
};

struct defy_half_bootloader {
    struct defy_half_bootloader_config config;
    struct {
        uint32_t erased_sector;
        uint32_t next_page;
        bool begun;
        bool ready;
        /* The flash the loader reported reaches past the settings' NVS sectors. */
        bool settings_reachable;
    } sides[DEFY_HALF_BOOTLOADER_SIDES];
    /* BIT(side): halves still to reset, and those whose RUN line is held low. */
    uint8_t reset_pending;
    uint8_t reset_asserted;
    /* A reset has put the halves in their loaders and no release has followed. */
    bool acquired;
};

int defy_half_bootloader_init(struct defy_half_bootloader *loader,
                              const struct defy_half_bootloader_config *config);
int defy_half_bootloader_reset_all(struct defy_half_bootloader *loader);
int defy_half_bootloader_recover(struct defy_half_bootloader *loader);
int defy_half_bootloader_begin(struct defy_half_bootloader *loader, enum defy_wired_side side,
                               struct defy_half_bootloader_info *info);
int defy_half_bootloader_read_seal(struct defy_half_bootloader *loader,
                                   enum defy_wired_side side, struct defy_half_seal *seal);
int defy_half_bootloader_erase_sector(struct defy_half_bootloader *loader,
                                      enum defy_wired_side side, uint32_t address);
/* Erases the settings' NVS sectors, DEFY_HALF_SETTINGS_START..DEFY_HALF_SETTINGS_END, one
 * sector at a time, and leaves no erased sector for a page write. -ERANGE before any transfer
 * when the flash the loader reported does not reach that far. */
int defy_half_bootloader_erase_settings(struct defy_half_bootloader *loader,
                                        enum defy_wired_side side);
/* Writes a page into the erased sector, after the last page written, and checks the CRC32 the
 * loader returns for it. */
int defy_half_bootloader_write_page(struct defy_half_bootloader *loader, enum defy_wired_side side,
                                    uint32_t address, const uint8_t data[DEFY_HALF_PAGE_SIZE]);
int defy_half_bootloader_validate(struct defy_half_bootloader *loader, enum defy_wired_side side,
                                  struct defy_half_seal *seal);
int defy_half_bootloader_abort(struct defy_half_bootloader *loader);

#endif
