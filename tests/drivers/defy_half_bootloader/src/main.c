/* SPDX-License-Identifier: MIT */

#include <errno.h>
#include <string.h>

#include <zephyr/device.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/drivers/i2c.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/byteorder.h>
#include <zephyr/sys/crc.h>
#include <zephyr/ztest.h>

static int32_t test_sleep(k_timeout_t timeout);
#define k_sleep test_sleep
#include "../../../../updater/defy_half_bootloader.c"
#undef k_sleep

enum phase {
    COMMAND,
    INFO,
    ERASE_ARGUMENTS,
    ERASE_REPLY,
    WRITE_ARGUMENTS,
    WRITE_DATA,
    WRITE_REPLY,
    READ_ARGUMENTS,
    READ_DATA,
    VALIDATE_REPLY,
};

static struct {
    enum phase phase;
    uint8_t info[44];
    uint8_t seal[28];
    uint8_t page[256];
    uint8_t erase_reply;
    uint8_t validate_reply;
    /* The erase, counted from 1, that the loader refuses; 0 for none. */
    unsigned int refused_erase;
    uint32_t erased_at[16];
    uint32_t write_crc_xor;
    uint32_t address;
    uint32_t size;
    unsigned int begun;
    unsigned int erased;
    unsigned int written;
    unsigned int finished;
} fake[2];
static struct defy_half_bootloader loader;
static struct defy_half_bootloader_config config;
static unsigned int transfers;
static unsigned int fail_transfer;
static int transfer_error;
static int configure_error;
static unsigned int configure_calls;
static uint32_t configured_speed;
static struct {
    gpio_pin_t pin;
    gpio_flags_t flags;
} gpio_calls[32];
static unsigned int gpio_count;
static unsigned int fail_gpio;
static gpio_pin_t fail_gpio_pin;
static gpio_flags_t fail_gpio_flags;
static int32_t sleeps[32];
static unsigned int sleep_count;

static int32_t test_sleep(k_timeout_t timeout) {
    zassert_true(sleep_count < ARRAY_SIZE(sleeps));
    sleeps[sleep_count++] = k_ticks_to_ms_ceil32(timeout.ticks);
    return 0;
}

static int fake_configure(const struct device *dev, uint32_t speed) {
    ARG_UNUSED(dev);
    configure_calls++;
    configured_speed = speed;
    return configure_error;
}

