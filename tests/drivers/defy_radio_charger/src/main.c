/* SPDX-License-Identifier: MIT */

#include <errno.h>
#include <string.h>

#include <zephyr/drivers/i2c.h>
#include <zephyr/ztest.h>

#include "charger.h"

struct transfer {
    bool write;
    uint8_t reg;
    uint8_t value;
};

static uint8_t registers[7];
static bool safety_locked;
static struct transfer transfers[128];
static size_t transfer_count;
static int failure_at;
static bool failure_after_write;
static int ignored_register;
static unsigned int enables;
static unsigned int watchdog_feeds;
static bool assert_safe_enable;

static void reset_registers(void) {
    const uint8_t defaults[] = {0xc0, 0x70, 0x0a, 0x94, 0x89, 0x24, 0x40};

    memcpy(registers, defaults, sizeof(registers));
}

static int transfer(const struct device *dev, struct i2c_msg *msgs, uint8_t count,
                    uint16_t address) {
    ARG_UNUSED(dev);
    zassert_equal(address, 0x6a);
    zassert_true(transfer_count < ARRAY_SIZE(transfers));
    uint8_t reg = msgs[0].buf[0];
    zassert_true(reg < ARRAY_SIZE(registers));
    bool write = count == 1;
    size_t index = transfer_count++;
    transfers[index] = (struct transfer){.write = write, .reg = reg};
    if (write) {
        zassert_equal(msgs[0].len, 2);
        transfers[index].value = msgs[0].buf[1];
    } else {
        zassert_equal(msgs[0].len, 1);
        zassert_equal(msgs[1].len, 1);
    }
    if ((int)index == failure_at && !(write && failure_after_write)) {
        return -ETIMEDOUT;
    }
    if (!write) {
        uint8_t value = registers[reg];
        if (reg == 4) {
            value |= 0x80;
        }
        msgs[1].buf[0] = value;
        return 0;
    }

    uint8_t value = msgs[0].buf[1];
    zassert_not_equal(reg, 3);
    zassert_false(reg == 4 && (value & 0x80));
    zassert_false(reg == 1 && (value & 0x03));
    if (reg != 6) {
        safety_locked = true;
    }
    if (reg == ignored_register) {
        return 0;
    }
    if (reg == 6) {
        if (!safety_locked) {
            registers[6] = value;
        }
    } else if (reg == 0) {
        if (value & 0x80) {
            watchdog_feeds++;
        }
        registers[0] = (registers[0] & 0xbf) | (value & 0x40);
    } else if (reg == 4) {
        registers[4] = MIN(value & 0x70, registers[6] & 0x70) | (value & 0x0f);
    } else {
        if (reg == 1 && !(value & 4)) {
            enables++;
            if (assert_safe_enable) {
                zassert_equal(registers[6] & 0x8f, 0);
                zassert_equal(registers[2], 0x8c);
                zassert_equal(registers[4] & 0x7f, (registers[6] & 0x70) | 9);
                zassert_equal(registers[5] & 0x67, 0x44);
            }
        }
        registers[reg] = value;
    }
    return (int)index == failure_at ? -ETIMEDOUT : 0;
}

static DEVICE_API(i2c, charger_api) = {.transfer = transfer};

DEVICE_DEFINE(charger_bus, "charger-bus", NULL, NULL, NULL, NULL, POST_KERNEL,
              CONFIG_I2C_INIT_PRIORITY, &charger_api);

static int missing_bus_init(const struct device *dev) {
    ARG_UNUSED(dev);
    return -ENODEV;
}

DEVICE_DEFINE(missing_bus, "missing-charger-bus", missing_bus_init, NULL, NULL, NULL,
              POST_KERNEL, CONFIG_I2C_INIT_PRIORITY, &charger_api);

static const struct device *const bus = DEVICE_GET(charger_bus);

static void before(void *fixture) {
    ARG_UNUSED(fixture);
    reset_registers();
    safety_locked = false;
    transfer_count = 0;
    failure_at = -1;
    failure_after_write = false;
    ignored_register = -1;
    enables = 0;
    watchdog_feeds = 0;
    assert_safe_enable = true;
}

static void clear_transfers(void) {
    transfer_count = 0;
    enables = 0;
    watchdog_feeds = 0;
}

ZTEST(defy_radio_charger, test_stock_limits_are_verified_before_enable) {
    zassert_ok(defy_radio_charger_service(bus));
    const uint8_t expected[] = {0xc0, 0xc8, 0x8c, 0x94, 0x79, 0x44, 0x70};
    zassert_mem_equal(registers, expected, sizeof(expected));
    zassert_equal(enables, 1);
    zassert_equal(watchdog_feeds, 1);
    const uint8_t writes[][2] = {
        {6, 0x70}, {6, 0x70}, {1, 0xcc}, {5, 0x44},
        {4, 0x79}, {2, 0x8c}, {0, 0xc0}, {1, 0xc8},
    };
    size_t index = 0;
    for (size_t i = 0; i < transfer_count; i++) {
        if (transfers[i].write) {
            zassert_true(index < ARRAY_SIZE(writes));
            zassert_equal(transfers[i].reg, writes[index][0]);
            zassert_equal(transfers[i].value, writes[index][1]);
            index++;
        }
    }
    zassert_equal(index, ARRAY_SIZE(writes));
}

