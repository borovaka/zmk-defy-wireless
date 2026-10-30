/*
 * The rf_bootloader client and radio installer against a fake bootloader. The
 * fake behaves as Dygma's bootloader does: status polls and hellos,
 * B/I/S/E/W/R/V/F, the seal page erased on the session's first erase or
 * write, one word programmed past every write, F storing a staged seal only
 * if the image validates and leaving without writing otherwise. It counts
 * programs per flash word against the nRF52820's limit of two between erases.
 *
 * SPDX-License-Identifier: MIT
 */

#include <assert.h>
#include <errno.h>
#include <string.h>

#include <dygma/defy_radio_factory.h>

static uint32_t get32(const uint8_t *data) {
    return (uint32_t)data[0] | (uint32_t)data[1] << 8 | (uint32_t)data[2] << 16 |
           (uint32_t)data[3] << 24;
}

#define FLASH_SIZE 0x40000u
#define S113_MAGIC 0x51b1e5dbu
#define STOCK_VERSION 0x10201u

enum mode { BOOTLOADER, APPLICATION, STOCK_APPLICATION };

struct fake {
    uint8_t flash[FLASH_SIZE];
    uint8_t programs[FLASH_SIZE / 4];
    uint8_t tx[66], out[66], response[64], staged[28];
    size_t transferred, out_length, response_length;
    enum mode mode;
    bool selected, writable, blocked, ack83, corrupt_crc, reject_seal, drop_write_ack;
    bool seal_staged, metadata_erased, seal_written;
    unsigned int exchanges, releases, packets, writes, erases, finishes, seals;
    unsigned int fail_exchange, fail_release, read_contentions, status_contentions, erase_delay;
    unsigned int waits, selected_waits, release_waits, exchange_time, delayed_release, release_delay;
    unsigned int hang_at_packet, max_programs, invalid_info_field;
    bool release_wait_pending, has_released, hung, tear, tearing;
    uint64_t time, response_ready, last_release;
    uint8_t parser, status;
    uint32_t address, length, last_write;
    unsigned int progress_reports;
    size_t progress_written, progress_size;
};

static struct fake fake;
static struct defy_radio_factory factory;

static void put32(uint8_t *data, uint32_t value) {
    for (unsigned int i = 0; i < 4; i++) {
        data[i] = value >> (8u * i);
    }
}

static void queued_status(struct fake *f, uint8_t value) {
    f->out[0] = 2;
    f->out[1] = value;
    f->out_length = 2;
    f->status = value;
}

static void answer(struct fake *f, const uint8_t *data, size_t length) {
    assert(length <= sizeof(f->response) && f->response_length == 0);
    memcpy(f->response, data, length);
    f->response_length = length;
}

/* Power lost part-way through, the erase reaches only the first half of the page. */
static void erase_page(struct fake *f, uint32_t address) {
    assert(address % 4096 == 0 && address + 4096 <= FLASH_SIZE);
    size_t length = f->tearing ? 2048 : 4096;
    memset(f->flash + address, 0xff, length);
    memset(f->programs + address / 4, 0, length / 4);
}

/* One NVMC word program: bits only clear, and at most twice between erases. */
static void program_word(struct fake *f, uint32_t address, const uint8_t *value) {
    assert(address % 4 == 0 && address + 4 <= FLASH_SIZE);
    for (unsigned int i = 0; i < 4; i++) {
        assert((f->flash[address + i] & value[i]) == value[i]);
        f->flash[address + i] = value[i];
    }
    uint8_t count = ++f->programs[address / 4];
    assert(count <= 2);
    if (count > f->max_programs) {
        f->max_programs = count;
    }
}

/*
 * rf_bootloader's write: the data, then the next word with its current value.
 * Power lost part-way through, only the first half of the words are written.
 */
static void program(struct fake *f, uint32_t address, const uint8_t *data, size_t length) {
    for (size_t offset = 0; offset < length; offset += 4) {
        if (f->tearing && offset >= length / 2) {
            return;
        }
        program_word(f, address + offset, data + offset);
    }
    uint8_t current[4];
    memcpy(current, f->flash + address + length, 4);
    program_word(f, address + length, current);
}

static void erase_metadata_once(struct fake *f) {
    if (!f->metadata_erased) {
        erase_page(f, 0x3d000);
        f->metadata_erased = true;
    }
}

static bool seal_valid(const uint8_t *seal) {
    uint8_t copy[28];
    memcpy(copy, seal, sizeof(copy));
    memset(copy + 8, 0, 4);
    return get32(seal) == 1 && get32(seal + 4) == 28 &&
           defy_radio_crc32(copy, sizeof(copy)) == get32(seal + 8);
}