static int fake_transfer(const struct device *dev, struct i2c_msg *msgs, uint8_t count,
                          uint16_t address) {
    ARG_UNUSED(dev);
    zassert_equal(count, 1);
    zassert_true(address == 0x5a || address == 0x5b);
    zassert_equal(msgs->flags & I2C_MSG_STOP, I2C_MSG_STOP);
    zassert_equal(msgs->flags & I2C_MSG_RESTART, 0);
    if (++transfers == fail_transfer) {
        return transfer_error;
    }
    unsigned int side = address - 0x5a;
    uint8_t *bytes = msgs->buf;
    bool read = (msgs->flags & I2C_MSG_READ) != 0;
    if (!read && msgs->len == 1 && (bytes[0] == 'B' || bytes[0] == 'F')) {
        if (bytes[0] == 'B') {
            fake[side].begun++;
        } else {
            fake[side].finished++;
        }
        fake[side].phase = COMMAND;
        return 0;
    }
    switch (fake[side].phase) {
    case COMMAND:
        zassert_false(read);
        zassert_equal(msgs->len, 1);
        switch (bytes[0]) {
        case 'I':
            fake[side].phase = INFO;
            break;
        case 'E':
            fake[side].phase = ERASE_ARGUMENTS;
            break;
        case 'W':
            fake[side].phase = WRITE_ARGUMENTS;
            break;
        case 'R':
            fake[side].phase = READ_ARGUMENTS;
            break;
        case 'V':
            fake[side].phase = VALIDATE_REPLY;
            break;
        default:
            zassert_unreachable("unexpected command %u", bytes[0]);
        }
        break;
    case INFO:
        zassert_true(read);
        zassert_equal(msgs->len, sizeof(fake[side].info));
        memcpy(bytes, fake[side].info, msgs->len);
        fake[side].phase = COMMAND;
        break;
    case ERASE_ARGUMENTS:
    case WRITE_ARGUMENTS:
    case READ_ARGUMENTS:
        zassert_false(read);
        zassert_equal(msgs->len, 8);
        fake[side].address = sys_get_le32(bytes);
        fake[side].size = sys_get_le32(bytes + 4);
        if (fake[side].phase == ERASE_ARGUMENTS) {
            zassert_equal(fake[side].address % 4096, 0);
            zassert_equal(fake[side].size, 4096);
            if (fake[side].erased < ARRAY_SIZE(fake[side].erased_at)) {
                fake[side].erased_at[fake[side].erased] = fake[side].address;
            }
            fake[side].erased++;
            fake[side].phase = ERASE_REPLY;
        } else if (fake[side].phase == WRITE_ARGUMENTS) {
            zassert_equal(fake[side].address % 256, 0);
            zassert_equal(fake[side].size, 256);
            fake[side].phase = WRITE_DATA;
        } else {
            zassert_equal(fake[side].address, 0x5000);
            zassert_equal(fake[side].size, 28);
            fake[side].phase = READ_DATA;
        }
        break;
    case ERASE_REPLY:
        zassert_true(read);
        zassert_equal(msgs->len, 1);
        bytes[0] = fake[side].erased == fake[side].refused_erase ? 0 : fake[side].erase_reply;
        fake[side].phase = COMMAND;
        break;
    case WRITE_DATA:
        zassert_false(read);
        zassert_equal(msgs->len, sizeof(fake[side].page));
        memcpy(fake[side].page, bytes, msgs->len);
        fake[side].written++;
        fake[side].phase = WRITE_REPLY;
        break;
    case WRITE_REPLY:
        zassert_true(read);
        zassert_equal(msgs->len, 4);
        sys_put_le32(crc32_ieee(fake[side].page, sizeof(fake[side].page)) ^
                         fake[side].write_crc_xor,
                     bytes);
        fake[side].phase = COMMAND;
        break;
    case READ_DATA:
        zassert_true(read);
        zassert_equal(msgs->len, sizeof(fake[side].seal));
        memcpy(bytes, fake[side].seal, msgs->len);
        fake[side].phase = COMMAND;
        break;
    case VALIDATE_REPLY:
        zassert_true(read);
        zassert_equal(msgs->len, 1);
        bytes[0] = fake[side].validate_reply;
        fake[side].phase = COMMAND;
        break;
    }
    return 0;
}

static DEVICE_API(i2c, bus_api) = {
    .configure = fake_configure,
    .transfer = fake_transfer,
};
DEVICE_DEFINE(test_bus, "half-bootloader-bus", NULL, NULL, NULL, NULL, POST_KERNEL, 50, &bus_api);

static int missing_init(const struct device *dev) {
    ARG_UNUSED(dev);
    return -ENODEV;
}
DEVICE_DEFINE(missing_bus, "missing-half-bus", missing_init, NULL, NULL, NULL, POST_KERNEL, 50,
               &bus_api);

static int fake_pin_configure(const struct device *dev, gpio_pin_t pin, gpio_flags_t flags) {
    ARG_UNUSED(dev);
    zassert_true(gpio_count < ARRAY_SIZE(gpio_calls));
    gpio_calls[gpio_count].pin = pin;
    gpio_calls[gpio_count++].flags = flags;
    return gpio_count == fail_gpio || (pin == fail_gpio_pin && flags == fail_gpio_flags)
               ? -EIO
               : 0;
}

static const struct gpio_driver_config gpio_config = {.port_pin_mask = UINT32_MAX};
static struct gpio_driver_data gpio_data;
static DEVICE_API(gpio, gpio_api) = {.pin_configure = fake_pin_configure};
DEVICE_DEFINE(test_gpio, "half-reset-gpio", NULL, NULL, &gpio_data, &gpio_config, POST_KERNEL, 50,
               &gpio_api);

static void seal_crc(unsigned int side) {
    sys_put_le32(0, fake[side].seal + 8);
    sys_put_le32(crc32_ieee(fake[side].seal, sizeof(fake[side].seal)), fake[side].seal + 8);
}

