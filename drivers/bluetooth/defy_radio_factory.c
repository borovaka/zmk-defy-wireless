/*
 * Dygma's rf_bootloader protocol, from the RP2040's side.
 *
 * Every transfer starts with a status poll and a hello. A request goes out as
 * a header and payload under one chip select, and a reply is read back after
 * the bootloader reports it ready. Each operation runs on one deadline; any
 * failure ends the session.
 *
 * SPDX-License-Identifier: MIT
 */

#include <errno.h>
#include <string.h>

#include <dygma/defy_radio_factory.h>

/* A transfer's kind, the second byte of its header. */
#define LINK_HELLO 1
#define LINK_RECEIVE 2
#define LINK_DATA 3

/* What INFO reports on the stock bootloader. */
#define STOCK_DEVICE_ID 0x57474652u /* "RFGW" */
#define STOCK_BOOTLOADER_VERSION 1u
#define STOCK_HARDWARE_VERSION 1u
#define STOCK_FLASH_START 0x1000u
#define STOCK_FLASH_END 0x39000u
#define STOCK_WRITE_ALIGNMENT 4u
#define STOCK_MAX_TRANSFER 128u

static void put32(uint8_t *data, uint32_t value) {
    for (unsigned int i = 0; i < 4; i++) {
        data[i] = value >> (8u * i);
    }
}

uint32_t defy_radio_crc32(const uint8_t *data, size_t length) {
    uint32_t crc = UINT32_MAX;

    for (size_t i = 0; i < length; i++) {
        crc ^= data[i];
        for (unsigned int bit = 0; bit < 8; bit++) {
            crc = (crc >> 1) ^ (0xedb88320u & (0u - (crc & 1u)));
        }
    }
    return crc ^ UINT32_MAX;
}

static int fail(struct defy_radio_factory *factory, int error) {
    factory->failed = true;
    factory->sealed = false;
    factory->validated = false;
    factory->metadata_erased = false;
    return error < 0 ? error : -EIO;
}

static bool expired(struct defy_radio_factory *factory, uint64_t started) {
    return factory->bus.now_ms(factory->bus.context) - started >= factory->timeout_ms;
}

/* Raises chip select, then keeps a 1 ms margin, this driver's choice, before the next transfer. */
static int end_transfer(struct defy_radio_factory *factory) {
    int ended = factory->bus.release(factory->bus.context);

    if (ended == 0) {
        factory->bus.wait_ms(factory->bus.context, 1);
    }
    return ended;
}

static int release(struct defy_radio_factory *factory, int result, uint64_t started) {
    int ended = end_transfer(factory);

    if (result != 0 || ended != 0) {
        if (result == 0) {
            factory->trace.phase = DEFY_RADIO_PHASE_RELEASE;
        }
        return fail(factory, result != 0 ? result : ended);
    }
    return expired(factory, started) ? fail(factory, -ETIMEDOUT) : 0;
}

/*
 * An unacknowledged request takes effect when chip select rises, so a deadline
 * that expires during the release margin cannot undo it. A failure once its
 * bytes were being clocked leaves delivery unknown.
 */
static int deliver(struct defy_radio_factory *factory, int result) {
    int ended = end_transfer(factory);

    if (result != 0 || ended != 0) {
        fail(factory, -EIO);
        return DEFY_RADIO_FINISH_UNCONFIRMED;
    }
    return 0;
}

static int exchange(struct defy_radio_factory *factory, const uint8_t *tx, uint8_t *rx,
                    size_t length) {
    factory->trace.length = length;
    factory->trace.exchanges++;
    memset(factory->trace.tx, 0, sizeof(factory->trace.tx));
    memcpy(factory->trace.tx, tx, length < 2 ? length : 2);
    memset(rx, 0, length);
    int result = factory->bus.exchange(factory->bus.context, tx, rx, length);

    memset(factory->trace.rx, 0, sizeof(factory->trace.rx));
    memcpy(factory->trace.rx, rx, length < 2 ? length : 2);

    return result == 0 ? 0 : fail(factory, result);
}