ZTEST(defy_radio_charger, test_stable_settings_only_feed_watchdog) {
    zassert_ok(defy_radio_charger_service(bus));
    clear_transfers();
    registers[0] = 0xda;
    registers[5] |= 0x18;
    zassert_ok(defy_radio_charger_service(bus));
    zassert_equal(enables, 0);
    zassert_equal(watchdog_feeds, 1);
    zassert_equal(transfer_count, 7);
    for (size_t i = 0; i + 1 < transfer_count; i++) {
        zassert_false(transfers[i].write);
    }
    zassert_equal(registers[0], 0xda);
}

ZTEST(defy_radio_charger, test_unknown_identity_and_unavailable_bus_are_never_written) {
    zassert_equal(defy_radio_charger_service(DEVICE_GET(missing_bus)), -ENODEV);
    zassert_equal(transfer_count, 0);
    const uint8_t ids[] = {0, 0x50, 0x93, 0x98, 0xff};
    for (size_t i = 0; i < ARRAY_SIZE(ids); i++) {
        registers[3] = ids[i];
        clear_transfers();
        zassert_equal(defy_radio_charger_service(bus), -ENODEV);
        zassert_equal(transfer_count, 1);
        zassert_false(transfers[0].write);
    }
}

ZTEST(defy_radio_charger, test_locked_lower_safety_current_is_respected) {
    for (uint8_t code = 0; code < 8; code++) {
        before(NULL);
        safety_locked = true;
        registers[6] = code << 4;
        zassert_ok(defy_radio_charger_service(bus));
        zassert_equal(registers[6], code << 4);
        zassert_equal(registers[4] & 0x7f, (code << 4) | 9);
        zassert_equal(registers[2], 0x8c);
        clear_transfers();
        zassert_ok(defy_radio_charger_service(bus));
        zassert_equal(enables, 0);
        zassert_equal(watchdog_feeds, 1);
    }
}

ZTEST(defy_radio_charger, test_unsafe_locked_voltage_limit_disables_without_watchdog_feed) {
    for (uint8_t code = 1; code < 16; code++) {
        before(NULL);
        safety_locked = true;
        registers[6] = 0x70 | code;
        zassert_equal(defy_radio_charger_service(bus), -ERANGE);
        zassert_true(registers[1] & 4);
        zassert_equal(enables, 0);
        zassert_equal(watchdog_feeds, 0);
    }
}

/*
 * Settings change when the charger's watchdog expires, as in docs/battery.md's
 * "Charging stops when servicing stops", or when the charger loses power.
 */
ZTEST(defy_radio_charger, test_each_corrupted_setting_is_repaired) {
    const uint8_t regs[] = {1, 2, 4, 5};
    for (size_t i = 0; i < ARRAY_SIZE(regs); i++) {
        before(NULL);
        zassert_ok(defy_radio_charger_service(bus));
        registers[regs[i]] ^= 0x04;
        clear_transfers();
        zassert_ok(defy_radio_charger_service(bus));
        zassert_equal(enables, 1);
        zassert_equal(registers[2], 0x8c);
        zassert_equal(registers[1], 0xc8);
    }
}

ZTEST(defy_radio_charger, test_failed_transfer_stops_initialization_and_disables_known_device) {
    zassert_ok(defy_radio_charger_service(bus));
    size_t count = transfer_count;
    for (size_t failed = 0; failed < count; failed++) {
        for (int accepted = 0; accepted < 2; accepted++) {
            before(NULL);
            failure_at = failed;
            failure_after_write = accepted;
            zassert_equal(defy_radio_charger_service(bus), -ETIMEDOUT);
            if (failed == 0) {
                zassert_equal(transfer_count, 1);
            } else {
                zassert_true(registers[1] & 4);
                zassert_equal(transfer_count, failed + 2);
                zassert_equal(transfers[transfer_count - 1].reg, 1);
                zassert_equal(transfers[transfer_count - 1].value, 0xcc);
            }
            failure_at = -1;
            clear_transfers();
            zassert_ok(defy_radio_charger_service(bus));
            zassert_equal(registers[1], 0xc8);
        }
    }
}

ZTEST(defy_radio_charger, test_verification_error_never_feeds_watchdog) {
    for (int failed = 1; failed < 7; failed++) {
        before(NULL);
        zassert_ok(defy_radio_charger_service(bus));
        clear_transfers();
        failure_at = failed;
        zassert_equal(defy_radio_charger_service(bus), -ETIMEDOUT);
        zassert_equal(watchdog_feeds, 0);
        zassert_true(registers[1] & 4);
        zassert_equal(transfer_count, failed + 2);
    }
}

ZTEST(defy_radio_charger, test_ignored_writes_cannot_enable_unverified_charging) {
    const uint8_t regs[] = {1, 2, 4, 5};
    for (size_t i = 0; i < ARRAY_SIZE(regs); i++) {
        before(NULL);
        ignored_register = regs[i];
        zassert_equal(defy_radio_charger_service(bus), -EBADMSG);
        zassert_equal(enables, 0);
        zassert_equal(watchdog_feeds, 0);
    }
}

ZTEST_SUITE(defy_radio_charger, NULL, NULL, before, NULL, NULL);
