/* SPDX-License-Identifier: MIT */

#include <assert.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "../../../apps/defy_loader/src/fat.c"
#include "../../../apps/defy_loader/src/uf2.h"

/* The volume as a host mounts it: the boot sector's fields, then the FATs, the root
 * directory and the files' cluster chains, all read through defy_fat_read(). */
struct volume {
    uint32_t bytes_per_sector;
    uint32_t sectors_per_cluster;
    uint32_t reserved;
    uint32_t fat_count;
    uint32_t root_entries;
    uint32_t total_sectors;
    uint32_t sectors_per_fat;
    uint32_t root_start;
    uint32_t root_sectors;
    uint32_t data_start;
    uint32_t clusters;
    uint16_t *fat;
};

struct found {
    char name[13];
    uint8_t attributes;
    uint16_t cluster;
    uint32_t size;
};

static uint16_t get16(const uint8_t *bytes) { return (uint16_t)(bytes[0] | bytes[1] << 8); }

static uint32_t get32(const uint8_t *bytes) {
    return get16(bytes) | (uint32_t)get16(bytes + 2) << 16;
}

static void read_sector(uint32_t sector, uint8_t *buffer) {
    /* Guard bytes around the sector show any write past its end. */
    uint8_t guarded[DEFY_FAT_SECTOR_SIZE + 64];
    memset(guarded, 0xa5, sizeof(guarded));
    defy_fat_read(sector, guarded + 32);
    for (int i = 0; i < 32; i++) {
        assert(guarded[i] == 0xa5 && guarded[DEFY_FAT_SECTOR_SIZE + 32 + i] == 0xa5);
    }
    memcpy(buffer, guarded + 32, DEFY_FAT_SECTOR_SIZE);
}

static void mount(struct volume *volume) {
    uint8_t boot[DEFY_FAT_SECTOR_SIZE];
    read_sector(0, boot);
    assert(boot[0] == 0xeb && boot[2] == 0x90);
    assert(boot[510] == 0x55 && boot[511] == 0xaa);
    volume->bytes_per_sector = get16(boot + 11);
    volume->sectors_per_cluster = boot[13];
    volume->reserved = get16(boot + 14);
    volume->fat_count = boot[16];
    volume->root_entries = get16(boot + 17);
    volume->total_sectors = get16(boot + 19) ? get16(boot + 19) : get32(boot + 32);
    volume->sectors_per_fat = get16(boot + 22);
    assert(volume->bytes_per_sector == 512);
    assert(volume->sectors_per_cluster == 1);
    assert(volume->reserved >= 1 && volume->fat_count == 2);
    assert(volume->total_sectors == DEFY_FAT_SECTOR_COUNT);
    assert(boot[21] == 0xf8);
    assert(volume->sectors_per_fat > 0);
    assert(boot[38] == 0x29);
    assert(memcmp(boot + 43, "DEFY       ", 11) == 0);
    assert(memcmp(boot + 54, "FAT16   ", 8) == 0);

    volume->root_sectors = (volume->root_entries * 32 + 511) / 512;
    volume->root_start = volume->reserved + volume->fat_count * volume->sectors_per_fat;
    volume->data_start = volume->root_start + volume->root_sectors;
    volume->clusters = (volume->total_sectors - volume->data_start) / volume->sectors_per_cluster;

    /* Both FATs, read whole, are the same. */
    uint32_t entries = volume->sectors_per_fat * 256;
    volume->fat = malloc(entries * sizeof(uint16_t));
    assert(volume->fat != NULL);
    for (uint32_t s = 0; s < volume->sectors_per_fat; s++) {
        uint8_t first[DEFY_FAT_SECTOR_SIZE], second[DEFY_FAT_SECTOR_SIZE];
        read_sector(volume->reserved + s, first);
        read_sector(volume->reserved + volume->sectors_per_fat + s, second);
        assert(memcmp(first, second, sizeof(first)) == 0);
        for (uint32_t i = 0; i < 256; i++) {
            volume->fat[s * 256 + i] = get16(first + i * 2);
        }
    }
    assert(volume->fat[0] == 0xfff8 && volume->fat[1] == 0xffff);
}

