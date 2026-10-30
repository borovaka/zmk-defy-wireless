/*
 * The drive's disk, for Zephyr's mass storage class. Reads come from the
 * emulated FAT volume (fat.c). Writes go to the UF2 writer (uf2.c), which
 * writes ZMK's code_partition through the flash map, so no write reaches
 * outside it. Sectors that hold no UF2 block, such as the FAT, directories
 * and the host's metadata files, are dropped.
 *
 * SPDX-License-Identifier: MIT
 */

#include <errno.h>

#include <zephyr/drivers/disk.h>
#include <zephyr/storage/disk_access.h>
#include <zephyr/storage/flash_map.h>
#include <zephyr/usb/class/usbd_msc.h>

#include "fat.h"
#include "layout.h"
#include "loader.h"
#include "uf2.h"

#define DISK_NAME "UF2"

BUILD_ASSERT(DEFY_FAT_SECTOR_SIZE == DEFY_UF2_BLOCK_SIZE);
BUILD_ASSERT(DEFY_FAT_SECTOR_SIZE <= CONFIG_USBD_MSC_SCSI_BUFFER_SIZE);
BUILD_ASSERT(sizeof(DEFY_LOADER_VERSION) == 4, "The INQUIRY revision takes 4 characters");

/* The drive's one logical unit. The SCSI INQUIRY strings are padded with spaces to their
 * fields' widths: 8, 16 and 4 characters. */
USBD_DEFINE_MSC_LUN(uf2, DISK_NAME, "Defy    ", "UF2 loader      ", DEFY_LOADER_VERSION " ");

static const struct flash_area *code_area;
static struct defy_uf2_file file;

static int flash_erase(void *context, uint32_t address) {
    ARG_UNUSED(context);
    return flash_area_erase(code_area, address - DEFY_LOADER_ZMK_START, DEFY_LOADER_PAGE_SIZE);
}

static int flash_write(void *context, uint32_t address, const void *data, size_t length) {
    ARG_UNUSED(context);
    return flash_area_write(code_area, address - DEFY_LOADER_ZMK_START, data, length);
}

static int flash_read(void *context, uint32_t address, void *data, size_t length) {
    ARG_UNUSED(context);
    return flash_area_read(code_area, address - DEFY_LOADER_ZMK_START, data, length);
}

static const struct defy_uf2_flash flash = {
    .erase = flash_erase,
    .write = flash_write,
    .read = flash_read,
};

static int disk_init(struct disk_info *disk) {
    ARG_UNUSED(disk);
    return 0;
}

static int disk_status(struct disk_info *disk) {
    ARG_UNUSED(disk);
    return DISK_STATUS_OK;
}

static int disk_read(struct disk_info *disk, uint8_t *data, uint32_t start, uint32_t count) {
    ARG_UNUSED(disk);
    for (uint32_t i = 0; i < count; i++) {
        defy_fat_read(start + i, data + i * DEFY_FAT_SECTOR_SIZE);
    }
    return 0;
}

static int disk_write(struct disk_info *disk, const uint8_t *data, uint32_t start,
                      uint32_t count) {
    ARG_UNUSED(disk);
    ARG_UNUSED(start);
    int err = 0;
    for (uint32_t i = 0; i < count; i++) {
        struct defy_uf2_block block;
        if (defy_uf2_parse(data + i * DEFY_UF2_BLOCK_SIZE, &block) != DEFY_UF2_BLOCK) {
            continue;
        }
        switch (defy_uf2_file_add(&file, &block, &flash)) {
        case DEFY_UF2_STORED:
            defy_loader_led_block();
            break;
        case DEFY_UF2_COMPLETE:
            defy_loader_led_block();
            defy_loader_copied();
            break;
        case DEFY_UF2_FAILED:
            err = -EIO;
            break;
        default:
            break;
        }
    }
    return err;
}

static int disk_ioctl(struct disk_info *disk, uint8_t command, void *value) {
    ARG_UNUSED(disk);
    switch (command) {
    case DISK_IOCTL_GET_SECTOR_COUNT:
        *(uint32_t *)value = DEFY_FAT_SECTOR_COUNT;
        return 0;
    case DISK_IOCTL_GET_SECTOR_SIZE:
        *(uint32_t *)value = DEFY_FAT_SECTOR_SIZE;
        return 0;
    case DISK_IOCTL_GET_ERASE_BLOCK_SZ:
        *(uint32_t *)value = 1;
        return 0;
    case DISK_IOCTL_CTRL_SYNC:
    case DISK_IOCTL_CTRL_INIT:
    case DISK_IOCTL_CTRL_DEINIT:
        return 0;
    default:
        return -EINVAL;
    }
}

static const struct disk_operations disk_operations = {
    .init = disk_init,
    .status = disk_status,
    .read = disk_read,
    .write = disk_write,
    .ioctl = disk_ioctl,
};

static struct disk_info disk = {
    .name = DISK_NAME,
    .ops = &disk_operations,
};

/* The disk as the mass storage class finds it, by name through Zephyr's disk access:
 * ready, with the volume's sector count and size, and a boot sector that ends in the
 * FAT signature. */
static int disk_check(void) {
    static uint8_t sector[DEFY_FAT_SECTOR_SIZE];
    uint32_t count = 0;
    uint32_t size = 0;
    if (disk_access_status(DISK_NAME) != DISK_STATUS_OK ||
        disk_access_ioctl(DISK_NAME, DISK_IOCTL_GET_SECTOR_COUNT, &count) ||
        disk_access_ioctl(DISK_NAME, DISK_IOCTL_GET_SECTOR_SIZE, &size) ||
        count != DEFY_FAT_SECTOR_COUNT || size != DEFY_FAT_SECTOR_SIZE ||
        disk_access_read(DISK_NAME, sector, 0, 1) ||
        sector[DEFY_FAT_SECTOR_SIZE - 2] != 0x55 || sector[DEFY_FAT_SECTOR_SIZE - 1] != 0xaa) {
        return -EIO;
    }
    return 0;
}

int defy_loader_disk_init(void) {
    int err = flash_area_open(FIXED_PARTITION_ID(code_partition), &code_area);
    if (!err) {
        defy_uf2_file_init(&file);
        err = disk_access_register(&disk);
    }
    return err ? err : disk_check();
}

bool defy_loader_disk_written(void) { return file.state != DEFY_UF2_IDLE; }