static void before(void *fixture) {
    ARG_UNUSED(fixture);
    memset(fake, 0, sizeof(fake));
    memset(&loader, 0, sizeof(loader));
    transfers = fail_transfer = configure_calls = gpio_count = fail_gpio = sleep_count = 0;
    fail_gpio_pin = UINT8_MAX;
    fail_gpio_flags = 0;
    configure_error = 0;
    transfer_error = -EIO;
    config = (struct defy_half_bootloader_config){
        .bus = DEVICE_GET(test_bus),
        .reset = {{.port = DEVICE_GET(test_gpio), .pin = 28},
                  {.port = DEVICE_GET(test_gpio), .pin = 29}},
    };
    const uint32_t info[] = {0x4b534352, 1, 1, 0x1000000, 0x5000, 0x5000,
                             0x5100, 0x1fb000, 4096, 256, 1024};
    const uint32_t seal[] = {1, 28, 0, 0x6000, 0x2a7c0, 0x3c1a7b52, 0x2000100};
    for (unsigned int side = 0; side < 2; side++) {
        for (unsigned int i = 0; i < ARRAY_SIZE(info); i++) {
            sys_put_le32(info[i], fake[side].info + i * 4);
        }
        for (unsigned int i = 0; i < ARRAY_SIZE(seal); i++) {
            sys_put_le32(seal[i], fake[side].seal + i * 4);
        }
        seal_crc(side);
        fake[side].erase_reply = 1;
        fake[side].validate_reply = 1;
    }
}

static void initialize(void) {
    zassert_ok(defy_half_bootloader_init(&loader, &config));
}

static void acquire(void) {
    initialize();
    zassert_ok(defy_half_bootloader_reset_all(&loader));
}

static void begin(unsigned int side) {
    struct defy_half_bootloader_info info;
    zassert_ok(defy_half_bootloader_begin(&loader, side, &info));
    zassert_equal(info.device_id, 0x4b534352);
    zassert_equal(info.bootloader_version, 1);
    zassert_equal(info.program_start, 0x5100);
}

static void open_left(void) {
    acquire();
    begin(DEFY_WIRED_LEFT);
}

static const uint8_t page[256] = {1, 7, 19, 0xfe};

static int write_page(uint32_t address) {
    return defy_half_bootloader_write_page(&loader, DEFY_WIRED_LEFT, address, page);
}

ZTEST(defy_half_bootloader, test_init_rejects_missing_devices) {
    config.bus = NULL;
    zassert_equal(defy_half_bootloader_init(&loader, &config), -EINVAL);
    config.bus = DEVICE_GET(missing_bus);
    zassert_equal(defy_half_bootloader_init(&loader, &config), -ENODEV);
    zassert_equal(configure_calls, 0);
    zassert_equal(gpio_count, 0);
}

ZTEST(defy_half_bootloader, test_init_configures_100khz_and_propagates_error) {
    configure_error = -ENOTSUP;
    zassert_equal(defy_half_bootloader_init(&loader, &config), -ENOTSUP);
    configure_error = 0;
    initialize();
    zassert_equal(configured_speed, I2C_MODE_CONTROLLER | I2C_SPEED_SET(I2C_SPEED_STANDARD));
    zassert_equal(gpio_count, 0);
}

ZTEST(defy_half_bootloader, test_reset_asserts_both_and_releases_as_inputs) {
    acquire();
    zassert_equal(gpio_count, 4);
    zassert_equal(gpio_calls[0].pin, 28);
    zassert_equal(gpio_calls[1].pin, 29);
    zassert_equal(gpio_calls[0].flags, GPIO_OUTPUT_LOW);
    zassert_equal(gpio_calls[1].flags, GPIO_OUTPUT_LOW);
    zassert_equal(gpio_calls[2].flags, GPIO_INPUT);
    zassert_equal(gpio_calls[3].flags, GPIO_INPUT);
    zassert_equal(sleep_count, 2);
    zassert_equal(sleeps[0], 10);
    zassert_equal(sleeps[1], 50);
    zassert_equal(transfers, 0);
}

ZTEST(defy_half_bootloader, test_reset_error_still_releases_both_lines) {
    initialize();
    fail_gpio = 1;
    zassert_equal(defy_half_bootloader_reset_all(&loader), -EIO);
    zassert_equal(gpio_count, 4);
    zassert_equal(gpio_calls[2].flags, GPIO_INPUT);
    zassert_equal(gpio_calls[3].flags, GPIO_INPUT);
    zassert_equal(sleep_count, 2);
    zassert_equal(sleeps[0], 10);
    zassert_equal(sleeps[1], 50);
    zassert_true(loader.acquired);
    zassert_equal(loader.reset_pending, BIT(DEFY_WIRED_LEFT));
    struct defy_half_bootloader_info info;
    zassert_equal(defy_half_bootloader_begin(&loader, DEFY_WIRED_LEFT, &info), -EBUSY);
    zassert_equal(defy_half_bootloader_begin(&loader, DEFY_WIRED_RIGHT, &info), -EBUSY);
    zassert_equal(defy_half_bootloader_erase_sector(&loader, DEFY_WIRED_RIGHT, 0x5000), -EBUSY);
    zassert_equal(defy_half_bootloader_erase_settings(&loader, DEFY_WIRED_RIGHT), -EBUSY);
}

