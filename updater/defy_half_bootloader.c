/* SPDX-License-Identifier: MIT */

#include <errno.h>
#include <string.h>

#include <zephyr/drivers/i2c.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/byteorder.h>
#include <zephyr/sys/crc.h>

#include <dygma/defy_half_bootloader.h>

#if defined(CONFIG_I2C_NRFX_TWIM)
BUILD_ASSERT(CONFIG_I2C_NRFX_TRANSFER_TIMEOUT > 0 &&
                 CONFIG_I2C_NRFX_TRANSFER_TIMEOUT <= 600,
             "a transfer to a half's bootloader may take at most 600 ms, the "
             "defy-half-updater snippet's limit");
#endif

/* The halves' bootloaders on the cable I2C bus, and their commands. */
#define LEFT_ADDRESS 0x5a
#define RIGHT_ADDRESS 0x5b
enum {
    CMD_BEGIN = 'B',
    CMD_INFO = 'I',
    CMD_READ = 'R',
    CMD_ERASE = 'E',
    CMD_WRITE = 'W',
    CMD_VALIDATE = 'V',
};
/* INFO's reply, eleven words, and the hardware version it must report. */
#define INFO_SIZE 44
#define HARDWARE_VERSION 1
/* A reset holds the RUN lines low this long, then gives the loaders this long to start. */
#define RESET_PULSE_MS 10
#define RESET_SETTLE_MS 50

/* Whole sectors above the application area, where no page can be written. */
BUILD_ASSERT(DEFY_HALF_SETTINGS_START >= DEFY_HALF_FLASH_END &&
             DEFY_HALF_SETTINGS_END > DEFY_HALF_SETTINGS_START &&
             DEFY_HALF_SETTINGS_START % DEFY_HALF_SECTOR_SIZE == 0 &&
             DEFY_HALF_SETTINGS_END % DEFY_HALF_SECTOR_SIZE == 0);

static bool valid_side(enum defy_wired_side side) {
    return side == DEFY_WIRED_LEFT || side == DEFY_WIRED_RIGHT;
}

static int usable(struct defy_half_bootloader *loader, enum defy_wired_side side) {
    if (!valid_side(side)) {
        return -EINVAL;
    }
    if (loader->reset_pending) {
        return -EBUSY;
    }
    return loader->sides[side].ready ? 0 : -EACCES;
}

static uint16_t bus_address(enum defy_wired_side side) {
    return side == DEFY_WIRED_LEFT ? LEFT_ADDRESS : RIGHT_ADDRESS;
}

/* After a failed transfer, the half must begin again. */
static int after_transfer(struct defy_half_bootloader *loader, enum defy_wired_side side, int err) {
    if (err < 0) {
        loader->sides[side].ready = false;
    }
    return err;
}

static int write_bytes(struct defy_half_bootloader *loader, enum defy_wired_side side,
                       const uint8_t *data, size_t length) {
    return after_transfer(loader, side,
                          i2c_write(loader->config.bus, data, length, bus_address(side)));
}

static int read_bytes(struct defy_half_bootloader *loader, enum defy_wired_side side,
                      uint8_t *data, size_t length) {
    return after_transfer(loader, side,
                          i2c_read(loader->config.bus, data, length, bus_address(side)));
}

static int command(struct defy_half_bootloader *loader, enum defy_wired_side side, uint8_t cmd) {
    return write_bytes(loader, side, &cmd, sizeof(cmd));
}

static int address_size(struct defy_half_bootloader *loader, enum defy_wired_side side,
                        uint32_t address, uint32_t size) {
    uint8_t request[8];
    sys_put_le32(address, request);
    sys_put_le32(size, request + 4);
    return write_bytes(loader, side, request, sizeof(request));
}

int defy_half_bootloader_init(struct defy_half_bootloader *loader,
                              const struct defy_half_bootloader_config *config) {
    if (!config->bus || !config->reset[0].port || !config->reset[1].port) {
        return -EINVAL;
    }
    if (!device_is_ready(config->bus) || !gpio_is_ready_dt(&config->reset[0]) ||
        !gpio_is_ready_dt(&config->reset[1])) {
        return -ENODEV;
    }
    int err = i2c_configure(config->bus, I2C_MODE_CONTROLLER | I2C_SPEED_SET(I2C_SPEED_STANDARD));
    if (err < 0) {
        return err;
    }
    memset(loader, 0, sizeof(*loader));
    loader->config = *config;
    return 0;
}

