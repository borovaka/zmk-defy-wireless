/*
 * The halves' version report outside the links: a half's own report from its
 * seal and build, and the Neuron's records with HALF VERSIONS's payload.
 *
 * SPDX-License-Identifier: MIT
 */

#include <errno.h>
#include <string.h>

#include <zephyr/kernel.h>
#include <zephyr/storage/flash_map.h>
#include <zephyr/sys/byteorder.h>
#include <zephyr/sys/crc.h>
#include <zephyr/ztest.h>

#include <dygma/defy_control.h>
#include <dygma/defy_half_package.h>
#include <dygma/defy_half_version.h>
#include <dygma/defy_wired_protocol.h>

#include "fake_uptime.h"

#include "../../../../battery/defy_half_versions.c"
#undef k_uptime_get
#include "../../../../battery/defy_half_version_local.c"

/* The seal partition, as flash_area_*() give it to the half. */
static const struct flash_area seal_area;
static uint8_t seal[DEFY_HALF_SEAL_SIZE];
static int open_error;
static int read_error;
static unsigned int opens;
static unsigned int closes;

int flash_area_open(uint8_t id, const struct flash_area **area) {
    zassert_equal(id, FIXED_PARTITION_ID(factory_seal));
    opens++;
    if (open_error) {
        return open_error;
    }
    *area = &seal_area;
    return 0;
}

int flash_area_read(const struct flash_area *area, off_t offset, void *dst, size_t length) {
    zassert_equal_ptr(area, &seal_area);
    zassert_equal(offset, 0);
    zassert_equal(length, DEFY_HALF_SEAL_SIZE);
    if (read_error) {
        return read_error;
    }
    memcpy(dst, seal, length);
    return 0;
}

void flash_area_close(const struct flash_area *area) {
    zassert_equal_ptr(area, &seal_area);
    closes++;
}

/* A valid seal for a program with this CRC32 and version. */
static void make_seal(uint32_t program_crc, uint32_t version) {
    sys_put_le32(1, seal);
    sys_put_le32(DEFY_HALF_SEAL_SIZE, seal + 4);
    sys_put_le32(0, seal + 8);
    sys_put_le32(DEFY_HALF_PROGRAM_START, seal + 12);
    sys_put_le32(0x24000, seal + 16);
    sys_put_le32(program_crc, seal + 20);
    sys_put_le32(version, seal + 24);
    sys_put_le32(crc32_ieee(seal, sizeof(seal)), seal + 8);
}

static const struct defy_half_version left = {
    .side = DEFY_WIRED_LEFT, .program_crc = 0x11111111, .seal_version = 1, .module = "v0.1.0",
};
static const struct defy_half_version right = {
    .side = DEFY_WIRED_RIGHT, .program_crc = 0x22222222, .seal_version = 2,
    .module = "v0.1.0-4-gabcdef0-",
};

static void reset(void *fixture) {
    ARG_UNUSED(fixture);
    memset(held, 0, sizeof(held));
    now = 0;
    make_seal(0xdeadbeef, 5);
    open_error = read_error = 0;
    opens = closes = 0;
}

static uint8_t payload[DEFY_HALF_VERSIONS_SIZE];

static void read_payload(void) {
    memset(payload, 0xa5, sizeof(payload));
    zassert_equal(defy_half_versions(payload, sizeof(payload)), DEFY_HALF_VERSIONS_SIZE);
    zassert_equal(payload[0], DEFY_HALF_VERSIONS_VERSION);
    zassert_equal(payload[1], 2);
    zassert_equal(sys_get_le16(payload + 2), DEFY_HALF_VERSIONS_RECORD);
    zassert_equal(sys_get_le32(payload + 4), (uint32_t)now);
}

static const uint8_t *entry(uint8_t side) {
    return payload + DEFY_HALF_VERSIONS_HEADER + side * DEFY_HALF_VERSIONS_RECORD;
}

/* The side's record in the payload: the link's bit, the age and the report. */
static void assert_entry(uint8_t side, uint8_t link_mask, uint32_t age,
                         const struct defy_half_version *version) {
    const uint8_t *record = entry(side);
    zassert_equal(record[0], side);
    zassert_equal(record[1], link_mask);
    zassert_equal(record[2], 0);
    zassert_equal(record[3], 0);
    zassert_equal(sys_get_le32(record + 4), age);
    uint8_t expected[DEFY_HALF_VERSION_SIZE] = {0};
    if (version) {
        defy_half_version_encode(version, expected);
    }
    zassert_mem_equal(record + 8, expected, sizeof(expected));
}