ZTEST(defy_half_bootloader, test_abort_before_acquisition_does_nothing) {
    initialize();
    zassert_ok(defy_half_bootloader_abort(&loader));
    zassert_equal(gpio_count, 0);
    zassert_equal(transfers, 0);
    zassert_equal(sleep_count, 0);
}

ZTEST(defy_half_bootloader, test_recover_without_session_resets_once_and_releases_acquisition) {
    initialize();
    zassert_ok(defy_half_bootloader_recover(&loader));
    zassert_false(loader.acquired);
    zassert_equal(gpio_count, 4);
    zassert_equal(transfers, 0);
    zassert_equal(sleep_count, 2);
    zassert_ok(defy_half_bootloader_abort(&loader));
    zassert_equal(gpio_count, 4);
}

ZTEST(defy_half_bootloader, test_begin_needs_reset_and_maps_logical_sides) {
    initialize();
    struct defy_half_bootloader_info info;
    zassert_equal(defy_half_bootloader_begin(&loader, DEFY_WIRED_LEFT, &info), -EINVAL);
    zassert_ok(defy_half_bootloader_reset_all(&loader));
    begin(DEFY_WIRED_LEFT);
    begin(DEFY_WIRED_RIGHT);
    zassert_equal(fake[0].begun, 1);
    zassert_equal(fake[1].begun, 1);
    zassert_equal(transfers, 6);
    zassert_equal(defy_half_bootloader_begin(&loader, DEFY_WIRED_RIGHT, &info), -EALREADY);
    zassert_equal(defy_half_bootloader_begin(&loader, DEFY_HALF_BOOTLOADER_SIDES, &info), -EINVAL);
    zassert_equal(defy_half_bootloader_begin(&loader, -1, &info), -EINVAL);
    zassert_equal(transfers, 6);
}

ZTEST(defy_half_bootloader, test_malformed_geometry_prevents_writes) {
    const unsigned int fields[] = {2, 4, 5, 6, 7, 8, 9, 10};
    for (unsigned int i = 0; i < ARRAY_SIZE(fields); i++) {
        before(NULL);
        acquire();
        sys_put_le32(0, fake[0].info + fields[i] * 4);
        struct defy_half_bootloader_info info;
        memset(&info, 0x5a, sizeof(info));
        struct defy_half_bootloader_info unchanged = info;
        zassert_equal(defy_half_bootloader_begin(&loader, DEFY_WIRED_LEFT, &info), -ENOTSUP);
        zassert_mem_equal(&info, &unchanged, sizeof(info));
        zassert_equal(defy_half_bootloader_erase_sector(&loader, DEFY_WIRED_LEFT, 0x5000), -EACCES);
        zassert_equal(fake[0].erased, 0);
    }
}

ZTEST(defy_half_bootloader, test_begin_errors_preserve_output_and_abort_cleans_partial_begin) {
    for (unsigned int failing = 1; failing <= 3; failing++) {
        before(NULL);
        acquire();
        fail_transfer = failing;
        transfer_error = failing == 1 ? -ETIMEDOUT : -EIO;
        struct defy_half_bootloader_info info;
        memset(&info, 0x5a, sizeof(info));
        struct defy_half_bootloader_info unchanged = info;
        zassert_equal(defy_half_bootloader_begin(&loader, DEFY_WIRED_LEFT, &info), transfer_error);
        zassert_mem_equal(&info, &unchanged, sizeof(info));
        zassert_ok(defy_half_bootloader_abort(&loader));
        zassert_equal(fake[0].finished, 0);
        zassert_false(loader.acquired);
    }
}

ZTEST(defy_half_bootloader, test_erase_bounds_reject_bootloader_settings_and_overflow) {
    open_left();
    const uint32_t bad[] = {0, 0x4000, 0x5001, 0x1bf100, 0x1c0000, UINT32_MAX};
    unsigned int start = transfers;
    for (unsigned int i = 0; i < ARRAY_SIZE(bad); i++) {
        zassert_equal(defy_half_bootloader_erase_sector(&loader, DEFY_WIRED_LEFT, bad[i]), -EINVAL);
    }
    zassert_equal(transfers, start);
    zassert_ok(defy_half_bootloader_erase_sector(&loader, DEFY_WIRED_LEFT, 0x1bf000));
    zassert_equal(fake[0].address, 0x1bf000);
}