/* Polls the bootloader's state, waiting out DEFY_RADIO_STATE_BUSY. */
static int status(struct defy_radio_factory *factory, uint8_t *value, uint64_t started,
                  enum defy_radio_factory_phase phase) {
    const uint8_t tx[2] = {0, 0};
    uint8_t rx[2];

    while (!expired(factory, started)) {
        factory->trace.phase = phase;
        int result = release(factory, exchange(factory, tx, rx, sizeof(tx)), started);

        if (result != 0) {
            return result;
        }
        if (defy_radio_factory_is_status(rx)) {
            if (rx[1] == DEFY_RADIO_STATE_BUSY) {
                factory->bus.wait_ms(factory->bus.context, 1);
                continue;
            }
            *value = rx[1];
            return 0;
        }
        if (rx[1] != 0 && rx[1] != 0xff) {
            return fail(factory, -EPROTO);
        }
        factory->bus.wait_ms(factory->bus.context, 1);
    }
    return fail(factory, -ETIMEDOUT);
}

/*
 * Waits for a state that accepts a hello, sends it, and waits for the hello
 * to succeed. Before the hello, DEFY_RADIO_STATE_FAILED is the result of an
 * earlier transfer the radio could not take, such as one cut short while it
 * started; Dygma's link listens again after it, so the next poll has the
 * current state.
 */
static int hello(struct defy_radio_factory *factory, uint64_t started) {
    uint8_t state;
    int result;

    do {
        result = status(factory, &state, started, DEFY_RADIO_PHASE_STATUS);
        if (result != 0) {
            return result;
        }
        if (state == DEFY_RADIO_STATE_SUCCEEDED || state == DEFY_RADIO_STATE_READY ||
            state == DEFY_RADIO_STATE_REPLY_WAITING) {
            break;
        }
        factory->bus.wait_ms(factory->bus.context, 1);
    } while (!expired(factory, started));
    if (expired(factory, started)) {
        return fail(factory, -ETIMEDOUT);
    }

    const uint8_t tx[2] = {DEFY_RADIO_LINK_HEADER_SIZE, LINK_HELLO};
    uint8_t rx[2];

    factory->trace.phase = DEFY_RADIO_PHASE_HELLO;
    result = release(factory, exchange(factory, tx, rx, sizeof(tx)), started);
    if (result != 0) {
        return result;
    }
    do {
        result = status(factory, &state, started, DEFY_RADIO_PHASE_HELLO_STATUS);
        if (result != 0) {
            return result;
        }
        if (state == DEFY_RADIO_STATE_SUCCEEDED) {
            return 0;
        }
        if (state == DEFY_RADIO_STATE_FAILED) {
            return fail(factory, -EPROTO);
        }
        factory->bus.wait_ms(factory->bus.context, 1);
    } while (!expired(factory, started));
    return fail(factory, -ETIMEDOUT);
}

static int send(struct defy_radio_factory *factory, const uint8_t *data, size_t length,
                uint64_t started, bool acknowledge) {
    if (length == 0 || length > DEFY_RADIO_MAX_TRANSFER) {
        return -EINVAL;
    }

    int result = hello(factory, started);

    if (result != 0) {
        return result;
    }

    uint8_t tx[2] = {(uint8_t)(length + DEFY_RADIO_LINK_HEADER_SIZE), LINK_DATA};
    uint8_t header[2];
    uint8_t ignored[DEFY_RADIO_MAX_TRANSFER];

    factory->trace.phase = DEFY_RADIO_PHASE_SEND_HEADER;
    result = exchange(factory, tx, header, sizeof(tx));
    if (result == 0 &&
        (header[0] != DEFY_RADIO_LINK_HEADER_SIZE || header[1] != DEFY_RADIO_STATE_SUCCEEDED)) {
        result = -EPROTO;
    }
    if (result == 0) {
        factory->trace.phase = DEFY_RADIO_PHASE_SEND_PAYLOAD;
        result = exchange(factory, data, ignored, length);
        if (!acknowledge) {
            return deliver(factory, result);
        }
    }
    result = release(factory, result, started);
    if (result != 0) {
        return result;
    }

    uint8_t state;

    result = status(factory, &state, started, DEFY_RADIO_PHASE_SEND_STATUS);
    if (result != 0) {
        return result;
    }
    return state == DEFY_RADIO_STATE_SUCCEEDED || state == DEFY_RADIO_STATE_SUCCEEDED_BUSY
               ? 0
               : fail(factory, -EPROTO);
}