static bool program_valid(const struct fake *f, const uint8_t *seal) {
    uint32_t start = get32(seal + 12), length = get32(seal + 16);
    return seal_valid(seal) && start >= 0x1000 && length <= 0x39000 - start &&
           defy_radio_crc32(f->flash + start, length) == get32(seal + 20);
}

static void boot(struct fake *f, enum mode mode) {
    f->mode = mode;
    f->seal_staged = f->metadata_erased = f->seal_written = false;
    f->writable = f->blocked = f->selected = false;
    f->response_length = 0;
    f->parser = 0;
    queued_status(f, mode == APPLICATION ? 0 : 0x82);
}

/* Hands off to the application, as state 4. */
static void hand_off(struct fake *f) {
    f->mode = APPLICATION;
    f->writable = false;
}

static void handle(struct fake *f, const uint8_t *data, size_t length);

/* A packet at hang_at_packet is the last one answered, and with tear set, power is lost during it. */
static void application(struct fake *f, const uint8_t *data, size_t length) {
    f->packets++;
    if (f->hang_at_packet != 0 && f->packets == f->hang_at_packet) {
        f->hung = true;
        f->tearing = f->tear;
    }
    handle(f, data, length);
    f->tearing = false;
}

static void handle(struct fake *f, const uint8_t *data, size_t length) {
    uint8_t response[44] = {0};
    /* Dygma's radio application takes packets in the bootloader's framing but answers none. */
    if (f->mode == STOCK_APPLICATION) {
        return;
    }
    if (f->parser == 0) {
        assert(length == 1);
        switch (data[0]) {
        case 'B':
            break;
        case 'I': {
            uint32_t size = get32(f->flash + 0x3008);
            uint32_t start = get32(f->flash + 0x3004) == S113_MAGIC ? ((size - 1u) & ~4095u) + 4096u : 0x1000u;
            uint32_t version = seal_valid(f->flash + 0x3d000) ? get32(f->flash + 0x3d000 + 24) : UINT32_MAX;
            uint32_t values[] = {0x57474652, 1, 1, version, 0x1000, start, 0x3d000, 0x38000, 4096, 4, 128};
            if (f->invalid_info_field != 0) {
                values[f->invalid_info_field - 1] ^= 1;
            }
            for (size_t i = 0; i < 11; i++) {
                put32(response + i * 4, values[i]);
            }
            answer(f, response, sizeof(response));
            break;
        }
        case 'S':
        case 'E':
        case 'W':
        case 'R':
            f->parser = data[0];
            break;
        case 'V':
            response[0] = f->seal_staged && program_valid(f, f->staged);
            answer(f, response, 1);
            break;
        case 'F':
            f->finishes++;
            if (!f->seal_written && f->seal_staged && program_valid(f, f->staged)) {
                f->seal_written = true;
                f->seals++;
                program(f, 0x3d000, f->staged, 28);
            }
            hand_off(f);
            break;
        default:
            assert(false);
        }
        return;
    }
    if (f->parser == 'S') {
        assert(length == 28);
        f->seal_staged = !f->reject_seal && seal_valid(data);
        if (f->seal_staged) {
            memcpy(f->staged, data, 28);
        }
        response[0] = f->seal_staged;
        answer(f, response, 1);
        f->parser = 0;
    } else if (f->parser == 'E' || f->parser == 'R' || f->parser == 'W') {
        assert(length == 8);
        f->address = get32(data);
        f->length = get32(data + 4);
        if (f->parser == 'E') {
            assert(f->address >= 0x1000 && f->address % 4096 == 0 && f->length == 4096 &&
                   f->address + f->length <= 0x39000);
            if (f->seal_staged) {
                erase_metadata_once(f);
                erase_page(f, f->address);
                f->erases++;
                f->response_ready = f->time + f->erase_delay;
            }
            response[0] = f->seal_staged;
            answer(f, response, 1);
            f->parser = 0;
        } else if (f->parser == 'R') {
            assert(f->length <= 64 && f->address + f->length <= FLASH_SIZE);
            answer(f, f->flash + f->address, f->length);
            f->parser = 0;
        } else {
            assert(f->length <= 64 && f->address >= 0x1000 && f->address + f->length + 4 <= 0x39000);
            f->parser = 0xff;
        }
    } else if (f->parser == 0xff) {
        assert(length == f->length);
        if (f->seal_staged) {
            erase_metadata_once(f);
            program(f, f->address, data, length);
            put32(response, defy_radio_crc32(f->flash + f->address, length) ^ (f->corrupt_crc ? 1u : 0u));
            f->writes++;
            f->last_write = f->address;
        }
        answer(f, response, 4);
        f->parser = 0;
        if (f->drop_write_ack) {
            f->hung = true;
        }
    }
}