int defy_half_bootloader_reset_all(struct defy_half_bootloader *loader) {
    if (!loader->reset_pending) {
        memset(loader->sides, 0, sizeof(loader->sides));
        loader->reset_pending = BIT_MASK(DEFY_HALF_BOOTLOADER_SIDES);
        loader->reset_asserted = 0;
    }
    loader->acquired = true;
    int result = 0;
    bool asserted = false;
    for (size_t i = 0; i < DEFY_HALF_BOOTLOADER_SIDES; i++) {
        if (!(loader->reset_pending & BIT(i)) || (loader->reset_asserted & BIT(i))) {
            continue;
        }
        asserted = true;
        int err = gpio_pin_configure_dt(&loader->config.reset[i], GPIO_OUTPUT_LOW);
        if (err == 0) {
            loader->reset_asserted |= BIT(i);
        } else if (result == 0) {
            result = err;
        }
    }
    if (asserted) {
        k_sleep(K_MSEC(RESET_PULSE_MS));
    }
    for (size_t i = 0; i < DEFY_HALF_BOOTLOADER_SIDES; i++) {
        if (!(loader->reset_pending & BIT(i))) {
            continue;
        }
        int err = gpio_pin_configure_dt(&loader->config.reset[i], GPIO_INPUT);
        if (err == 0 && (loader->reset_asserted & BIT(i))) {
            loader->reset_pending &= ~BIT(i);
            loader->reset_asserted &= ~BIT(i);
        } else if (err < 0 && result == 0) {
            result = err;
        }
    }
    k_sleep(K_MSEC(RESET_SETTLE_MS));
    return result;
}

int defy_half_bootloader_begin(struct defy_half_bootloader *loader, enum defy_wired_side side,
                               struct defy_half_bootloader_info *info) {
    if (!loader->acquired || !valid_side(side) || !info) {
        return -EINVAL;
    }
    if (loader->reset_pending) {
        return -EBUSY;
    }
    if (loader->sides[side].begun) {
        return -EALREADY;
    }
    int err = command(loader, side, CMD_BEGIN);
    if (err < 0) {
        return err;
    }
    loader->sides[side].begun = true;
    err = command(loader, side, CMD_INFO);
    if (err < 0) {
        return err;
    }
    uint8_t bytes[INFO_SIZE];
    err = read_bytes(loader, side, bytes, sizeof(bytes));
    if (err < 0) {
        return err;
    }
    struct defy_half_bootloader_info received = {
        .device_id = sys_get_le32(bytes),
        .bootloader_version = sys_get_le32(bytes + 4),
        .hardware_version = sys_get_le32(bytes + 8),
        .program_version = sys_get_le32(bytes + 12),
        .flash_start = sys_get_le32(bytes + 16),
        .validation_start = sys_get_le32(bytes + 20),
        .program_start = sys_get_le32(bytes + 24),
        .flash_available = sys_get_le32(bytes + 28),
        .erase_alignment = sys_get_le32(bytes + 32),
        .write_alignment = sys_get_le32(bytes + 36),
        .max_transmission = sys_get_le32(bytes + 40),
    };
    if (received.hardware_version != HARDWARE_VERSION ||
        received.flash_start != DEFY_HALF_FLASH_START ||
        received.validation_start != DEFY_HALF_FLASH_START ||
        received.program_start != DEFY_HALF_FLASH_START + DEFY_HALF_PAGE_SIZE ||
        received.flash_available < DEFY_HALF_FLASH_END - DEFY_HALF_FLASH_START ||
        received.erase_alignment != DEFY_HALF_SECTOR_SIZE ||
        received.write_alignment != DEFY_HALF_PAGE_SIZE ||
        received.max_transmission < DEFY_HALF_PAGE_SIZE) {
        return -ENOTSUP;
    }
    loader->sides[side].settings_reachable =
        received.flash_available >= DEFY_HALF_SETTINGS_END - DEFY_HALF_FLASH_START;
    loader->sides[side].ready = true;
    *info = received;
    return 0;
}

int defy_half_bootloader_read_seal(struct defy_half_bootloader *loader,
                                   enum defy_wired_side side, struct defy_half_seal *seal) {
    if (!seal) {
        return -EINVAL;
    }
    int err = usable(loader, side);
    if (err < 0) {
        return err;
    }
    err = command(loader, side, CMD_READ);
    if (err == 0) {
        err = address_size(loader, side, DEFY_HALF_FLASH_START, DEFY_HALF_SEAL_SIZE);
    }
    uint8_t bytes[DEFY_HALF_SEAL_SIZE];
    if (err == 0) {
        err = read_bytes(loader, side, bytes, sizeof(bytes));
    }
    if (err < 0) {
        return err;
    }
    return defy_half_seal_parse(bytes, seal) ? 0 : -EBADMSG;
}