ZTEST(defy_half_bootloader, test_pages_require_erased_sector_and_sequential_offsets) {
    open_left();
    zassert_equal(write_page(0x5000), -EACCES);
    zassert_ok(defy_half_bootloader_erase_sector(&loader, DEFY_WIRED_LEFT, 0x5000));
    zassert_equal(write_page(0x5100), -EACCES);
    for (uint32_t address = 0x5000; address < 0x6000; address += 256) {
        zassert_ok(write_page(address));
    }
    zassert_equal(fake[0].written, 16);
    zassert_mem_equal(fake[0].page, page, sizeof(page));
    zassert_equal(write_page(0x6000), -EACCES);
    zassert_equal(write_page(0x5000), -EACCES);
    zassert_ok(defy_half_bootloader_erase_sector(&loader, DEFY_WIRED_LEFT, 0x6000));
    zassert_ok(write_page(0x6000));
}

ZTEST(defy_half_bootloader, test_page_bounds_do_not_touch_bus) {
    open_left();
    zassert_ok(defy_half_bootloader_erase_sector(&loader, DEFY_WIRED_LEFT, 0x5000));
    unsigned int start = transfers;
    const uint32_t bad[] = {0, 0x4f00, 0x5001, 0x1bff01, 0x1c0000, UINT32_MAX};
    for (unsigned int i = 0; i < ARRAY_SIZE(bad); i++) {
        zassert_equal(write_page(bad[i]), -EINVAL);
    }
    zassert_equal(defy_half_bootloader_write_page(&loader, DEFY_WIRED_LEFT, 0x5000, NULL), -EINVAL);
    zassert_equal(transfers, start);
    zassert_ok(write_page(0x5000));
}

ZTEST(defy_half_bootloader, test_last_allowed_page_can_be_written) {
    open_left();
    zassert_ok(defy_half_bootloader_erase_sector(&loader, DEFY_WIRED_LEFT, 0x1bf000));
    for (uint32_t address = 0x1bf000; address < 0x1c0000; address += 256) {
        zassert_ok(write_page(address));
    }
    zassert_equal(fake[0].address, 0x1bff00);
    zassert_equal(write_page(0x1c0000), -EINVAL);
}

ZTEST(defy_half_bootloader, test_erase_failures_close_only_selected_side) {
    for (unsigned int failing = 1; failing <= 3; failing++) {
        before(NULL);
        open_left();
        begin(DEFY_WIRED_RIGHT);
        fail_transfer = transfers + failing;
        zassert_equal(defy_half_bootloader_erase_sector(&loader, DEFY_WIRED_LEFT, 0x5000), -EIO);
        zassert_false(loader.sides[0].ready);
        zassert_true(loader.sides[1].ready);
        zassert_equal(write_page(0x5000), -EACCES);
    }
    before(NULL);
    open_left();
    fake[0].erase_reply = 0;
    zassert_equal(defy_half_bootloader_erase_sector(&loader, DEFY_WIRED_LEFT, 0x5000), -EIO);
    zassert_equal(write_page(0x5000), -EACCES);
}

ZTEST(defy_half_bootloader, test_write_failures_and_returned_crc_mismatch_close_side) {
    for (unsigned int failing = 1; failing <= 4; failing++) {
        before(NULL);
        open_left();
        zassert_ok(defy_half_bootloader_erase_sector(&loader, DEFY_WIRED_LEFT, 0x5000));
        fail_transfer = transfers + failing;
        transfer_error = -ETIMEDOUT;
        zassert_equal(write_page(0x5000), -ETIMEDOUT);
        zassert_equal(write_page(0x5000), -EACCES);
    }
    before(NULL);
    open_left();
    zassert_ok(defy_half_bootloader_erase_sector(&loader, DEFY_WIRED_LEFT, 0x5000));
    fake[0].write_crc_xor = 1;
    zassert_equal(write_page(0x5000), -EBADMSG);
    zassert_false(loader.sides[0].ready);
}

