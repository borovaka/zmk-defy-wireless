/*
 * The RP2040's side of Dygma's rf_bootloader protocol on the nRF52820, and
 * installing the radio firmware through it the way the stock keyscanner
 * installs its radio application.
 *
 * Writes are limited to the stock application slot, 0x1C000 to 0x39000, so
 * the MBR, S113, the bootloader and its state pages are never touched. The
 * bootloader stores the seal at 0x3D000 itself, after validating the image.
 *
 * Plain C with no Zephyr dependencies, for the host tests in
 * tests/host/defy_radio_factory.
 *
 * SPDX-License-Identifier: MIT
 */

#ifndef DYGMA_DEFY_RADIO_FACTORY_H
#define DYGMA_DEFY_RADIO_FACTORY_H

#include <errno.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* init() takes timeouts from this to DEFY_RADIO_FACTORY_MAX_TIMEOUT_MS. */
#define DEFY_RADIO_FACTORY_MIN_TIMEOUT_MS 500u
#define DEFY_RADIO_FACTORY_MAX_TIMEOUT_MS 600000u

#define DEFY_RADIO_SLOT_START 0x1c000u
#define DEFY_RADIO_SLOT_END 0x39000u
/* rf_bootloader programs one word past every write, so the slot's last word stays free. */
#define DEFY_RADIO_IMAGE_MAX (DEFY_RADIO_SLOT_END - DEFY_RADIO_SLOT_START - 4u)
/* An initial stack pointer and a reset vector. */
#define DEFY_RADIO_IMAGE_MIN 8u
#define DEFY_RADIO_SEAL_ADDRESS 0x3d000u
#define DEFY_RADIO_SEAL_SIZE 28u

/* rf_bootloader's erase unit. */
#define DEFY_RADIO_PAGE_SIZE 4096u
/* The longest payload of one transfer, and so of one write or read. */
#define DEFY_RADIO_MAX_TRANSFER 64u

/* A transfer's header: its length, the header's two bytes included, then its kind. */
#define DEFY_RADIO_LINK_HEADER_SIZE 2u

/*
 * rf_bootloader's link state. A status poll is answered with a bare header
 * whose second byte is the state.
 */
enum defy_radio_link_state {
    /* The last transfer succeeded. */
    DEFY_RADIO_STATE_SUCCEEDED = 0x80,
    /* The last transfer failed. */
    DEFY_RADIO_STATE_FAILED = 0x81,
    /* Ready for a command. */
    DEFY_RADIO_STATE_READY = 0x82,
    /* The last transfer succeeded, and the bootloader is now busy. */
    DEFY_RADIO_STATE_SUCCEEDED_BUSY = 0x83,
    DEFY_RADIO_STATE_BUSY = 0x84,
    /* A reply is waiting to be read. */
    DEFY_RADIO_STATE_REPLY_WAITING = 0x85,
};

/*
 * The seal: seven little-endian words, as scripts/package_radio.py packs them.
 * Its CRC is over the whole seal with the CRC word itself as zero.
 */
#define DEFY_RADIO_SEAL_KIND 1u
#define DEFY_RADIO_SEAL_KIND_OFFSET 0u
#define DEFY_RADIO_SEAL_SIZE_OFFSET 4u
#define DEFY_RADIO_SEAL_CRC_OFFSET 8u
#define DEFY_RADIO_SEAL_START_OFFSET 12u
#define DEFY_RADIO_SEAL_LENGTH_OFFSET 16u
#define DEFY_RADIO_SEAL_IMAGE_CRC_OFFSET 20u
#define DEFY_RADIO_SEAL_VERSION_OFFSET 24u

/* finish() result when the request may have reached the radio but its delivery is unknown. */
#define DEFY_RADIO_FINISH_UNCONFIRMED (-EINPROGRESS)

/*
 * The SPI master. exchange() clocks length bytes with chip select held low,
 * release() raises it. The protocol runs on its own deadlines through now_ms()
 * and wait_ms(). progress(), which may be NULL, hears how much of an image
 * install() has written, between operations, with chip select high.
 */
struct defy_radio_factory_bus {
    void *context;
    int (*exchange)(void *context, const uint8_t *tx, uint8_t *rx, size_t length);
    int (*release)(void *context);
    uint64_t (*now_ms)(void *context);
    void (*wait_ms)(void *context, uint32_t milliseconds);
    void (*progress)(void *context, size_t written, size_t size);
};

