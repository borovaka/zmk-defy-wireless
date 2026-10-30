/*
 * Keeps the battery charger configured as the stock firmware does, and feeds
 * its watchdog while the half is awake.
 *
 * SPDX-License-Identifier: MIT
 */

#include <errno.h>
#include <stdint.h>

#include <zephyr/drivers/i2c.h>

#include <dygma/defy_battery.h>

#include "charger.h"

#define STOCK_SAFETY 0x70
#define STOCK_SPECIAL 0x44
#define STOCK_VOLTAGE 0x8c
#define STOCK_CONTROL 0xc8
#define STOCK_CURRENT_BITS 0x09
#define CHARGE_DISABLE 0x04
#define WATCHDOG_RESET_AND_STAT 0xc0

/* The bits each check compares. */
#define ALL_BITS 0xff
#define SPECIAL_MASK 0x67
#define CURRENT_MASK 0x7f

static int read_register(const struct device *bus, uint8_t reg, uint8_t *value) {
    return i2c_write_read(bus, DEFY_CHARGER_ADDRESS, &reg, 1, value, 1);
}

static int write_register(const struct device *bus, uint8_t reg, uint8_t value) {
    uint8_t bytes[] = {reg, value};

    return i2c_write(bus, bytes, sizeof(bytes), DEFY_CHARGER_ADDRESS);
}

static int verify_register(const struct device *bus, uint8_t reg, uint8_t value, uint8_t mask) {
    uint8_t actual;
    int err = read_register(bus, reg, &actual);

    if (err != 0) {
        return err;
    }
    return (actual & mask) == value ? 0 : -EBADMSG;
}

static int write_verified(const struct device *bus, uint8_t reg, uint8_t value, uint8_t mask) {
    int err = write_register(bus, reg, value);

    return err != 0 ? err : verify_register(bus, reg, value, mask);
}

static int read_safety(const struct device *bus, uint8_t *safety) {
    int err = read_register(bus, DEFY_CHARGER_REG_SAFETY, safety);

    if (err != 0) {
        return err;
    }
    return (*safety & ~STOCK_SAFETY) == 0 ? 0 : -ERANGE;
}

static uint8_t charge_current(uint8_t safety) {
    return (safety & STOCK_SAFETY) | STOCK_CURRENT_BITS;
}

static int configuration_matches(const struct device *bus) {
    uint8_t safety;
    int err = read_safety(bus, &safety);

    if (err == 0) {
        err = verify_register(bus, DEFY_CHARGER_REG_CONTROL, STOCK_CONTROL, ALL_BITS);
    }
    if (err == 0) {
        err = verify_register(bus, DEFY_CHARGER_REG_SPECIAL, STOCK_SPECIAL, SPECIAL_MASK);
    }
    if (err == 0) {
        err = verify_register(bus, DEFY_CHARGER_REG_CURRENT, charge_current(safety), CURRENT_MASK);
    }
    if (err == 0) {
        err = verify_register(bus, DEFY_CHARGER_REG_VOLTAGE, STOCK_VOLTAGE, ALL_BITS);
    }
    return err;
}

static int configure(const struct device *bus) {
    uint8_t safety;
    /* Twice, as the stock firmware writes it. */
    int err = write_register(bus, DEFY_CHARGER_REG_SAFETY, STOCK_SAFETY);

    if (err == 0) {
        err = write_register(bus, DEFY_CHARGER_REG_SAFETY, STOCK_SAFETY);
    }
    if (err == 0) {
        err = read_safety(bus, &safety);
    }
    if (err == 0) {
        err = write_verified(bus, DEFY_CHARGER_REG_CONTROL, STOCK_CONTROL | CHARGE_DISABLE,
                             ALL_BITS);
    }
    if (err == 0) {
        err = write_verified(bus, DEFY_CHARGER_REG_SPECIAL, STOCK_SPECIAL, SPECIAL_MASK);
    }
    if (err == 0) {
        err = write_verified(bus, DEFY_CHARGER_REG_CURRENT, charge_current(safety), CURRENT_MASK);
    }
    if (err == 0) {
        err = write_verified(bus, DEFY_CHARGER_REG_VOLTAGE, STOCK_VOLTAGE, ALL_BITS);
    }
    if (err == 0) {
        err = write_register(bus, DEFY_CHARGER_REG_STATUS, WATCHDOG_RESET_AND_STAT);
    }
    if (err == 0) {
        err = write_verified(bus, DEFY_CHARGER_REG_CONTROL, STOCK_CONTROL, ALL_BITS);
    }
    return err;
}

int defy_radio_charger_service(const struct device *bus) {
    if (!device_is_ready(bus)) {
        return -ENODEV;
    }

    uint8_t identity;
    int err = read_register(bus, DEFY_CHARGER_REG_ID, &identity);

    if (err != 0) {
        return err;
    }
    if ((identity & DEFY_BATTERY_CHARGER_ID_MASK) != DEFY_BATTERY_CHARGER_ID_VALUE) {
        return -ENODEV;
    }

    err = configuration_matches(bus);
    if (err == 0) {
        err = write_register(bus, DEFY_CHARGER_REG_STATUS, WATCHDOG_RESET_AND_STAT);
    } else if (err == -EBADMSG || err == -ERANGE) {
        err = configure(bus);
    }
    if (err != 0) {
        write_register(bus, DEFY_CHARGER_REG_CONTROL, STOCK_CONTROL | CHARGE_DISABLE);
    }
    return err;
}