ZTEST(defy_half_bootloader, test_settings_erase_covers_the_nvs_sectors_in_order) {
    open_left();
    begin(DEFY_WIRED_RIGHT);
    unsigned int start = transfers;
    zassert_ok(defy_half_bootloader_erase_settings(&loader, DEFY_WIRED_LEFT));
    zassert_equal(fake[0].erased, 8);
    for (unsigned int i = 0; i < 8; i++) {
        zassert_equal(fake[0].erased_at[i], 0x1c0000 + i * 4096);
    }
    zassert_equal(transfers - start, 8 * 3);
    zassert_equal(fake[1].erased, 0);
    zassert_true(loader.sides[0].ready);
    zassert_ok(defy_half_bootloader_erase_settings(&loader, DEFY_WIRED_RIGHT));
    zassert_equal(fake[1].erased, 8);
    zassert_equal(fake[1].erased_at[0], 0x1c0000);
    zassert_equal(fake[1].erased_at[7], 0x1c7000);
    zassert_equal(fake[0].erased, 8);
}

ZTEST(defy_half_bootloader, test_settings_erase_leaves_no_page_writable) {
    open_left();
    zassert_ok(defy_half_bootloader_erase_sector(&loader, DEFY_WIRED_LEFT, 0x5000));
    zassert_ok(write_page(0x5000));
    zassert_ok(defy_half_bootloader_erase_settings(&loader, DEFY_WIRED_LEFT));
    unsigned int start = transfers;
    zassert_equal(write_page(0x5100), -EACCES);
    zassert_equal(write_page(0x1c0000), -EINVAL);
    zassert_equal(write_page(0x1c7f00), -EINVAL);
    zassert_equal(defy_half_bootloader_erase_sector(&loader, DEFY_WIRED_LEFT, 0x1c0000), -EINVAL);
    zassert_equal(defy_half_bootloader_erase_sector(&loader, DEFY_WIRED_LEFT, 0x1c7000), -EINVAL);
    zassert_equal(transfers, start);
    zassert_equal(fake[0].written, 1);
    zassert_ok(defy_half_bootloader_erase_sector(&loader, DEFY_WIRED_LEFT, 0x5000));
    zassert_ok(write_page(0x5000));
}

ZTEST(defy_half_bootloader, test_settings_erase_needs_a_begun_half) {
    initialize();
    zassert_equal(defy_half_bootloader_erase_settings(&loader, DEFY_WIRED_LEFT), -EACCES);
    zassert_ok(defy_half_bootloader_reset_all(&loader));
    zassert_equal(defy_half_bootloader_erase_settings(&loader, DEFY_WIRED_LEFT), -EACCES);
    begin(DEFY_WIRED_LEFT);
    zassert_equal(defy_half_bootloader_erase_settings(&loader, DEFY_WIRED_RIGHT), -EACCES);
    zassert_equal(defy_half_bootloader_erase_settings(&loader, DEFY_HALF_BOOTLOADER_SIDES),
                  -EINVAL);
    zassert_equal(defy_half_bootloader_erase_settings(&loader, -1), -EINVAL);
    zassert_equal(transfers, 3);
    zassert_equal(fake[0].erased + fake[1].erased, 0);
}

ZTEST(defy_half_bootloader, test_settings_erase_needs_the_reported_flash_to_reach_them) {
    acquire();
    /* Flash available from 0x5000: up to 0x1c8000 needs 0x1c3000 bytes. */
    sys_put_le32(0x1c3000 - 1, fake[0].info + 28);
    sys_put_le32(0x1c3000, fake[1].info + 28);
    begin(DEFY_WIRED_LEFT);
    begin(DEFY_WIRED_RIGHT);
    unsigned int start = transfers;
    zassert_equal(defy_half_bootloader_erase_settings(&loader, DEFY_WIRED_LEFT), -ERANGE);
    zassert_equal(transfers, start);
    zassert_true(loader.sides[0].ready);
    zassert_ok(defy_half_bootloader_erase_sector(&loader, DEFY_WIRED_LEFT, 0x5000));
    zassert_ok(defy_half_bootloader_erase_settings(&loader, DEFY_WIRED_RIGHT));
    zassert_equal(fake[1].erased, 8);
}

