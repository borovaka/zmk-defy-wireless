/* SPDX-License-Identifier: MIT */

#include <stddef.h>
#include <string.h>

#include "fat.h"

#define RESERVED_SECTORS 1u
#define FAT_COUNT 2u
#define SECTORS_PER_FAT 32u
/* The usual FAT16 root directory, 32 sectors: room for the metadata files hosts add. */
#define ROOT_ENTRIES 512u
#define DIRECTORY_ENTRY_SIZE 32u
#define ROOT_SECTORS (ROOT_ENTRIES * DIRECTORY_ENTRY_SIZE / DEFY_FAT_SECTOR_SIZE)
#define FAT_START RESERVED_SECTORS
#define ROOT_START (FAT_START + FAT_COUNT * SECTORS_PER_FAT)
#define DATA_START (ROOT_START + ROOT_SECTORS)
/* One sector per cluster; data clusters are numbered from 2. */
#define CLUSTERS (DEFY_FAT_SECTOR_COUNT - DATA_START)
#define FIRST_CLUSTER 2u

#define FAT16_END_OF_CHAIN 0xffffu
#define FAT16_MEDIA 0xfff8u

#define ATTRIBUTE_READ_ONLY 0x01u
#define ATTRIBUTE_VOLUME_ID 0x08u
/* 2026-10-27 12:00, in FAT's date and time formats. */
#define FILE_DATE (((2026u - 1980u) << 9) | (10u << 5) | 27u)
#define FILE_TIME (12u << 11)

_Static_assert(CLUSTERS >= 4085 && CLUSTERS < 65525, "The cluster count makes a FAT16 volume");
_Static_assert((CLUSTERS + FIRST_CLUSTER) * 2 <= SECTORS_PER_FAT * DEFY_FAT_SECTOR_SIZE,
               "Each FAT holds an entry for every cluster");

#define VOLUME_LABEL "DEFY       "

static const char info[] = "UF2 Bootloader " DEFY_LOADER_VERSION " zmk-defy-wireless\r\n"
                           "Model: Dygma Neuron (wireless)\r\n"
                           "Board-ID: " DEFY_LOADER_BOARD_ID "\r\n";

static const char index_page[] =
    "<!doctype html>\n"
    "<html><head><meta http-equiv=\"refresh\" "
    "content=\"0;URL='https://github.com/borovaka/zmk-defy-wireless#readme'\"></head>\n"
    "<body><a href=\"https://github.com/borovaka/zmk-defy-wireless#readme\">"
    "zmk-defy-wireless</a></body></html>\n";

struct file {
    /* 8.3 name, padded with spaces, without the dot. */
    char name[11];
    const char *content;
    uint32_t size;
};

/* Each file takes one cluster, from FIRST_CLUSTER on in this order. */
static const struct file files[] = {
    {{'I', 'N', 'F', 'O', '_', 'U', 'F', '2', 'T', 'X', 'T'}, info, sizeof(info) - 1},
    {{'I', 'N', 'D', 'E', 'X', ' ', ' ', ' ', 'H', 'T', 'M'}, index_page, sizeof(index_page) - 1},
};

#define FILE_COUNT (sizeof(files) / sizeof(files[0]))

_Static_assert(sizeof(info) - 1 <= DEFY_FAT_SECTOR_SIZE, "INFO_UF2.TXT fits one cluster");
_Static_assert(sizeof(index_page) - 1 <= DEFY_FAT_SECTOR_SIZE, "INDEX.HTM fits one cluster");

static void put_le16(uint8_t *bytes, uint16_t value) {
    bytes[0] = value & 0xff;
    bytes[1] = value >> 8;
}

static void put_le32(uint8_t *bytes, uint32_t value) {
    put_le16(bytes, value & 0xffff);
    put_le16(bytes + 2, value >> 16);
}

static void boot_sector(uint8_t *sector) {
    static const uint8_t jump[] = {0xeb, 0x3c, 0x90};
    memcpy(sector, jump, sizeof(jump));
    memcpy(sector + 3, "MSWIN4.1", 8);
    put_le16(sector + 11, DEFY_FAT_SECTOR_SIZE);
    sector[13] = 1; /* sectors per cluster */
    put_le16(sector + 14, RESERVED_SECTORS);
    sector[16] = FAT_COUNT;
    put_le16(sector + 17, ROOT_ENTRIES);
    put_le16(sector + 19, DEFY_FAT_SECTOR_COUNT);
    sector[21] = FAT16_MEDIA & 0xff;
    put_le16(sector + 22, SECTORS_PER_FAT);
    put_le16(sector + 24, 1); /* sectors per track */
    put_le16(sector + 26, 1); /* heads */
    sector[36] = 0x80;        /* drive number */
    sector[38] = 0x29;        /* extended boot signature: the next three fields */
    put_le32(sector + 39, 0x44454659u);
    memcpy(sector + 43, VOLUME_LABEL, 11);
    memcpy(sector + 54, "FAT16   ", 8);
    sector[510] = 0x55;
    sector[511] = 0xaa;
}

static void fat_sector(uint32_t index, uint8_t *sector) {
    if (index != 0) {
        return;
    }
    put_le16(sector, FAT16_MEDIA);
    put_le16(sector + 2, FAT16_END_OF_CHAIN);
    for (uint32_t i = 0; i < FILE_COUNT; i++) {
        put_le16(sector + (FIRST_CLUSTER + i) * 2, FAT16_END_OF_CHAIN);
    }
}

static void directory_entry(uint8_t *entry, const char *name, uint8_t attributes,
                            uint16_t cluster, uint32_t size) {
    memcpy(entry, name, 11);
    entry[11] = attributes;
    put_le16(entry + 14, FILE_TIME); /* created */
    put_le16(entry + 16, FILE_DATE);
    put_le16(entry + 18, FILE_DATE); /* accessed */
    put_le16(entry + 22, FILE_TIME); /* written */
    put_le16(entry + 24, FILE_DATE);
    put_le16(entry + 26, cluster);
    put_le32(entry + 28, size);
}

static void root_sector(uint32_t index, uint8_t *sector) {
    if (index != 0) {
        return;
    }
    directory_entry(sector, VOLUME_LABEL, ATTRIBUTE_VOLUME_ID, 0, 0);
    for (uint32_t i = 0; i < FILE_COUNT; i++) {
        directory_entry(sector + (i + 1) * DIRECTORY_ENTRY_SIZE, files[i].name,
                        ATTRIBUTE_READ_ONLY, FIRST_CLUSTER + i, files[i].size);
    }
}

void defy_fat_read(uint32_t sector, uint8_t *buffer) {
    memset(buffer, 0, DEFY_FAT_SECTOR_SIZE);
    if (sector == 0) {
        boot_sector(buffer);
    } else if (sector < ROOT_START) {
        fat_sector((sector - FAT_START) % SECTORS_PER_FAT, buffer);
    } else if (sector < DATA_START) {
        root_sector(sector - ROOT_START, buffer);
    } else if (sector - DATA_START < FILE_COUNT) {
        const struct file *file = &files[sector - DATA_START];
        memcpy(buffer, file->content, file->size);
    }
}