/* rf_bootloader's INFO reply. */
struct defy_radio_factory_info {
    uint32_t device_id;
    uint32_t bootloader_version;
    uint32_t hardware_version;
    uint32_t program_version;
    uint32_t flash_start;
    uint32_t program_start;
    uint32_t validation_start;
    uint32_t flash_available;
    uint32_t erase_alignment;
    uint32_t write_alignment;
    uint32_t max_transfer;
};

enum defy_radio_factory_phase {
    DEFY_RADIO_PHASE_NONE,
    DEFY_RADIO_PHASE_STATUS,
    DEFY_RADIO_PHASE_HELLO,
    DEFY_RADIO_PHASE_HELLO_STATUS,
    DEFY_RADIO_PHASE_SEND_HEADER,
    DEFY_RADIO_PHASE_SEND_PAYLOAD,
    DEFY_RADIO_PHASE_SEND_STATUS,
    DEFY_RADIO_PHASE_RECEIVE_REQUEST,
    DEFY_RADIO_PHASE_RECEIVE_HEADER,
    DEFY_RADIO_PHASE_RECEIVE_PAYLOAD,
    DEFY_RADIO_PHASE_INFO,
    DEFY_RADIO_PHASE_RELEASE,
};

/*
 * Where the session's last exchange stood, for the start-up report
 * (lighting/defy_radio_install_leds.c).
 */
struct defy_radio_factory_trace {
    enum defy_radio_factory_phase phase;
    uint8_t command;
    uint8_t tx[2];
    uint8_t rx[2];
    size_t length;
    unsigned int exchanges;
};

/* One transport session. Any failure ends it; start a new one with init() and begin(). */
struct defy_radio_factory {
    struct defy_radio_factory_bus bus;
    uint32_t timeout_ms;
    bool failed;
    bool begun;
    bool seal_sent;
    bool sealed;
    bool validated;
    bool metadata_erased;
    /* F was clocked out in full, so the radio has it. */
    bool finish_transmitted;
    struct defy_radio_factory_info info;
    struct defy_radio_factory_trace trace;
};

/* The little-endian word at data, as in the seal and the bootloader's replies. */
static inline uint32_t defy_radio_get32(const uint8_t *data) {
    return (uint32_t)data[0] | (uint32_t)data[1] << 8 | (uint32_t)data[2] << 16 |
           (uint32_t)data[3] << 24;
}

/* Whether a status poll's answer is Dygma's protocol: rf_bootloader or the stock application. */
static inline bool defy_radio_factory_is_status(const uint8_t answer[2]) {
    return answer[0] == DEFY_RADIO_LINK_HEADER_SIZE && answer[1] >= DEFY_RADIO_STATE_SUCCEEDED &&
           answer[1] <= DEFY_RADIO_STATE_REPLY_WAITING;
}

int defy_radio_factory_init(struct defy_radio_factory *factory,
                            const struct defy_radio_factory_bus *bus, uint32_t timeout_ms);

/*
 * B, which holds the bootloader in its update state, then INFO, checked
 * against the stock bootloader's values.
 */
int defy_radio_factory_begin(struct defy_radio_factory *factory,
                             struct defy_radio_factory_info *info);

int defy_radio_factory_seal(struct defy_radio_factory *factory, const uint8_t *seal);
int defy_radio_factory_erase(struct defy_radio_factory *factory, uint32_t address);
int defy_radio_factory_write(struct defy_radio_factory *factory, uint32_t address,
                             const uint8_t *data, size_t length);
int defy_radio_factory_read(struct defy_radio_factory *factory, uint32_t address, uint8_t *data,
                            size_t length);
int defy_radio_factory_validate(struct defy_radio_factory *factory);

/* F after a validated image and an erased seal page: stores the seal and starts the image. */
int defy_radio_factory_finish(struct defy_radio_factory *factory);

/*
 * F with no seal sent in this session: the bootloader stores nothing and
 * starts the application, as the stock keyscanner leaves it on every boot.
 */
int defy_radio_factory_leave(struct defy_radio_factory *factory);

uint32_t defy_radio_crc32(const uint8_t *data, size_t length);

/*
 * After begin(): leaves the bootloader if the radio already holds this image
 * and seal, and otherwise installs it, vectors last, and finishes. *installed
 * says which. Refuses to write unless INFO shows the stock S113 in place, as
 * an application start of 0x1C000.
 */
int defy_radio_factory_install(struct defy_radio_factory *factory, const uint8_t *image,
                               size_t size, const uint8_t *seal, bool *installed);

#endif
