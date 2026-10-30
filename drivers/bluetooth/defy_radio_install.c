/*
 * Installing the radio firmware through rf_bootloader, the way the stock
 * keyscanner installs its radio application: seal, erase, write, validate,
 * finish. Two differences: the first 64 bytes go last, so an interrupted
 * install leaves erased vectors, which S113 answers by sleeping until the next
 * reset rather than jumping into a partial image; and the image is checked
 * against its seal before anything is erased.
 *
 * Every word is programmed at most twice between erases, the nRF52820's
 * limit: once by its own write, and once more when the bootloader rewrites
 * the word past the previous write with its current value.
 *
 * SPDX-License-Identifier: MIT
 */

#include <errno.h>
#include <string.h>

#include <dygma/defy_radio_factory.h>

static bool image_matches_seal(const uint8_t *image, size_t size, const uint8_t *seal) {
    return image != NULL && seal != NULL &&
           defy_radio_get32(seal + DEFY_RADIO_SEAL_START_OFFSET) == DEFY_RADIO_SLOT_START &&
           defy_radio_get32(seal + DEFY_RADIO_SEAL_LENGTH_OFFSET) == size &&
           size >= DEFY_RADIO_IMAGE_MIN && size <= DEFY_RADIO_IMAGE_MAX && (size & 3u) == 0 &&
           defy_radio_get32(seal + DEFY_RADIO_SEAL_IMAGE_CRC_OFFSET) ==
               defy_radio_crc32(image, size);
}

static void report(struct defy_radio_factory *factory, size_t written, size_t size) {
    if (factory->bus.progress != NULL) {
        factory->bus.progress(factory->bus.context, written, size);
    }
}

/* Writes the chunk at offset and adds its length to *written. */
static int write_chunk(struct defy_radio_factory *factory, const uint8_t *image, size_t size,
                       size_t offset, size_t *written) {
    size_t length =
        size - offset < DEFY_RADIO_MAX_TRANSFER ? size - offset : DEFY_RADIO_MAX_TRANSFER;
    int result = defy_radio_factory_write(factory, DEFY_RADIO_SLOT_START + offset, image + offset,
                                          length);

    if (result == 0) {
        *written += length;
        report(factory, *written, size);
    }
    return result;
}

static int install(struct defy_radio_factory *factory, const uint8_t *image, size_t size,
                   const uint8_t *seal) {
    size_t written = 0;
    int result = defy_radio_factory_seal(factory, seal);

    if (result != 0) {
        return result;
    }

    /* Through the page holding the word the bootloader programs past the image. */
    for (uint32_t address = DEFY_RADIO_SLOT_START; address < DEFY_RADIO_SLOT_START + size + 4u;
         address += DEFY_RADIO_PAGE_SIZE) {
        result = defy_radio_factory_erase(factory, address);
        if (result != 0) {
            return result;
        }
    }
    report(factory, 0, size);

    for (size_t offset = DEFY_RADIO_MAX_TRANSFER; offset < size;
         offset += DEFY_RADIO_MAX_TRANSFER) {
        result = write_chunk(factory, image, size, offset, &written);
        if (result != 0) {
            return result;
        }
    }
    result = write_chunk(factory, image, size, 0, &written);
    if (result != 0) {
        return result;
    }

    /* The bootloader checks the CRC of the whole image in flash against the seal. */
    result = defy_radio_factory_validate(factory);
    if (result != 0) {
        return result;
    }
    return defy_radio_factory_finish(factory);
}

int defy_radio_factory_install(struct defy_radio_factory *factory, const uint8_t *image,
                               size_t size, const uint8_t *seal, bool *installed) {
    if (factory == NULL || installed == NULL || !factory->begun || factory->failed ||
        !image_matches_seal(image, size, seal)) {
        return -EINVAL;
    }
    *installed = false;

    /* Without S113's identification, INFO reports 0x1000: not a radio this can write. */
    if (factory->info.program_start != DEFY_RADIO_SLOT_START) {
        return -ENOTSUP;
    }

    if (factory->info.program_version ==
        defy_radio_get32(seal + DEFY_RADIO_SEAL_VERSION_OFFSET)) {
        uint8_t stored[DEFY_RADIO_SEAL_SIZE];
        int result = defy_radio_factory_read(factory, DEFY_RADIO_SEAL_ADDRESS, stored,
                                             sizeof(stored));

        if (result != 0) {
            return result;
        }
        if (memcmp(stored, seal, sizeof(stored)) == 0) {
            return defy_radio_factory_leave(factory);
        }
    }

    *installed = true;
    return install(factory, image, size, seal);
}