/* Erases one sector, which the callers have checked, and needs the loader's success reply. */
static int erase(struct defy_half_bootloader *loader, enum defy_wired_side side, uint32_t address) {
    int err = command(loader, side, CMD_ERASE);
    if (err == 0) {
        err = address_size(loader, side, address, DEFY_HALF_SECTOR_SIZE);
    }
    uint8_t done = 0;
    if (err == 0) {
        err = read_bytes(loader, side, &done, sizeof(done));
    }
    if (err == 0 && done != 1) {
        loader->sides[side].ready = false;
        err = -EIO;
    }
    return err;
}

int defy_half_bootloader_erase_sector(struct defy_half_bootloader *loader,
                                      enum defy_wired_side side, uint32_t address) {
    int err = usable(loader, side);
    if (err < 0) {
        return err;
    }
    if (address < DEFY_HALF_FLASH_START || address > DEFY_HALF_FLASH_END - DEFY_HALF_SECTOR_SIZE ||
        address % DEFY_HALF_SECTOR_SIZE != 0) {
        return -EINVAL;
    }
    loader->sides[side].erased_sector = 0;
    err = erase(loader, side, address);
    if (err == 0) {
        loader->sides[side].erased_sector = address;
        loader->sides[side].next_page = address;
    }
    return err;
}

int defy_half_bootloader_erase_settings(struct defy_half_bootloader *loader,
                                        enum defy_wired_side side) {
    int err = usable(loader, side);
    if (err < 0) {
        return err;
    }
    if (!loader->sides[side].settings_reachable) {
        return -ERANGE;
    }
    loader->sides[side].erased_sector = 0;
    for (uint32_t address = DEFY_HALF_SETTINGS_START; err == 0 && address < DEFY_HALF_SETTINGS_END;
         address += DEFY_HALF_SECTOR_SIZE) {
        err = erase(loader, side, address);
    }
    return err;
}

int defy_half_bootloader_write_page(struct defy_half_bootloader *loader, enum defy_wired_side side,
                                    uint32_t address, const uint8_t data[DEFY_HALF_PAGE_SIZE]) {
    int err = usable(loader, side);
    if (err < 0) {
        return err;
    }
    if (!data || address < DEFY_HALF_FLASH_START ||
        address > DEFY_HALF_FLASH_END - DEFY_HALF_PAGE_SIZE ||
        address % DEFY_HALF_PAGE_SIZE != 0) {
        return -EINVAL;
    }
    if (loader->sides[side].erased_sector == 0 ||
        address != loader->sides[side].next_page ||
        address - loader->sides[side].erased_sector >= DEFY_HALF_SECTOR_SIZE) {
        return -EACCES;
    }
    err = command(loader, side, CMD_WRITE);
    if (err == 0) {
        err = address_size(loader, side, address, DEFY_HALF_PAGE_SIZE);
    }
    if (err == 0) {
        err = write_bytes(loader, side, data, DEFY_HALF_PAGE_SIZE);
    }
    uint8_t reply[4];
    if (err == 0) {
        err = read_bytes(loader, side, reply, sizeof(reply));
    }
    if (err == 0 && sys_get_le32(reply) != crc32_ieee(data, DEFY_HALF_PAGE_SIZE)) {
        loader->sides[side].ready = false;
        err = -EBADMSG;
    }
    if (err == 0) {
        loader->sides[side].next_page += DEFY_HALF_PAGE_SIZE;
    }
    return err;
}

int defy_half_bootloader_validate(struct defy_half_bootloader *loader, enum defy_wired_side side,
                                  struct defy_half_seal *seal) {
    if (!seal) {
        return -EINVAL;
    }
    int err = usable(loader, side);
    if (err < 0) {
        return err;
    }
    err = command(loader, side, CMD_VALIDATE);
    uint8_t done = 0;
    if (err == 0) {
        err = read_bytes(loader, side, &done, sizeof(done));
    }
    if (err < 0 || done != 1) {
        return err < 0 ? err : -EBADMSG;
    }
    return defy_half_bootloader_read_seal(loader, side, seal);
}

int defy_half_bootloader_recover(struct defy_half_bootloader *loader) {
    int err = defy_half_bootloader_reset_all(loader);
    loader->acquired = err != 0;
    return err;
}

int defy_half_bootloader_abort(struct defy_half_bootloader *loader) {
    return loader->acquired ? defy_half_bootloader_recover(loader) : 0;
}