static int receive(struct defy_radio_factory *factory, uint8_t *data, size_t length,
                   uint64_t started) {
    const uint8_t request[2] = {DEFY_RADIO_LINK_HEADER_SIZE, LINK_RECEIVE};
    const uint8_t zero[DEFY_RADIO_MAX_TRANSFER] = {0};
    uint8_t header[2];

    while (!expired(factory, started)) {
        factory->trace.phase = DEFY_RADIO_PHASE_RECEIVE_REQUEST;
        int result = release(factory, exchange(factory, request, header, sizeof(request)), started);

        if (result != 0) {
            return result;
        }
        for (;;) {
            if (expired(factory, started)) {
                return fail(factory, -ETIMEDOUT);
            }
            factory->trace.phase = DEFY_RADIO_PHASE_RECEIVE_HEADER;
            result = exchange(factory, zero, header, sizeof(header));
            if (result != 0) {
                return release(factory, result, started);
            }
            if (header[1] == LINK_DATA) {
                size_t count = header[0] >= DEFY_RADIO_LINK_HEADER_SIZE
                                   ? header[0] - DEFY_RADIO_LINK_HEADER_SIZE
                                   : DEFY_RADIO_MAX_TRANSFER + 1u;

                if (count != 0 && count != length) {
                    return release(factory, -EPROTO, started);
                }
                if (count != 0) {
                    factory->trace.phase = DEFY_RADIO_PHASE_RECEIVE_PAYLOAD;
                    result = exchange(factory, zero, data, count);
                }
                result = release(factory, result, started);
                if (result != 0 || count != 0) {
                    return result;
                }
                break;
            }
            result = release(factory, 0, started);
            if (result != 0) {
                return result;
            }
            if (header[0] == DEFY_RADIO_LINK_HEADER_SIZE &&
                header[1] == DEFY_RADIO_STATE_REPLY_WAITING) {
                break;
            }
            if (header[0] == DEFY_RADIO_LINK_HEADER_SIZE && header[1] == DEFY_RADIO_STATE_BUSY) {
                uint8_t state;

                result = status(factory, &state, started, DEFY_RADIO_PHASE_RECEIVE_HEADER);
                if (result != 0) {
                    return result;
                }
                if (state == DEFY_RADIO_STATE_FAILED) {
                    return fail(factory, -EPROTO);
                }
                break;
            }
            if (header[1] == DEFY_RADIO_STATE_FAILED ||
                (header[1] != 0 && header[1] != 0xff && !defy_radio_factory_is_status(header))) {
                return fail(factory, -EPROTO);
            }
            factory->bus.wait_ms(factory->bus.context, 1);
        }
        factory->bus.wait_ms(factory->bus.context, 1);
    }
    return fail(factory, -ETIMEDOUT);
}

static int ready(struct defy_radio_factory *factory) {
    if (factory == NULL || factory->failed || !factory->begun) {
        return -ENOTCONN;
    }
    return 0;
}

static int operation(struct defy_radio_factory *factory, uint8_t command,
                     const uint8_t *parameters, size_t parameter_length, const uint8_t *data,
                     size_t data_length, uint8_t *response, size_t response_length) {
    int result = ready(factory);

    if (result != 0) {
        return result;
    }

    uint64_t started = factory->bus.now_ms(factory->bus.context);

    factory->trace.command = command;
    result = send(factory, &command, 1, started, true);
    if (result == 0 && parameter_length != 0) {
        result = send(factory, parameters, parameter_length, started, true);
    }
    if (result == 0 && data_length != 0) {
        result = send(factory, data, data_length, started, true);
    }
    if (result == 0 && response_length != 0) {
        result = receive(factory, response, response_length, started);
    }
    return result;
}

static bool in_slot(uint32_t address, size_t length) {
    return address >= DEFY_RADIO_SLOT_START && address < DEFY_RADIO_SLOT_END && length != 0 &&
           length <= DEFY_RADIO_SLOT_END - address;
}

static bool valid_seal(const uint8_t *seal) {
    if (seal == NULL ||
        defy_radio_get32(seal + DEFY_RADIO_SEAL_KIND_OFFSET) != DEFY_RADIO_SEAL_KIND ||
        defy_radio_get32(seal + DEFY_RADIO_SEAL_SIZE_OFFSET) != DEFY_RADIO_SEAL_SIZE ||
        defy_radio_get32(seal + DEFY_RADIO_SEAL_START_OFFSET) != DEFY_RADIO_SLOT_START ||
        defy_radio_get32(seal + DEFY_RADIO_SEAL_LENGTH_OFFSET) < DEFY_RADIO_IMAGE_MIN ||
        defy_radio_get32(seal + DEFY_RADIO_SEAL_LENGTH_OFFSET) > DEFY_RADIO_IMAGE_MAX ||
        (defy_radio_get32(seal + DEFY_RADIO_SEAL_LENGTH_OFFSET) & 3u) != 0) {
        return false;
    }

    uint8_t copy[DEFY_RADIO_SEAL_SIZE];

    memcpy(copy, seal, sizeof(copy));
    memset(copy + DEFY_RADIO_SEAL_CRC_OFFSET, 0, 4);
    return defy_radio_crc32(copy, sizeof(copy)) ==
           defy_radio_get32(seal + DEFY_RADIO_SEAL_CRC_OFFSET);
}

