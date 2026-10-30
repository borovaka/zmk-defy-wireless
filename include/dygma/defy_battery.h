/* SPDX-License-Identifier: MIT */

#ifndef DYGMA_DEFY_BATTERY_H
#define DYGMA_DEFY_BATTERY_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/*
 * A battery frame is a 'DB' cable frame (dygma/defy_wired_frame.h), little-endian:
 *
 *    0  'D', 'B'
 *    2  DEFY_BATTERY_VERSION
 *    3  the side, enum defy_wired_side
 *    4  the session, 32 bits, never 0
 *    8  the sample's sequence, 32 bits, never 0
 *   12  the sample's age in seconds, rounded up, at most 255
 *   13  the status, enum defy_battery_status
 *   14  the eight words, 16 bits each
 *   30  the CRC16
 */
#define DEFY_BATTERY_VERSION 1u
#define DEFY_BATTERY_WORD_COUNT 8u
#define DEFY_BATTERY_HCI_PAYLOAD_SIZE 28u
#define DEFY_BATTERY_FRAME_SIZE 32u
#define DEFY_BATTERY_MAX_AGE_MS 90000u

#define DEFY_BATTERY_FRAME_VERSION_OFFSET 2u
#define DEFY_BATTERY_FRAME_SIDE_OFFSET 3u
#define DEFY_BATTERY_FRAME_SESSION_OFFSET 4u
#define DEFY_BATTERY_FRAME_SEQUENCE_OFFSET 8u
#define DEFY_BATTERY_FRAME_AGE_OFFSET 12u
#define DEFY_BATTERY_FRAME_STATUS_OFFSET 13u
#define DEFY_BATTERY_FRAME_WORDS_OFFSET 14u

/* The gauge's registers, in a DEFY_BATTERY_SAMPLE_OK sample. */
enum defy_battery_word {
    DEFY_BATTERY_DEVNAME,
    DEFY_BATTERY_STATUS,
    DEFY_BATTERY_FSTAT,
    DEFY_BATTERY_MODELCFG,
    DEFY_BATTERY_REPSOC,
    DEFY_BATTERY_VCELL,
    DEFY_BATTERY_CURRENT,
    DEFY_BATTERY_DESIGNCAP,
};

/* The gauge's DevName and DesignCap, which a gauge sample must hold. */
#define DEFY_BATTERY_GAUGE_DEVNAME 0x4031u
#define DEFY_BATTERY_GAUGE_DESIGNCAP 0x1284u

enum defy_battery_status {
    DEFY_BATTERY_SAMPLE_OK = 0,
    DEFY_BATTERY_SAMPLE_NO_DEVICE = 1,
    DEFY_BATTERY_SAMPLE_IO_ERROR = 2,
    DEFY_BATTERY_SAMPLE_I2C_DIAGNOSTIC = 3,
    DEFY_BATTERY_SAMPLE_ANALOG_OK = 4,
    DEFY_BATTERY_SAMPLE_ANALOG_ERROR = 5,
};

#define DEFY_BATTERY_ANALOG_CHARGER_TAG 0xa102u
#define DEFY_BATTERY_ANALOG_FILTERED_TAG 0xa103u
#define DEFY_BATTERY_LEVEL_UNAVAILABLE 255u
#define DEFY_BATTERY_ANALOG_REFERENCE_2V4 2u
#define DEFY_BATTERY_ANALOG_REFERENCE_VDD 4u

enum defy_battery_analog_word {
    DEFY_BATTERY_ANALOG_FORMAT,
    DEFY_BATTERY_ANALOG_THRESHOLD,
    DEFY_BATTERY_ANALOG_REFERENCE,
    DEFY_BATTERY_ANALOG_MILLIVOLTS,
    /* The charger's registers, two to a word, low byte first, up to the low byte of
     * DEFY_BATTERY_ANALOG_CHARGER_VALID. */
    DEFY_BATTERY_ANALOG_CHARGER = 4,
    /* Its high byte: BIT(register) for each register read. */
    DEFY_BATTERY_ANALOG_CHARGER_VALID = 7,
    /* The negative errno of a DEFY_BATTERY_SAMPLE_ANALOG_ERROR sample, in the threshold's word. */
    DEFY_BATTERY_ANALOG_ERRNO = DEFY_BATTERY_ANALOG_THRESHOLD,
};

enum defy_battery_source {
    DEFY_BATTERY_SOURCE_UNKNOWN,
    DEFY_BATTERY_SOURCE_GAUGE,
    DEFY_BATTERY_SOURCE_ANALOG,
};

/* The FAN5405's registers 0x00..0x06, in an analogue sample from word
 * DEFY_BATTERY_ANALOG_CHARGER. */