ZTEST(defy_half_bootloader, test_settings_erase_failure_stops_and_closes_only_that_side) {
    for (unsigned int failing = 1; failing <= 3; failing++) {
        before(NULL);
        open_left();
        begin(DEFY_WIRED_RIGHT);
        fail_transfer = transfers + 2 * 3 + failing;
        zassert_equal(defy_half_bootloader_erase_settings(&loader, DEFY_WIRED_LEFT), -EIO);
        zassert_equal(transfers, fail_transfer);
        zassert_false(loader.sides[0].ready);
        zassert_true(loader.sides[1].ready);
        zassert_equal(defy_half_bootloader_erase_settings(&loader, DEFY_WIRED_LEFT), -EACCES);
        zassert_equal(transfers, fail_transfer);
    }
    before(NULL);
    open_left();
    fake[0].refused_erase = 5;
    zassert_equal(defy_half_bootloader_erase_settings(&loader, DEFY_WIRED_LEFT), -EIO);
    zassert_equal(fake[0].erased, 5);
    zassert_equal(fake[0].erased_at[4], 0x1c4000);
    zassert_false(loader.sides[0].ready);
    zassert_equal(write_page(0x5000), -EACCES);
}

ZTEST(defy_half_bootloader, test_seal_read_without_program_validation) {
    open_left();
    struct defy_half_seal seal;
    fake[0].validate_reply = 0;
    zassert_ok(defy_half_bootloader_read_seal(&loader, DEFY_WIRED_LEFT, &seal));
    zassert_equal(seal.program_start, 0x6000);
    zassert_equal(seal.program_size, 0x2a7c0);
    zassert_equal(seal.program_crc, 0x3c1a7b52);
    zassert_equal(seal.program_version, 0x2000100);
}

ZTEST(defy_half_bootloader, test_invalid_seal_never_replaces_output_and_remains_flashable) {
    for (unsigned int field = 0; field < 5; field++) {
        before(NULL);
        open_left();
        sys_put_le32(field == 4 ? UINT32_MAX : 0, fake[0].seal + field * 4);
        if (field != 2) {
            seal_crc(0);
        }
        struct defy_half_seal seal;
        memset(&seal, 0x5a, sizeof(seal));
        struct defy_half_seal unchanged = seal;
        zassert_equal(defy_half_bootloader_read_seal(&loader, DEFY_WIRED_LEFT, &seal),
                      -EBADMSG);
        zassert_mem_equal(&seal, &unchanged, sizeof(seal));
        zassert_ok(defy_half_bootloader_erase_sector(&loader, DEFY_WIRED_LEFT, 0x5000));
    }
}

ZTEST(defy_half_bootloader, test_invalid_program_remains_flashable) {
    open_left();
    struct defy_half_seal seal;
    fake[0].validate_reply = 0;
    zassert_equal(defy_half_bootloader_validate(&loader, DEFY_WIRED_LEFT, &seal), -EBADMSG);
    zassert_ok(defy_half_bootloader_erase_sector(&loader, DEFY_WIRED_LEFT, 0x5000));
}

ZTEST(defy_half_bootloader, test_validation_requires_both_program_and_seal) {
    open_left();
    struct defy_half_seal seal;
    fake[0].seal[8] ^= 1;
    zassert_equal(defy_half_bootloader_validate(&loader, DEFY_WIRED_LEFT, &seal), -EBADMSG);
    seal_crc(0);
    zassert_ok(defy_half_bootloader_validate(&loader, DEFY_WIRED_LEFT, &seal));
    zassert_equal(seal.program_crc, 0x3c1a7b52);
    zassert_ok(defy_half_bootloader_erase_sector(&loader, DEFY_WIRED_LEFT, 0x5000));
}

ZTEST(defy_half_bootloader, test_validation_transfer_errors_preserve_output) {
    for (unsigned int failing = 1; failing <= 5; failing++) {
        before(NULL);
        open_left();
        fail_transfer = transfers + failing;
        struct defy_half_seal seal;
        memset(&seal, 0x5a, sizeof(seal));
        struct defy_half_seal unchanged = seal;
        zassert_equal(defy_half_bootloader_validate(&loader, DEFY_WIRED_LEFT, &seal), -EIO);
        zassert_mem_equal(&seal, &unchanged, sizeof(seal));
        zassert_false(loader.sides[0].ready);
    }
}

ZTEST(defy_half_bootloader, test_abort_resets_both_without_issuing_finish) {
    open_left();
    begin(DEFY_WIRED_RIGHT);
    unsigned int start = transfers;
    fail_transfer = transfers + 1;
    zassert_ok(defy_half_bootloader_abort(&loader));
    zassert_equal(transfers, start);
    zassert_equal(fake[0].finished, 0);
    zassert_equal(fake[1].finished, 0);
    zassert_equal(gpio_count, 8);
    zassert_equal(sleeps[sleep_count - 2], 10);
    zassert_equal(sleeps[sleep_count - 1], 50);
    zassert_false(loader.acquired);
    zassert_false(loader.sides[0].ready);
    zassert_false(loader.sides[1].ready);
    zassert_ok(defy_half_bootloader_abort(&loader));
    zassert_equal(transfers, start);
    zassert_equal(gpio_count, 8);
}

