/*
 * The battery charger on I2C, which battery.c samples and charger.c keeps
 * configured as the stock firmware does.
 *
 * SPDX-License-Identifier: MIT
 */

#ifndef DEFY_RADIO_CHARGER_H
#define DEFY_RADIO_CHARGER_H

#include <zephyr/device.h>

#define DEFY_CHARGER_ADDRESS 0x6a

#define DEFY_CHARGER_REG_STATUS 0x00
#define DEFY_CHARGER_REG_CONTROL 0x01
#define DEFY_CHARGER_REG_VOLTAGE 0x02
#define DEFY_CHARGER_REG_ID 0x03
#define DEFY_CHARGER_REG_CURRENT 0x04
#define DEFY_CHARGER_REG_SPECIAL 0x05
#define DEFY_CHARGER_REG_SAFETY 0x06

#define DEFY_CHARGER_SERVICE_INTERVAL_MS 5000

int defy_radio_charger_service(const struct device *bus);

#endif