int defy_radio_factory_seal(struct defy_radio_factory *factory, const uint8_t *seal) {
    int result = ready(factory);

    if (result != 0) {
        return result;
    }
    factory->sealed = false;
    factory->validated = false;
    if (!valid_seal(seal)) {
        return -EINVAL;
    }

    uint8_t response;

    factory->seal_sent = true;
    result = operation(factory, 'S', seal, DEFY_RADIO_SEAL_SIZE, NULL, 0, &response, 1);
    if (result != 0 || response != 1) {
        return result != 0 ? result : fail(factory, -EIO);
    }
    factory->sealed = true;
    return 0;
}

/* One page. The bootloader also erases its seal page on the first erase or write of a session. */
int defy_radio_factory_erase(struct defy_radio_factory *factory, uint32_t address) {
    if (!in_slot(address, DEFY_RADIO_PAGE_SIZE) || address % DEFY_RADIO_PAGE_SIZE != 0) {
        return -EINVAL;
    }

    int result = ready(factory);

    if (result != 0 || !factory->sealed) {
        return result != 0 ? result : -EPERM;
    }

    uint8_t parameters[8], response;

    put32(parameters, address);
    put32(parameters + 4, DEFY_RADIO_PAGE_SIZE);
    factory->validated = false;
    result = operation(factory, 'E', parameters, sizeof(parameters), NULL, 0, &response, 1);
    if (result != 0 || response != 1) {
        return result != 0 ? result : fail(factory, -EIO);
    }
    factory->metadata_erased = true;
    return 0;
}

/*
 * The bootloader programs one word past the data with that word's current
 * value. It must stay inside the slot, and the caller must have erased the
 * page it falls in.
 */
int defy_radio_factory_write(struct defy_radio_factory *factory, uint32_t address,
                             const uint8_t *data, size_t length) {
    if (data == NULL || length == 0 || length > DEFY_RADIO_MAX_TRANSFER || (address & 3u) != 0 ||
        (length & 3u) != 0 || !in_slot(address, length + 4u) ||
        address / DEFY_RADIO_PAGE_SIZE != (address + length - 1u) / DEFY_RADIO_PAGE_SIZE) {
        return -EINVAL;
    }

    int result = ready(factory);

    if (result != 0 || !factory->sealed) {
        return result != 0 ? result : -EPERM;
    }

    uint8_t parameters[8], response[4];

    put32(parameters, address);
    put32(parameters + 4, length);
    factory->validated = false;
    result = operation(factory, 'W', parameters, sizeof(parameters), data, length, response,
                       sizeof(response));
    if (result != 0) {
        return result;
    }
    if (defy_radio_get32(response) != defy_radio_crc32(data, length)) {
        return fail(factory, -EILSEQ);
    }
    return 0;
}

int defy_radio_factory_read(struct defy_radio_factory *factory, uint32_t address, uint8_t *data,
                            size_t length) {
    bool seal = address >= DEFY_RADIO_SEAL_ADDRESS &&
                address < DEFY_RADIO_SEAL_ADDRESS + DEFY_RADIO_SEAL_SIZE && length != 0 &&
                length <= DEFY_RADIO_SEAL_ADDRESS + DEFY_RADIO_SEAL_SIZE - address;

    if (data == NULL || (!in_slot(address, length) && !seal) || length > DEFY_RADIO_MAX_TRANSFER) {
        return -EINVAL;
    }

    uint8_t parameters[8];

    put32(parameters, address);
    put32(parameters + 4, length);
    return operation(factory, 'R', parameters, sizeof(parameters), NULL, 0, data, length);
}