static bool silent(const struct fake *f) { return f->hung || f->mode == APPLICATION; }

static int bus_exchange(void *context, const uint8_t *tx, uint8_t *rx, size_t length) {
    struct fake *f = context;
    if (f->selected) {
        assert(f->waits == f->selected_waits);
    } else {
        assert(!f->release_wait_pending);
        assert(!f->has_released || f->time - f->last_release >= 1);
        f->selected_waits = f->waits;
    }
    f->exchanges++;
    f->time += f->exchange_time;
    if (f->exchanges == f->fail_exchange) {
        return -EIO;
    }
    if (!f->selected) {
        f->selected = true;
        f->transferred = 0;
    }
    assert(length != 0 && f->transferred + length <= sizeof(f->tx));
    memcpy(f->tx + f->transferred, tx, length);
    for (size_t i = 0; i < length; i++) {
        size_t offset = f->transferred + i;
        rx[i] = silent(f) ? 0xff : offset < f->out_length ? f->out[offset] : 0xff;
    }
    f->transferred += length;
    return 0;
}

static int bus_release(void *context) {
    struct fake *f = context;
    f->releases++;
    if (f->releases == f->delayed_release) {
        f->time += f->release_delay;
    }
    f->last_release = f->time;
    f->has_released = true;
    f->release_wait_pending = true;
    if (!f->selected) {
        return 0;
    }
    f->selected = false;
    if (silent(f)) {
        return 0;
    }
    if (f->blocked) {
        queued_status(f, 0x84);
        return 0;
    }
    if (f->out[1] == 3) {
        if (f->out_length > 2) {
            f->response_length = 0;
        }
        queued_status(f, 0x82);
    } else if (f->tx[1] == 1) {
        assert(f->transferred == 2 && f->tx[0] == 2);
        f->writable = true;
        queued_status(f, 0x80);
    } else if (f->tx[1] == 3) {
        assert(f->writable && f->status == 0x80);
        if (f->tx[0] != f->transferred) {
            /* A header without its full payload. */
            queued_status(f, 0x81);
            f->writable = false;
            return 0;
        }
        assert(f->transferred > 2);
        bool blocked = f->ack83 && f->parser == 0xff;
        application(f, f->tx + 2, f->transferred - 2);
        f->writable = false;
        f->blocked = blocked;
        if (!silent(f)) {
            queued_status(f, blocked ? 0x83 : 0x80);
        }
    } else if (f->tx[1] == 2) {
        assert(f->transferred == 2 && f->tx[0] == 2);
        if (f->read_contentions != 0) {
            f->read_contentions--;
            queued_status(f, 0x84);
            return 0;
        }
        size_t length = f->time < f->response_ready ? 0 : f->response_length;
        f->out[0] = (uint8_t)(length + 2);
        f->out[1] = 3;
        memcpy(f->out + 2, f->response, length);
        f->out_length = length + 2;
    } else {
        assert(f->transferred == 2 && f->tx[0] == 0 && f->tx[1] == 0);
        if (f->status_contentions != 0) {
            f->status_contentions--;
            queued_status(f, 0x84);
        } else {
            queued_status(f, f->writable ? 0x80 : f->response_length ? 0x85 : 0x82);
        }
    }
    return f->releases == f->fail_release ? -EIO : 0;
}

static uint64_t bus_time(void *context) { return ((struct fake *)context)->time; }

static void bus_wait(void *context, uint32_t milliseconds) {
    struct fake *f = context;
    assert(!f->selected);
    f->waits++;
    if (f->release_wait_pending) {
        assert(milliseconds == 1);
        f->release_wait_pending = false;
        f->release_waits++;
    }
    f->time += milliseconds;
}

/* Each install reports 0 after its erases, then grows to its size, between operations. */
static void bus_progress(void *context, size_t written, size_t size) {
    struct fake *f = context;
    assert(!f->selected && written <= size);
    assert(written == 0 || (written > f->progress_written && size == f->progress_size));
    f->progress_reports++;
    f->progress_written = written;
    f->progress_size = size;
}

static const struct defy_radio_factory_bus bus = {&fake,    bus_exchange, bus_release,
                                                  bus_time, bus_wait,     bus_progress};