enum defy_battery_charger_register {
    DEFY_BATTERY_CHARGER_ID,
    DEFY_BATTERY_CHARGER_STATUS,
    DEFY_BATTERY_CHARGER_CONTROL,
    DEFY_BATTERY_CHARGER_VOLTAGE,
    DEFY_BATTERY_CHARGER_CURRENT,
    DEFY_BATTERY_CHARGER_SPECIAL,
    DEFY_BATTERY_CHARGER_SAFETY,
    DEFY_BATTERY_CHARGER_REGISTER_COUNT,
};

enum defy_battery_charge_state {
    DEFY_BATTERY_CHARGE_READY = 0,
    DEFY_BATTERY_CHARGE_CHARGING = 1,
    DEFY_BATTERY_CHARGE_DONE = 2,
    DEFY_BATTERY_CHARGE_FAULT = 3,
    DEFY_BATTERY_CHARGE_UNKNOWN = 255,
};

/* The fault field in sleep mode: no input power, the normal state on battery. */
#define DEFY_BATTERY_CHARGER_FAULT_SLEEP 2u

#define DEFY_BATTERY_CHARGER_VALID_MASK 0x7fu
#define DEFY_BATTERY_CHARGER_ID_MASK 0xfcu
#define DEFY_BATTERY_CHARGER_ID_VALUE 0x94u

#define DEFY_BATTERY_I2C_DIAGNOSTIC_TAG 0xd101u
#define DEFY_BATTERY_I2C_CHARGER_TAG 0xd102u
#define DEFY_BATTERY_I2C_NO_EVENT 0xffffu
#define DEFY_BATTERY_I2C_PACKED_UNKNOWN 0xeu
#define DEFY_BATTERY_I2C_PACKED_NO_EVENT 0xfu
#define DEFY_BATTERY_I2C_NO_VALUE 0xffffu
#define DEFY_BATTERY_I2C_SDA_HIGH (1u << 10)
#define DEFY_BATTERY_I2C_SCL_HIGH (1u << 11)
#define DEFY_BATTERY_I2C_ENABLED (1u << 12)
#define DEFY_BATTERY_I2C_SDA_DISCONNECTED (1u << 13)
#define DEFY_BATTERY_I2C_SCL_DISCONNECTED (1u << 14)

enum defy_battery_i2c_diagnostic_word {
    DEFY_BATTERY_I2C_TAG,
    DEFY_BATTERY_I2C_REGISTER_EVENTS,
    DEFY_BATTERY_I2C_ERRNO,
    DEFY_BATTERY_I2C_ELAPSED_LOW,
    DEFY_BATTERY_I2C_ELAPSED_HIGH,
    DEFY_BATTERY_I2C_EVENT_TYPE,
    DEFY_BATTERY_I2C_PINS,
    DEFY_BATTERY_I2C_PIN_CNF_LOW8,
};

enum defy_battery_i2c_charger_word {
    DEFY_BATTERY_I2C_CHARGER_FORMAT,
    DEFY_BATTERY_I2C_PROBE_REGISTERS,
    DEFY_BATTERY_I2C_GAUGE_ERRNO,
    DEFY_BATTERY_I2C_GAUGE_TRACE,
    DEFY_BATTERY_I2C_CHARGER_ERRNO,
    DEFY_BATTERY_I2C_CHARGER_VALUE,
    DEFY_BATTERY_I2C_CHARGER_TRACE,
    DEFY_BATTERY_I2C_PROBE_ELAPSED_MS,
};

struct defy_battery_sample {
    uint32_t sequence;
    uint32_t age_ms;
    uint8_t status;
    uint16_t words[DEFY_BATTERY_WORD_COUNT];
};

struct defy_battery_frame {
    uint8_t side;
    uint32_t session;
    struct defy_battery_sample sample;
};

struct defy_battery_reading {
    enum defy_battery_source source;
    uint8_t level;
    uint16_t millivolts;
};

struct defy_battery_charger {
    enum defy_battery_charge_state state;
    uint8_t fault;
    bool boost;
    bool status_valid;
};

int defy_battery_analog_mv(uint16_t threshold, uint16_t reference, uint16_t *millivolts);
int defy_battery_sample_reading(const struct defy_battery_sample *sample,
                                struct defy_battery_reading *reading);
int defy_battery_sample_charger(const struct defy_battery_sample *sample,
                                struct defy_battery_charger *charger);
int defy_battery_frame_decode(const uint8_t *data, size_t length,
                              struct defy_battery_frame *frame);
int defy_battery_frame_encode(const struct defy_battery_frame *frame,
                              uint8_t data[DEFY_BATTERY_FRAME_SIZE]);

#endif