ZTEST(defy_half_bootloader, test_abort_retries_failed_reset_assertion_and_release) {
    for (unsigned int failing = 1; failing <= 4; failing++) {
        before(NULL);
        open_left();
        begin(DEFY_WIRED_RIGHT);
        fail_gpio = gpio_count + failing;
        zassert_equal(defy_half_bootloader_abort(&loader), -EIO);
        zassert_true(loader.acquired);
        zassert_equal(gpio_count, 8);
        zassert_equal(fake[0].finished, 0);
        zassert_equal(fake[1].finished, 0);
        unsigned int transfer_count = transfers;
        unsigned int sleep_start = sleep_count;
        zassert_ok(defy_half_bootloader_abort(&loader));
        zassert_false(loader.acquired);
        unsigned int retried_calls = failing <= 2 ? 2 : 1;
        zassert_equal(gpio_count, 8 + retried_calls);
        unsigned int failed_side = (failing - 1) % 2;
        for (unsigned int i = 8; i < gpio_count; i++) {
            zassert_equal(gpio_calls[i].pin, config.reset[failed_side].pin);
        }
        zassert_equal(gpio_calls[gpio_count - 1].flags, GPIO_INPUT);
        zassert_equal(sleep_count - sleep_start, retried_calls);
        zassert_equal(sleeps[sleep_count - 1], 50);
        if (retried_calls == 2) {
            zassert_equal(gpio_calls[8].flags, GPIO_OUTPUT_LOW);
            zassert_equal(sleeps[sleep_start], 10);
        }
        zassert_equal(transfers, transfer_count);
        zassert_ok(defy_half_bootloader_abort(&loader));
        zassert_equal(gpio_count, 8 + retried_calls);
    }
}

ZTEST(defy_half_bootloader, test_permanent_reset_failure_does_not_reset_completed_half) {
    const gpio_flags_t failures[] = {GPIO_OUTPUT_LOW, GPIO_INPUT};
    for (unsigned int side = 0; side < DEFY_HALF_BOOTLOADER_SIDES; side++) {
        for (unsigned int mode = 0; mode < ARRAY_SIZE(failures); mode++) {
            before(NULL);
            open_left();
            begin(DEFY_WIRED_RIGHT);
            fail_gpio_pin = config.reset[side].pin;
            fail_gpio_flags = failures[mode];
            zassert_equal(defy_half_bootloader_abort(&loader), -EIO);
            zassert_equal(gpio_count, 8);
            for (unsigned int attempt = 0; attempt < 3; attempt++) {
                unsigned int start = gpio_count;
                zassert_equal(defy_half_bootloader_abort(&loader), -EIO);
                zassert_true(loader.acquired);
                zassert_equal(loader.reset_pending, BIT(side));
                for (unsigned int i = start; i < gpio_count; i++) {
                    zassert_equal(gpio_calls[i].pin, config.reset[side].pin);
                }
            }
            zassert_equal(fake[0].finished, 0);
            zassert_equal(fake[1].finished, 0);
            fail_gpio_pin = UINT8_MAX;
            zassert_ok(defy_half_bootloader_abort(&loader));
            zassert_false(loader.acquired);
            zassert_equal(loader.reset_pending, 0);
            zassert_equal(loader.reset_asserted, 0);
        }
    }
}

ZTEST(defy_half_bootloader, test_new_session_after_abort_resets_both_halves_again) {
    open_left();
    zassert_ok(defy_half_bootloader_abort(&loader));
    zassert_false(loader.acquired);
    zassert_ok(defy_half_bootloader_reset_all(&loader));
    zassert_true(loader.acquired);
    zassert_equal(gpio_count, 12);
    zassert_equal(gpio_calls[8].pin, config.reset[0].pin);
    zassert_equal(gpio_calls[9].pin, config.reset[1].pin);
    zassert_equal(gpio_calls[10].pin, config.reset[0].pin);
    zassert_equal(gpio_calls[11].pin, config.reset[1].pin);
    begin(DEFY_WIRED_LEFT);
    begin(DEFY_WIRED_RIGHT);
}

ZTEST_SUITE(defy_half_bootloader, NULL, NULL, before, NULL, NULL);