/* A stock radio: S113's identification, Dygma's application and its seal. */
static void stock_flash(void) {
    memset(fake.flash, 0xff, sizeof(fake.flash));
    memset(fake.programs, 1, sizeof(fake.programs));
    for (uint32_t address = 0; address < 0x2b834; address++) {
        fake.flash[address] = (uint8_t)(address * 7u + (address >> 9));
    }
    put32(fake.flash + 0x3004, S113_MAGIC);
    put32(fake.flash + 0x3008, 0x1c000);
    for (uint32_t address = 0x39000; address < 0x3d000; address++) {
        fake.flash[address] = (uint8_t)(address * 3u);
    }
    uint8_t seal[28];
    put32(seal, 1);
    put32(seal + 4, 28);
    put32(seal + 8, 0);
    put32(seal + 12, 0x1c000);
    put32(seal + 16, 0xf834);
    put32(seal + 20, defy_radio_crc32(fake.flash + 0x1c000, 0xf834));
    put32(seal + 24, STOCK_VERSION);
    put32(seal + 8, defy_radio_crc32(seal, 28));
    memcpy(fake.flash + 0x3d000, seal, 28);
}

static void reset(void) {
    memset(&fake, 0, sizeof(fake));
    stock_flash();
    fake.exchange_time = 1;
    boot(&fake, BOOTLOADER);
    assert(defy_radio_factory_init(&factory, &bus, 500) == 0);
}

static void new_session(void) {
    fake.hung = false;
    fake.hang_at_packet = 0;
    fake.fail_exchange = fake.fail_release = 0;
    fake.has_released = false;
    fake.release_wait_pending = false;
    assert(defy_radio_factory_init(&factory, &bus, 500) == 0);
}

static void begin(void) {
    struct defy_radio_factory_info info;
    assert(defy_radio_factory_begin(&factory, &info) == 0);
    assert(info.device_id == 0x57474652 && info.validation_start == 0x3d000);
    assert(!fake.selected);
}

static void make_seal(uint8_t seal[28], uint32_t length, uint32_t crc, uint32_t version) {
    put32(seal, 1);
    put32(seal + 4, 28);
    put32(seal + 8, 0);
    put32(seal + 12, 0x1c000);
    put32(seal + 16, length);
    put32(seal + 20, crc);
    put32(seal + 24, version);
    put32(seal + 8, defy_radio_crc32(seal, 28));
}

static void accept_seal(void) {
    uint8_t seal[28];
    make_seal(seal, 0x1000, 123, 7);
    assert(defy_radio_factory_seal(&factory, seal) == 0);
    assert(factory.sealed && fake.seal_staged);
}

static void assert_release_margin(void) {
    assert(!fake.selected && !fake.release_wait_pending);
    assert(fake.release_waits == fake.releases);
    assert(fake.time - fake.last_release >= 1);
}

/* The protocol client. */

static void test_operations(void) {
    reset();
    begin();
    assert(factory.info.program_start == 0x1c000 && factory.info.program_version == STOCK_VERSION);
    uint8_t data[64], readback[64];
    for (size_t i = 0; i < sizeof(data); i++) {
        data[i] = i * 13u;
    }
    assert(defy_radio_factory_finish(&factory) == -EPERM);
    accept_seal();
    assert(defy_radio_factory_erase(&factory, 0x1d000) == 0);
    assert(fake.metadata_erased);
    assert(defy_radio_factory_write(&factory, 0x1d000, data, sizeof(data)) == 0);
    assert(fake.writes == 1);
    assert(defy_radio_factory_read(&factory, 0x1d000, readback, sizeof(readback)) == 0);
    assert(memcmp(data, readback, sizeof(data)) == 0);
    assert_release_margin();
}

static void test_mutations_stay_in_the_slot(void) {
    static const uint32_t protected[] = {0, 0x1000, 0x3000, 0x1b000, 0x39000, 0x3d000, 0x3f000};
    uint8_t data[64] = {0}, readback[4];
    reset();
    begin();
    accept_seal();
    unsigned int before = fake.exchanges;
    for (size_t i = 0; i < sizeof(protected) / sizeof(protected[0]); i++) {
        assert(defy_radio_factory_erase(&factory, protected[i]) == -EINVAL);
        assert(defy_radio_factory_write(&factory, protected[i], data, 4) == -EINVAL);
    }
    /* Unaligned, too long, across a page, or with the extra word past the slot. */
    assert(defy_radio_factory_erase(&factory, 0x1c001) == -EINVAL);
    assert(defy_radio_factory_write(&factory, 0x1c002, data, 4) == -EINVAL);
    assert(defy_radio_factory_write(&factory, 0x1c000, data, 3) == -EINVAL);
    assert(defy_radio_factory_write(&factory, 0x1c000, data, 0) == -EINVAL);
    assert(defy_radio_factory_write(&factory, 0x1c000, data, 68) == -EINVAL);
    assert(defy_radio_factory_write(&factory, 0x1cfe0, data, 64) == -EINVAL);
    assert(defy_radio_factory_write(&factory, 0x38ffc, data, 4) == -EINVAL);
    assert(defy_radio_factory_write(&factory, 0x38fc0, data, 64) == -EINVAL);
    assert(defy_radio_factory_read(&factory, 0x1bffc, readback, 4) == -EINVAL);
    assert(defy_radio_factory_read(&factory, 0x38ffe, readback, 4) == -EINVAL);
    assert(defy_radio_factory_read(&factory, 0x3d01b, readback, 2) == -EINVAL);
    assert(defy_radio_factory_read(&factory, 0x3d000, readback, 0) == -EINVAL);
    assert(fake.exchanges == before && fake.writes == 0 && fake.erases == 0);
    /* The last page's final chunk ends one word short of the bootloader. */
    assert(defy_radio_factory_erase(&factory, 0x38000) == 0);
    assert(defy_radio_factory_write(&factory, 0x38fbc, data, 64) == 0);
    assert(defy_radio_factory_read(&factory, 0x3d000, readback, 4) == 0);
}

