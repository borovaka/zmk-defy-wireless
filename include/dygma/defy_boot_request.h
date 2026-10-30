/*
 * Dygma's boot request, which makes n2_bootloader wait for commands after the
 * next reset; see docs/software-bootloader.md. Both the ZMK Neuron's 1200-baud
 * entry (bootloader/defy_neuron_bootloader.c) and the UF2 loader
 * (apps/defy_loader/src/main.c) write it.
 *
 * The functions are static inline: each image includes this header from the
 * one source file that writes the request.
 *
 * SPDX-License-Identifier: MIT
 */

#ifndef DYGMA_DEFY_BOOT_REQUEST_H
#define DYGMA_DEFY_BOOT_REQUEST_H

#include <errno.h>
#include <stdint.h>
#include <string.h>

#include <zephyr/devicetree.h>
#include <zephyr/drivers/flash.h>
#include <zephyr/storage/flash_map.h>
#include <zephyr/sys/byteorder.h>
#include <zephyr/sys/crc.h>
#include <zephyr/sys/util.h>
#include <zephyr/toolchain.h>

/* Dygma's n2_bootloader 1.0.0, and the CRC32 of its code. */
#define DEFY_BOOT_ADDRESS 0x77000u
#define DEFY_BOOT_LENGTH 0x6f54u
#define DEFY_BOOT_CRC 0x080f9450u
/* The page that holds the request. */
#define DEFY_BOOT_REQUEST_ADDRESS 0x7f000u
#define DEFY_BOOT_REQUEST_PAGE_SIZE 0x1000u
/* A host asks for the bootloader by setting a serial port to this rate with DTR low, and
 * the rate and DTR are checked again this much later. */
#define DEFY_BOOT_TOUCH_BAUD 1200u
#define DEFY_BOOT_TOUCH_DELAY_MS 250
/* The request record's version, and the request that makes n2_bootloader wait for commands. */
#define DEFY_BOOT_REQUEST_VERSION 1u
#define DEFY_BOOT_REQUEST_ENTER 1u

BUILD_ASSERT(IS_ENABLED(CONFIG_SOC_NRF52833));
BUILD_ASSERT(FIXED_PARTITION_OFFSET(boot_partition) == DEFY_BOOT_ADDRESS);
BUILD_ASSERT(FIXED_PARTITION_SIZE(boot_partition) ==
             DEFY_BOOT_REQUEST_ADDRESS - DEFY_BOOT_ADDRESS);
BUILD_ASSERT(FIXED_PARTITION_OFFSET(boot_request_partition) == DEFY_BOOT_REQUEST_ADDRESS);
BUILD_ASSERT(FIXED_PARTITION_SIZE(boot_request_partition) == DEFY_BOOT_REQUEST_PAGE_SIZE);
BUILD_ASSERT(DT_SAME_NODE(DT_MTD_FROM_FIXED_PARTITION(DT_NODELABEL(boot_partition)),
                          DT_MTD_FROM_FIXED_PARTITION(DT_NODELABEL(boot_request_partition))));

static inline int defy_boot_request_verify_bootloader(const struct flash_area *boot) {
    uint8_t buffer[128];
    uint32_t crc = 0;
    for (size_t offset = 0; offset < DEFY_BOOT_LENGTH; offset += sizeof(buffer)) {
        size_t length = MIN(sizeof(buffer), DEFY_BOOT_LENGTH - offset);
        int err = flash_area_read(boot, offset, buffer, length);
        if (err) {
            return err;
        }
        crc = crc32_ieee_update(crc, buffer, length);
    }
    return crc == DEFY_BOOT_CRC ? 0 : -ENOTSUP;
}

/*
 * Checks the request page's place and size, and the bootloader's CRC32, then
 * writes the request and reads it back. Erases and writes nothing when the
 * page already holds the request or a check fails. 0 once the request is in
 * place.
 */
static inline int defy_boot_request_write(void) {
    const struct flash_area *boot;
    const struct flash_area *request;
    int err = flash_area_open(FIXED_PARTITION_ID(boot_partition), &boot);
    if (err) {
        return err;
    }
    err = flash_area_open(FIXED_PARTITION_ID(boot_request_partition), &request);
    if (err) {
        flash_area_close(boot);
        return err;
    }

    /* The partitions' places and their device are checked at build time, above. */
    struct flash_pages_info page;
    if (flash_area_align(request) != sizeof(uint32_t)) {
        err = -EINVAL;
        goto out;
    }
    err = flash_get_page_info_by_offs(request->fa_dev, request->fa_off, &page);
    if (err) {
        goto out;
    }
    if (page.start_offset != DEFY_BOOT_REQUEST_ADDRESS ||
        page.size != DEFY_BOOT_REQUEST_PAGE_SIZE) {
        err = -EINVAL;
        goto out;
    }
    err = defy_boot_request_verify_bootloader(boot);
    if (err) {
        goto out;
    }

    /* The version, the record's size, its CRC32 with this word zeroed, and the request. */
    uint32_t record[] = {sys_cpu_to_le32(DEFY_BOOT_REQUEST_VERSION),
                         sys_cpu_to_le32(4 * sizeof(uint32_t)), 0,
                         sys_cpu_to_le32(DEFY_BOOT_REQUEST_ENTER)};
    uint32_t readback[ARRAY_SIZE(record)];
    record[2] = sys_cpu_to_le32(crc32_ieee((uint8_t *)record, sizeof(record)));
    err = flash_area_read(request, 0, readback, sizeof(readback));
    if (err || memcmp(record, readback, sizeof(record)) == 0) {
        goto out;
    }
    err = flash_area_erase(request, 0, request->fa_size);
    if (err) {
        goto out;
    }
    err = flash_area_write(request, 0, record, sizeof(record));
    if (err) {
        goto out;
    }
    err = flash_area_read(request, 0, readback, sizeof(readback));
    if (!err && memcmp(record, readback, sizeof(record)) != 0) {
        err = -EIO;
    }

out:
    flash_area_close(request);
    flash_area_close(boot);
    return err;
}

#endif /* DYGMA_DEFY_BOOT_REQUEST_H */