static void assert_none(uint8_t side) { assert_entry(side, 0, UINT32_MAX, NULL); }

ZTEST(defy_half_version, test_local_reports_seal_side_and_first_18_bytes_of_module) {
    struct defy_half_version version;
    memset(&version, 0xa5, sizeof(version));
    defy_half_version_local(&version);
    zassert_equal(version.side, DEFY_WIRED_RIGHT);
    zassert_equal(version.program_crc, 0xdeadbeef);
    zassert_equal(version.seal_version, 5);
    zassert_str_equal(version.module, "v0.1.0-12-g2826cb2");
    zassert_equal(opens, 1);
    zassert_equal(closes, 1);
}

ZTEST(defy_half_version, test_local_without_a_valid_seal_reports_zeros) {
    struct defy_half_version version;

    /* A seal that cannot be read, or whose partition does not open. */
    read_error = -EIO;
    defy_half_version_local(&version);
    zassert_equal(version.program_crc, 0);
    zassert_equal(version.seal_version, 0);
    zassert_equal(version.side, DEFY_WIRED_RIGHT);
    zassert_str_equal(version.module, "v0.1.0-12-g2826cb2");
    read_error = 0;
    open_error = -ENOENT;
    defy_half_version_local(&version);
    zassert_equal(version.program_crc, 0);
    zassert_equal(opens, 2);
    zassert_equal(closes, 1);
    open_error = 0;
    defy_half_version_local(&version);
    zassert_equal(version.program_crc, 0xdeadbeef);
}

ZTEST(defy_half_version, test_payload_without_records) {
    now = 1234;
    read_payload();
    assert_none(DEFY_WIRED_LEFT);
    assert_none(DEFY_WIRED_RIGHT);

    /* Too small an output is left alone. */
    memset(payload, 0x5a, sizeof(payload));
    zassert_equal(defy_half_versions(payload, sizeof(payload) - 1), 0);
    zassert_equal(defy_half_versions(NULL, sizeof(payload)), 0);
    for (size_t i = 0; i < sizeof(payload); i++) {
        zassert_equal(payload[i], 0x5a);
    }
    uint8_t larger[DEFY_HALF_VERSIONS_SIZE + 8];
    memset(larger, 0x5a, sizeof(larger));
    zassert_equal(defy_half_versions(larger, sizeof(larger)), DEFY_HALF_VERSIONS_SIZE);
    zassert_equal(larger[DEFY_HALF_VERSIONS_SIZE], 0x5a);
}

ZTEST(defy_half_version, test_records_report_link_age_and_report) {
    now = 1000;
    defy_half_version_received(DEFY_LINK_WIRED, 1, &left);
    now = 1200;
    defy_half_version_received(DEFY_LINK_BLE, 0, &right);
    now = 1500;
    read_payload();
    assert_entry(DEFY_WIRED_LEFT, BIT(DEFY_LINK_WIRED), 500, &left);
    assert_entry(DEFY_WIRED_RIGHT, BIT(DEFY_LINK_BLE), 300, &right);

    /* A newer report on the same link replaces the record and its age. */
    struct defy_half_version updated = left;
    updated.program_crc = 0x33333333;
    defy_half_version_received(DEFY_LINK_WIRED, 0, &updated);
    now = 1510;
    read_payload();
    assert_entry(DEFY_WIRED_LEFT, BIT(DEFY_LINK_WIRED), 10, &updated);
}