static void test_seal_required_for_mutation(void) {
    uint8_t seal[28], data[4] = {1, 2, 3, 4};
    reset();
    begin();
    unsigned int before = fake.exchanges;
    assert(defy_radio_factory_erase(&factory, 0x1c000) == -EPERM);
    assert(defy_radio_factory_write(&factory, 0x1c000, data, 4) == -EPERM);
    assert(defy_radio_factory_validate(&factory) == -EPERM);
    make_seal(seal, 0x1000, 1, 2);
    seal[8] ^= 1;
    assert(defy_radio_factory_seal(&factory, seal) == -EINVAL);
    make_seal(seal, 0x1000, 1, 2);
    put32(seal + 12, 0x1000);
    put32(seal + 8, 0);
    put32(seal + 8, defy_radio_crc32(seal, 28));
    assert(defy_radio_factory_seal(&factory, seal) == -EINVAL);
    make_seal(seal, DEFY_RADIO_IMAGE_MAX + 4, 1, 2);
    assert(defy_radio_factory_seal(&factory, seal) == -EINVAL);
    assert(fake.exchanges == before && !factory.seal_sent);
    fake.reject_seal = true;
    make_seal(seal, 0x1000, 1, 2);
    assert(defy_radio_factory_seal(&factory, seal) == -EIO);
    assert(factory.failed && factory.seal_sent);
    before = fake.exchanges;
    assert(defy_radio_factory_erase(&factory, 0x1c000) == -ENOTCONN);
    assert(fake.exchanges == before);
}

static void test_leave_never_follows_a_seal(void) {
    reset();
    begin();
    uint8_t before[FLASH_SIZE / 4];
    memcpy(before, fake.programs, sizeof(before));
    assert(defy_radio_factory_leave(&factory) == 0);
    assert(fake.finishes == 1 && fake.mode == APPLICATION && fake.seals == 0);
    assert(memcmp(before, fake.programs, sizeof(before)) == 0);
    assert(defy_radio_factory_leave(&factory) == -ENOTCONN);

    reset();
    begin();
    accept_seal();
    unsigned int exchanges = fake.exchanges;
    assert(defy_radio_factory_leave(&factory) == -EPERM);
    assert(fake.exchanges == exchanges && fake.finishes == 0);
}

static void test_backpressure_is_not_replayed(void) {
    reset();
    begin();
    accept_seal();
    assert(defy_radio_factory_erase(&factory, 0x1c000) == 0);
    fake.ack83 = true;
    uint8_t data[4] = {1, 2, 3, 4};
    assert(defy_radio_factory_write(&factory, 0x1c000, data, sizeof(data)) == -ETIMEDOUT);
    assert(fake.writes == 1 && factory.failed);
    assert(defy_radio_factory_write(&factory, 0x1c000, data, sizeof(data)) == -ENOTCONN);
    assert(fake.writes == 1);
}

static void test_errors(void) {
    uint8_t data[4] = {0}, response[4];
    reset();
    assert(defy_radio_factory_read(&factory, 0x1c000, response, 4) == -ENOTCONN);
    assert(defy_radio_factory_finish(&factory) == -ENOTCONN);
    assert(defy_radio_factory_finish(NULL) == -ENOTCONN);
    begin();
    accept_seal();
    assert(defy_radio_factory_erase(&factory, 0x1c000) == 0);
    fake.corrupt_crc = true;
    assert(defy_radio_factory_write(&factory, 0x1c000, data, 4) == -EILSEQ);
    assert(factory.failed);

    reset();
    boot(&fake, APPLICATION);
    assert(defy_radio_factory_begin(&factory, NULL) == -ETIMEDOUT);
    assert(fake.time <= 502 && fake.packets == 0 && !fake.selected);

    reset();
    begin();
    accept_seal();
    assert(defy_radio_factory_erase(&factory, 0x1c000) == 0);
    fake.drop_write_ack = true;
    assert(defy_radio_factory_write(&factory, 0x1c000, data, 4) == -ETIMEDOUT);
    assert(fake.writes == 1 && factory.failed);

    reset();
    begin();
    accept_seal();
    fake.fail_exchange = fake.exchanges + 5;
    assert(defy_radio_factory_erase(&factory, 0x1c000) == -EIO);
    assert(!fake.selected && factory.failed);
}