int defy_radio_factory_validate(struct defy_radio_factory *factory) {
    int result = ready(factory);

    if (result != 0 || !factory->sealed) {
        return result != 0 ? result : -EPERM;
    }

    uint8_t response;

    factory->validated = false;
    result = operation(factory, 'V', NULL, 0, NULL, 0, &response, 1);
    if (result == 0 && response != 1) {
        return fail(factory, -EILSEQ);
    }
    factory->validated = result == 0;
    return result;
}

static int send_finish(struct defy_radio_factory *factory) {
    uint8_t command = 'F';
    factory->trace.command = command;
    int result = send(factory, &command, 1, factory->bus.now_ms(factory->bus.context), false);

    factory->finish_transmitted = result == 0;
    factory->begun = false;
    factory->sealed = false;
    factory->validated = false;
    factory->metadata_erased = false;
    factory->failed = true;
    return result;
}

int defy_radio_factory_finish(struct defy_radio_factory *factory) {
    int result = ready(factory);

    if (result != 0) {
        return result;
    }
    if (!factory->validated || !factory->metadata_erased) {
        return -EPERM;
    }
    return send_finish(factory);
}

int defy_radio_factory_leave(struct defy_radio_factory *factory) {
    int result = ready(factory);

    if (result != 0) {
        return result;
    }
    /* A seal staged in this session would be validated and stored. */
    if (factory->seal_sent) {
        return -EPERM;
    }
    return send_finish(factory);
}

int defy_radio_factory_init(struct defy_radio_factory *factory,
                            const struct defy_radio_factory_bus *bus, uint32_t timeout_ms) {
    if (factory == NULL || bus == NULL || bus->exchange == NULL || bus->release == NULL ||
        bus->now_ms == NULL || bus->wait_ms == NULL ||
        timeout_ms < DEFY_RADIO_FACTORY_MIN_TIMEOUT_MS ||
        timeout_ms > DEFY_RADIO_FACTORY_MAX_TIMEOUT_MS) {
        return -EINVAL;
    }

    struct defy_radio_factory_bus copied_bus = *bus;

    memset(factory, 0, sizeof(*factory));
    factory->bus = copied_bus;
    factory->timeout_ms = timeout_ms;
    return 0;
}

int defy_radio_factory_begin(struct defy_radio_factory *factory,
                             struct defy_radio_factory_info *info) {
    if (factory == NULL || factory->bus.exchange == NULL || factory->failed || factory->begun) {
        return -EINVAL;
    }

    uint64_t started = factory->bus.now_ms(factory->bus.context);
    uint8_t command = 'B';
    factory->trace.command = command;
    int result = send(factory, &command, 1, started, true);

    if (result != 0) {
        return result;
    }
    factory->begun = true;

    uint8_t response[sizeof(struct defy_radio_factory_info)];

    result = operation(factory, 'I', NULL, 0, NULL, 0, response, sizeof(response));
    if (result != 0) {
        return result;
    }

    struct defy_radio_factory_info value = {
        .device_id = defy_radio_get32(response),
        .bootloader_version = defy_radio_get32(response + 4),
        .hardware_version = defy_radio_get32(response + 8),
        .program_version = defy_radio_get32(response + 12),
        .flash_start = defy_radio_get32(response + 16),
        .program_start = defy_radio_get32(response + 20),
        .validation_start = defy_radio_get32(response + 24),
        .flash_available = defy_radio_get32(response + 28),
        .erase_alignment = defy_radio_get32(response + 32),
        .write_alignment = defy_radio_get32(response + 36),
        .max_transfer = defy_radio_get32(response + 40),
    };

    factory->trace.phase = DEFY_RADIO_PHASE_INFO;
    factory->info = value;

    /*
     * The application start is derived from S113's identification at 0x3000
     * and is only informational here; install() decides what it means.
     */
    if (value.device_id != STOCK_DEVICE_ID ||
        value.bootloader_version != STOCK_BOOTLOADER_VERSION ||
        value.hardware_version != STOCK_HARDWARE_VERSION ||
        value.flash_start != STOCK_FLASH_START ||
        value.flash_available != STOCK_FLASH_END - STOCK_FLASH_START ||
        value.validation_start != DEFY_RADIO_SEAL_ADDRESS ||
        value.erase_alignment != DEFY_RADIO_PAGE_SIZE ||
        value.write_alignment != STOCK_WRITE_ALIGNMENT ||
        value.max_transfer != STOCK_MAX_TRANSFER) {
        return fail(factory, -ENOTSUP);
    }
    if (info != NULL) {
        *info = value;
    }
    return 0;
}