ZTEST(defy_half_version, test_newest_link_wins_and_the_other_holds_when_one_goes_down) {
    struct defy_half_version over_ble = left;
    over_ble.program_crc = 0x44444444;

    now = 100;
    defy_half_version_received(DEFY_LINK_WIRED, 0, &left);
    now = 200;
    defy_half_version_received(DEFY_LINK_BLE, 1, &over_ble);
    read_payload();
    assert_entry(DEFY_WIRED_LEFT, BIT(DEFY_LINK_BLE), 0, &over_ble);
    now = 300;
    defy_half_version_received(DEFY_LINK_WIRED, 0, &left);
    read_payload();
    assert_entry(DEFY_WIRED_LEFT, BIT(DEFY_LINK_WIRED), 0, &left);

    /* The cable goes down: the record over Bluetooth, which still holds. */
    now = 350;
    defy_half_version_wired_down(DEFY_WIRED_LEFT);
    read_payload();
    assert_entry(DEFY_WIRED_LEFT, BIT(DEFY_LINK_BLE), 150, &over_ble);
    assert_none(DEFY_WIRED_RIGHT);
    defy_half_version_ble_down(1);
    read_payload();
    assert_none(DEFY_WIRED_LEFT);

    /* Bluetooth goes down first: the cable's record. */
    defy_half_version_received(DEFY_LINK_WIRED, 0, &left);
    now = 360;
    defy_half_version_received(DEFY_LINK_BLE, 1, &over_ble);
    defy_half_version_ble_down(1);
    read_payload();
    assert_entry(DEFY_WIRED_LEFT, BIT(DEFY_LINK_WIRED), 10, &left);
}

ZTEST(defy_half_version, test_down_drops_only_its_link_and_side) {
    defy_half_version_received(DEFY_LINK_BLE, 0, &left);
    defy_half_version_received(DEFY_LINK_BLE, 1, &right);
    defy_half_version_received(DEFY_LINK_WIRED, 0, &right);

    /* A cable going down leaves Bluetooth's records alone, and the other side's. */
    defy_half_version_wired_down(DEFY_WIRED_LEFT);
    read_payload();
    assert_entry(DEFY_WIRED_LEFT, BIT(DEFY_LINK_BLE), 0, &left);
    defy_half_version_wired_down(DEFY_WIRED_RIGHT);
    read_payload();
    assert_entry(DEFY_WIRED_RIGHT, BIT(DEFY_LINK_BLE), 0, &right);

    /* A Bluetooth slot going down drops the record that came by it alone. */
    defy_half_version_ble_down(1);
    read_payload();
    assert_entry(DEFY_WIRED_LEFT, BIT(DEFY_LINK_BLE), 0, &left);
    assert_none(DEFY_WIRED_RIGHT);
    defy_half_version_ble_down(2);
    defy_half_version_ble_down(0);
    read_payload();
    assert_none(DEFY_WIRED_LEFT);

    /* A half that moved slots: the latest report names the slot. */
    defy_half_version_received(DEFY_LINK_BLE, 0, &left);
    defy_half_version_received(DEFY_LINK_BLE, 1, &left);
    defy_half_version_ble_down(0);
    read_payload();
    assert_entry(DEFY_WIRED_LEFT, BIT(DEFY_LINK_BLE), 0, &left);
    defy_half_version_ble_down(1);
    read_payload();
    assert_none(DEFY_WIRED_LEFT);
}

ZTEST(defy_half_version, test_invalid_reports_and_sides_are_ignored) {
    struct defy_half_version third = left;
    third.side = 2;

    defy_half_version_received(DEFY_LINK_BLE + 1, 0, &left);
    defy_half_version_received(DEFY_LINK_WIRED, 0, &third);
    defy_half_version_received(DEFY_LINK_WIRED, 0, NULL);
    defy_half_version_wired_down(2);
    read_payload();
    assert_none(DEFY_WIRED_LEFT);
    assert_none(DEFY_WIRED_RIGHT);
}

ZTEST(defy_half_version, test_age_stops_short_of_no_record) {
    defy_half_version_received(DEFY_LINK_WIRED, 0, &left);
    now = (int64_t)UINT32_MAX + 5;
    read_payload();
    assert_entry(DEFY_WIRED_LEFT, BIT(DEFY_LINK_WIRED), UINT32_MAX - 1, &left);
    now = (int64_t)UINT32_MAX - 1;
    read_payload();
    assert_entry(DEFY_WIRED_LEFT, BIT(DEFY_LINK_WIRED), UINT32_MAX - 1, &left);
    now = 7;
    read_payload();
    assert_entry(DEFY_WIRED_LEFT, BIT(DEFY_LINK_WIRED), 7, &left);
}

ZTEST_SUITE(defy_half_version, NULL, NULL, reset, NULL, NULL);