static void test_wrong_bootloader_identity(void) {
    for (unsigned int field = 1; field <= 11; field++) {
        reset();
        fake.invalid_info_field = field;
        /* Version and application start are what the installer decides on. */
        if (field == 4 || field == 6) {
            begin();
            continue;
        }
        assert(defy_radio_factory_begin(&factory, NULL) == -ENOTSUP);
        assert(factory.failed && fake.writes == 0);
    }
}

static void test_transient_contention_and_erase_budget(void) {
    reset();
    fake.status_contentions = 3;
    queued_status(&fake, 0x84);
    begin();
    accept_seal();
    fake.read_contentions = 3;
    fake.erase_delay = 88;
    uint64_t started = fake.time;
    assert(defy_radio_factory_erase(&factory, 0x1c000) == 0);
    assert(fake.erases == 1 && fake.time - started >= 88 && fake.time - started < 500);

    reset();
    begin();
    accept_seal();
    fake.erase_delay = 600;
    started = fake.time;
    assert(defy_radio_factory_erase(&factory, 0x1c000) == -ETIMEDOUT);
    assert(fake.time - started <= 502 && factory.failed);
}

static void test_finish_delivery(void) {
    /* A finish whose bytes went out counts as sent, even if the deadline passes in the margin. */
    reset();
    fake.exchange_time = 0;
    begin();
    accept_seal();
    assert(defy_radio_factory_erase(&factory, 0x1c000) == 0);
    fake.seal_staged = false; /* Keep the fake from storing an unvalidated seal. */
    factory.validated = true;
    fake.delayed_release = fake.releases + 4;
    fake.release_delay = 496;
    uint64_t started = fake.time;
    assert(defy_radio_factory_finish(&factory) == 0);
    assert(fake.time - started == 500 && factory.finish_transmitted && fake.finishes == 1);
    assert_release_margin();

    /* A failed release after the bytes: possibly delivered. */
    reset();
    begin();
    accept_seal();
    assert(defy_radio_factory_erase(&factory, 0x1c000) == 0);
    fake.seal_staged = false;
    factory.validated = true;
    fake.fail_release = fake.releases + 4;
    assert(defy_radio_factory_finish(&factory) == DEFY_RADIO_FINISH_UNCONFIRMED);
    assert(fake.finishes == 1 && !factory.finish_transmitted && factory.failed);

    /* A failed payload exchange: possibly delivered. */
    reset();
    begin();
    accept_seal();
    assert(defy_radio_factory_erase(&factory, 0x1c000) == 0);
    factory.validated = true;
    fake.fail_exchange = fake.exchanges + 5;
    assert(defy_radio_factory_finish(&factory) == DEFY_RADIO_FINISH_UNCONFIRMED);
    assert(fake.finishes == 0 && !factory.finish_transmitted);

    /* A failed header exchange: never sent. */
    reset();
    begin();
    accept_seal();
    assert(defy_radio_factory_erase(&factory, 0x1c000) == 0);
    factory.validated = true;
    fake.fail_exchange = fake.exchanges + 4;
    assert(defy_radio_factory_finish(&factory) == -EIO);
    assert(fake.finishes == 0 && !factory.finish_transmitted && factory.failed);
}

/* As the driver meets Dygma's radio application: B and I go unanswered, and nothing is written. */
static void test_stock_application_does_not_begin(void) {
    reset();
    boot(&fake, STOCK_APPLICATION);
    assert(defy_radio_factory_begin(&factory, NULL) == -ETIMEDOUT);
    assert(factory.failed);
    assert(fake.packets == 2 && fake.mode == STOCK_APPLICATION);
    assert(fake.seals + fake.erases + fake.writes + fake.finishes == 0);
    assert(!fake.selected);
}

/* A transfer cut short as the radio started leaves 0x81 for the next poll, after which the link listens again. */
static void test_stale_error_before_hello(void) {
    reset();
    queued_status(&fake, 0x81);
    begin();
    assert(factory.info.program_start == 0x1c000);
}

/* The installer. */

static uint8_t image[5000];
static uint8_t image_seal[28];
static uint32_t image_version;

static void make_image(size_t size) {
    assert(size <= sizeof(image) && size % 4 == 0);
    for (size_t i = 0; i < size; i++) {
        image[i] = (uint8_t)(i * 31u + 5u);
    }
    put32(image, 0x20003600);
    put32(image + 4, 0x1c101);
    image_version = defy_radio_crc32(image, size);
    make_seal(image_seal, size, defy_radio_crc32(image, size), image_version);
}