static size_t list_root(const struct volume *volume, struct found *found, size_t capacity,
                        char label[12]) {
    size_t count = 0;
    int ended = 0;
    label[0] = 0;
    for (uint32_t s = 0; s < volume->root_sectors; s++) {
        uint8_t sector[DEFY_FAT_SECTOR_SIZE];
        read_sector(volume->root_start + s, sector);
        for (uint32_t e = 0; e < 16; e++) {
            const uint8_t *entry = sector + e * 32;
            if (entry[0] == 0) {
                ended = 1;
            }
            if (ended) {
                /* Past the end marker, every entry is free. */
                for (int i = 0; i < 32; i++) {
                    assert(entry[i] == 0);
                }
                continue;
            }
            assert(entry[0] != 0xe5);
            if (entry[11] & 0x08) {
                assert(label[0] == 0);
                memcpy(label, entry, 11);
                label[11] = 0;
                assert(get16(entry + 26) == 0 && get32(entry + 28) == 0);
                continue;
            }
            assert(count < capacity);
            struct found *file = &found[count++];
            /* NAME    EXT -> NAME.EXT */
            size_t length = 0;
            for (int i = 0; i < 8 && entry[i] != ' '; i++) {
                file->name[length++] = (char)entry[i];
            }
            file->name[length++] = '.';
            for (int i = 8; i < 11 && entry[i] != ' '; i++) {
                file->name[length++] = (char)entry[i];
            }
            file->name[length] = 0;
            file->attributes = entry[11];
            file->cluster = get16(entry + 26);
            file->size = get32(entry + 28);
            assert(get16(entry + 20) == 0);
            /* A date between 1980 and 2107 with a valid month and day. */
            uint16_t date = get16(entry + 24);
            assert((date >> 5 & 15) >= 1 && (date >> 5 & 15) <= 12 && (date & 31) >= 1);
        }
    }
    return count;
}

/* Follows the file's chain and reads its content; the chain must end with the file. */
static char *read_file(const struct volume *volume, const struct found *file, uint8_t *used) {
    char *content = calloc(1, file->size + 1);
    assert(content != NULL);
    uint32_t cluster = file->cluster;
    uint32_t offset = 0;
    while (offset < file->size) {
        assert(cluster >= 2 && cluster < volume->clusters + 2);
        assert(!used[cluster]);
        used[cluster] = 1;
        uint8_t sector[DEFY_FAT_SECTOR_SIZE];
        read_sector(volume->data_start + (cluster - 2) * volume->sectors_per_cluster, sector);
        uint32_t length = file->size - offset < 512 ? file->size - offset : 512;
        memcpy(content + offset, sector, length);
        offset += length;
        /* The rest of the last cluster is zero. */
        for (uint32_t i = length; i < 512; i++) {
            assert(sector[i] == 0);
        }
        uint16_t next = volume->fat[cluster];
        if (offset < file->size) {
            cluster = next;
        } else {
            assert(next >= 0xfff8);
        }
    }
    assert(strlen(content) == file->size);
    return content;
}

static void test_volume_as_a_host_reads_it(void) {
    struct volume volume;
    mount(&volume);
    struct found found[8];
    char label[12];
    size_t count = list_root(&volume, found, 8, label);
    assert(strcmp(label, "DEFY       ") == 0);
    assert(count == 2);

    uint8_t *used = calloc(volume.clusters + 2, 1);
    assert(used != NULL);
    int info_seen = 0, index_seen = 0;
    for (size_t i = 0; i < count; i++) {
        assert(!(found[i].attributes & 0x18));
        char *content = read_file(&volume, &found[i], used);
        if (strcmp(found[i].name, "INFO_UF2.TXT") == 0) {
            info_seen = 1;
            /* The usual lines, the first with the loader's version, and the Board-ID that
             * the zmk variant's uf2 runner matches. */
            assert(strcmp(content, "UF2 Bootloader 1.1 zmk-defy-wireless\r\n"
                                   "Model: Dygma Neuron (wireless)\r\n"
                                   "Board-ID: nRF52833-DefyNeuron-v1\r\n") == 0);
        } else if (strcmp(found[i].name, "INDEX.HTM") == 0) {
            index_seen = 1;
            assert(strstr(content, "https://github.com/borovaka/zmk-defy-wireless#readme"));
            assert(strstr(content, "http-equiv=\"refresh\""));
        } else {
            assert(0 && "unexpected file");
        }
        free(content);
    }
    assert(info_seen && index_seen);

    /* Every other cluster is free and reads as zeros, and there is room for a UF2 file of
     * ZMK's whole range, one 512-byte block per 256 bytes. */
    uint32_t free_clusters = 0;
    for (uint32_t cluster = 2; cluster < volume.clusters + 2; cluster++) {
        if (used[cluster]) {
            continue;
        }
        assert(volume.fat[cluster] == 0);
        free_clusters++;
    }
    assert(free_clusters * 512u >= DEFY_UF2_MAX_BLOCKS * DEFY_UF2_BLOCK_SIZE);
    for (uint32_t i = volume.clusters + 2; i < volume.sectors_per_fat * 256; i++) {
        assert(volume.fat[i] == 0);
    }
    for (uint32_t sector = volume.data_start; sector < volume.total_sectors; sector++) {
        uint32_t cluster = (sector - volume.data_start) / volume.sectors_per_cluster + 2;
        if (used[cluster]) {
            continue;
        }
        uint8_t buffer[DEFY_FAT_SECTOR_SIZE];
        read_sector(sector, buffer);
        for (int i = 0; i < 512; i++) {
            assert(buffer[i] == 0);
        }
    }
    free(used);
    free(volume.fat);
}

int main(void) {
    test_volume_as_a_host_reads_it();
    return 0;
}