static uint8_t untouched[FLASH_SIZE];

/* Everything outside the image, its erased tail and the seal is as the stock radio left it. */
static void assert_installed(size_t size) {
    assert(memcmp(fake.flash + 0x1c000, image, size) == 0);
    uint32_t erased_end = (0x1c000 + size + 4 + 4095) & ~4095u;
    for (uint32_t address = 0x1c000 + size; address < erased_end; address++) {
        assert(fake.flash[address] == 0xff);
    }
    assert(memcmp(fake.flash + 0x3d000, image_seal, 28) == 0);
    assert(memcmp(fake.flash, untouched, 0x1c000) == 0);
    assert(memcmp(fake.flash + erased_end, untouched + erased_end, 0x3d000 - erased_end) == 0);
    assert(memcmp(fake.flash + 0x3e000, untouched + 0x3e000, FLASH_SIZE - 0x3e000) == 0);
    assert(fake.max_programs <= 2);
    assert(fake.mode == APPLICATION && fake.seal_written);
}

static int install_size(size_t size, bool *installed) {
    return defy_radio_factory_install(&factory, image, size, image_seal, installed);
}

static void test_install_over_stock(void) {
    const size_t sizes[] = {8, 60, 64, 4092, 4096 - 4, 4800};
    for (size_t i = 0; i < sizeof(sizes) / sizeof(sizes[0]); i++) {
        make_image(sizes[i]);
        reset();
        fake.exchange_time = 0;
        memcpy(untouched, fake.flash, sizeof(untouched));
        begin();
        bool installed;
        assert(install_size(sizes[i], &installed) == 0 && installed);
        assert(factory.finish_transmitted);
        /* The vectors went last. */
        assert(fake.last_write == 0x1c000);
        assert_installed(sizes[i]);
        assert(fake.erases == ((sizes[i] + 4 + 4095) / 4096));
        /* Once after the erases, then once per chunk, to the whole image. */
        assert(fake.progress_reports == 1 + (sizes[i] + 63) / 64);
        assert(fake.progress_written == sizes[i] && fake.progress_size == sizes[i]);
    }
}

static void test_current_image_only_leaves(void) {
    make_image(4800);
    reset();
    fake.exchange_time = 0;
    begin();
    bool installed;
    assert(install_size(4800, &installed) == 0 && installed);
    memcpy(untouched, fake.flash, sizeof(untouched));
    uint8_t programs[FLASH_SIZE / 4];
    memcpy(programs, fake.programs, sizeof(programs));

    for (int boot_count = 0; boot_count < 5; boot_count++) {
        boot(&fake, BOOTLOADER);
        new_session();
        begin();
        assert(factory.info.program_version == image_version);
        unsigned int writes = fake.writes, erases = fake.erases, finishes = fake.finishes;
        unsigned int reports = fake.progress_reports;
        assert(install_size(4800, &installed) == 0 && !installed);
        assert(fake.writes == writes && fake.erases == erases && fake.finishes == finishes + 1);
        assert(fake.progress_reports == reports);
        assert(fake.mode == APPLICATION && !fake.seal_staged);
        assert(memcmp(untouched, fake.flash, sizeof(untouched)) == 0);
        assert(memcmp(programs, fake.programs, sizeof(programs)) == 0);
    }
}

static void test_same_version_with_other_seal_reinstalls(void) {
    make_image(4800);
    reset();
    fake.exchange_time = 0;
    begin();
    bool installed;
    assert(install_size(4800, &installed) == 0 && installed);
    /* Same version, but a seal with another CRC, as from a different build of that version. */
    erase_page(&fake, 0x3d000);
    uint8_t other[28];
    make_seal(other, 4800, 0x12345678, image_version);
    memcpy(fake.flash + 0x3d000, other, 28);
    boot(&fake, BOOTLOADER);
    new_session();
    begin();
    assert(install_size(4800, &installed) == 0 && installed);
    assert(memcmp(fake.flash + 0x3d000, image_seal, 28) == 0 && fake.max_programs <= 2);
}

static void test_refuses_radio_without_s113(void) {
    make_image(4800);
    reset();
    put32(fake.flash + 0x3004, 0xffffffffu);
    begin();
    assert(factory.info.program_start == 0x1000);
    unsigned int packets = fake.packets;
    bool installed;
    assert(install_size(4800, &installed) == -ENOTSUP && !installed);
    assert(fake.packets == packets && fake.writes == 0 && fake.erases == 0);
}

static void test_rejects_image_that_does_not_match_its_seal(void) {
    make_image(4800);
    reset();
    begin();
    unsigned int exchanges = fake.exchanges;
    bool installed;
    assert(install_size(4796, &installed) == -EINVAL);
    image[100] ^= 1;
    assert(install_size(4800, &installed) == -EINVAL);
    image[100] ^= 1;
    put32(image_seal + 12, 0x1000);
    assert(install_size(4800, &installed) == -EINVAL);
    assert(fake.exchanges == exchanges);
    assert(defy_radio_factory_install(&factory, image, 4800, image_seal, NULL) == -EINVAL);
}

/*
 * Power is lost part-way through each packet of an install in turn. After the
 * power cycle the bootloader starts a new session with whatever the packet
 * left, and the next install completes within the two programs per word.
 */
static void test_install_after_power_loss_at_every_packet(void) {
    make_image(4800);
    reset();
    fake.exchange_time = 0;
    begin();
    unsigned int start = fake.packets;
    bool installed;
    assert(install_size(4800, &installed) == 0);
    unsigned int total = fake.packets - start;

    for (unsigned int packet = 1; packet <= total; packet++) {
        reset();
        fake.exchange_time = 0;
        memcpy(untouched, fake.flash, sizeof(untouched));
        begin();
        fake.hang_at_packet = fake.packets + packet;
        fake.tear = true;
        int result = install_size(4800, &installed);
        assert(installed && (result != 0 || packet == total));
        boot(&fake, BOOTLOADER);
        new_session();
        begin();
        assert(install_size(4800, &installed) == 0 && installed);
        assert_installed(4800);
    }
}

/*
 * The RP2040's transfer fails at each exchange of an install in turn. Where
 * that leaves the bootloader between commands, holding its session, a new
 * install in the same session completes. Whether the bootloader resynchronises
 * in the middle of a command is not known, so those cases are left to a power
 * cycle, covered above.
 */
static void test_install_after_transfer_failure_in_the_same_session(void) {
    make_image(4800);
    reset();
    fake.exchange_time = 0;
    begin();
    unsigned int start = fake.exchanges;
    bool installed;
    assert(install_size(4800, &installed) == 0);
    unsigned int total = fake.exchanges - start;
    unsigned int retried = 0;

    for (unsigned int exchange = 1; exchange <= total; exchange++) {
        reset();
        fake.exchange_time = 0;
        memcpy(untouched, fake.flash, sizeof(untouched));
        begin();
        fake.fail_exchange = fake.exchanges + exchange;
        if (install_size(4800, &installed) == 0) {
            assert_installed(4800);
            continue;
        }
        if (fake.mode != BOOTLOADER || fake.selected || fake.parser != 0 ||
            fake.response_length != 0 || fake.writable) {
            continue;
        }
        /* The broken packet's error status ends the first new session, as the driver would retry. */
        int began = -1;
        for (int attempt = 0; attempt < 2 && began != 0; attempt++) {
            new_session();
            began = defy_radio_factory_begin(&factory, NULL);
        }
        assert(began == 0);
        assert(install_size(4800, &installed) == 0 && installed);
        assert_installed(4800);
        retried++;
    }
    assert(retried > 0);
}

static void test_begin_failure_writes_nothing(void) {
    reset();
    begin();
    unsigned int exchanges = fake.exchanges;
    unsigned int releases = fake.releases;

    for (unsigned int i = 1; i <= exchanges; i++) {
        reset();
        fake.fail_exchange = i;
        assert(defy_radio_factory_begin(&factory, NULL) == -EIO);
        assert(fake.seals + fake.erases + fake.writes + fake.finishes == 0);
        assert(!fake.selected);
    }
    for (unsigned int i = 1; i <= releases; i++) {
        reset();
        fake.fail_release = i;
        assert(defy_radio_factory_begin(&factory, NULL) == -EIO);
        assert(fake.seals + fake.erases + fake.writes + fake.finishes == 0);
        assert(!fake.selected);
    }
}

int main(void) {
    test_begin_failure_writes_nothing();
    test_operations();
    test_mutations_stay_in_the_slot();
    test_seal_required_for_mutation();
    test_leave_never_follows_a_seal();
    test_backpressure_is_not_replayed();
    test_errors();
    test_wrong_bootloader_identity();
    test_transient_contention_and_erase_budget();
    test_finish_delivery();
    test_stock_application_does_not_begin();
    test_stale_error_before_hello();
    test_install_over_stock();
    test_current_image_only_leaves();
    test_same_version_with_other_seal_reinstalls();
    test_refuses_radio_without_s113();
    test_rejects_image_that_does_not_match_its_seal();
    test_install_after_power_loss_at_every_packet();
    test_install_after_transfer_failure_in_the_same_session();
    return 0;
}
