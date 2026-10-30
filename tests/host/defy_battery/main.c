/* SPDX-License-Identifier: MIT */

#include <dygma/defy_battery.h>
#include <assert.h>
#include <errno.h>
#include <string.h>

static struct defy_battery_frame sample(void) {
    return (struct defy_battery_frame){
        .side = 1, .session = 0x12345678,
        .sample = {.sequence = 0x87654321, .age_ms = 1001,
                   .words = {0x4031, 2, 0, 0, 50 * 256 + 128, 51200, 0xfff0, 0x1284}},
    };
}

static void test_readings(void) {
    struct defy_battery_frame frame = sample();
    struct defy_battery_reading reading;
    assert(defy_battery_sample_reading(&frame.sample, &reading) == 0);
    assert(reading.level == 50 && reading.millivolts == 4000);
    assert(reading.source == DEFY_BATTERY_SOURCE_GAUGE);
    frame.sample.words[DEFY_BATTERY_REPSOC] = 0;
    assert(defy_battery_sample_reading(&frame.sample, &reading) == 0 && reading.level == 0);
    frame.sample.words[DEFY_BATTERY_REPSOC] = 100 * 256;
    assert(defy_battery_sample_reading(&frame.sample, &reading) == 0 && reading.level == 100);
    frame.sample.words[DEFY_BATTERY_REPSOC]++;
    assert(defy_battery_sample_reading(&frame.sample, &reading) == -ERANGE);
    const unsigned indices[] = {DEFY_BATTERY_DEVNAME, DEFY_BATTERY_DESIGNCAP,
                                DEFY_BATTERY_FSTAT, DEFY_BATTERY_MODELCFG,
                                DEFY_BATTERY_VCELL};
    const unsigned bad[] = {0x4032, 0, 1, 0x8000, 0xffff};
    const int errors[] = {-ENODEV, -ENODEV, -EAGAIN, -EAGAIN, -ERANGE};
    for (size_t i = 0; i < sizeof(indices) / sizeof(indices[0]); i++) {
        frame = sample();
        frame.sample.words[indices[i]] = bad[i];
        assert(defy_battery_sample_reading(&frame.sample, &reading) == errors[i]);
    }
    frame = sample();
    frame.sample.age_ms = DEFY_BATTERY_MAX_AGE_MS - 1;
    assert(defy_battery_sample_reading(&frame.sample, &reading) == 0);
    frame.sample.age_ms++;
    assert(defy_battery_sample_reading(&frame.sample, &reading) == -ESTALE);
    frame = sample();
    frame.sample.status = DEFY_BATTERY_SAMPLE_IO_ERROR;
    assert(defy_battery_sample_reading(&frame.sample, &reading) == -ENODATA);
    frame.sample.status = DEFY_BATTERY_SAMPLE_I2C_DIAGNOSTIC;
    assert(defy_battery_sample_reading(&frame.sample, &reading) == -ENODATA);
}

static void test_frames(void) {
    struct defy_battery_frame frame = sample(), decoded;
    uint8_t data[DEFY_BATTERY_FRAME_SIZE];
    assert(defy_battery_frame_encode(&frame, data) == 0);
    assert(data[0] == 'D' && data[1] == 'B' && data[4] == 0x78 && data[12] == 2);
    assert(defy_battery_frame_decode(data, sizeof(data), &decoded) == 0);
    assert(decoded.side == 1 && decoded.session == frame.session);
    assert(decoded.sample.sequence == frame.sample.sequence && decoded.sample.age_ms == 2000);
    assert(memcmp(decoded.sample.words, frame.sample.words, sizeof(frame.sample.words)) == 0);
    frame.sample.age_ms = UINT32_MAX;
    assert(defy_battery_frame_encode(&frame, data) == 0 && data[12] == 255);
    assert(defy_battery_frame_decode(data, sizeof(data), &decoded) == 0);
    assert(decoded.sample.age_ms == 255000);
    frame.sample.status = DEFY_BATTERY_SAMPLE_IO_ERROR;
    assert(defy_battery_frame_encode(&frame, data) == 0);
    assert(defy_battery_frame_decode(data, sizeof(data), &decoded) == 0);
    assert(decoded.sample.status == DEFY_BATTERY_SAMPLE_IO_ERROR);
    frame.sample.status = DEFY_BATTERY_SAMPLE_I2C_DIAGNOSTIC;
    frame.sample.words[0] = DEFY_BATTERY_I2C_DIAGNOSTIC_TAG;
    assert(defy_battery_frame_encode(&frame, data) == 0);
    assert(defy_battery_frame_decode(data, sizeof(data), &decoded) == 0);
    assert(decoded.sample.status == DEFY_BATTERY_SAMPLE_I2C_DIAGNOSTIC);
    assert(decoded.sample.words[0] == DEFY_BATTERY_I2C_DIAGNOSTIC_TAG);
    frame.sample.status = DEFY_BATTERY_SAMPLE_ANALOG_ERROR + 1;
    assert(defy_battery_frame_encode(&frame, data) == -EINVAL);
    frame = sample(); frame.session = 0;
    assert(defy_battery_frame_encode(&frame, data) == -EINVAL);
    frame = sample(); frame.side = 2;
    assert(defy_battery_frame_encode(&frame, data) == -EINVAL);
    frame = sample(); frame.sample.sequence = 0;
    assert(defy_battery_frame_encode(&frame, data) == -EINVAL);
}

static void test_charger_diagnostics(void) {
    const uint16_t details[][DEFY_BATTERY_WORD_COUNT] = {
        {DEFY_BATTERY_I2C_CHARGER_TAG, 0x0321, (uint16_t)-EIO, 0x101, 0, 0x94, 0x210, 0x0101},
        {DEFY_BATTERY_I2C_CHARGER_TAG, 0x0321, (uint16_t)-EIO, 0x101,
         (uint16_t)-EIO, DEFY_BATTERY_I2C_NO_VALUE, 0x0ff, 0xff01},
        {DEFY_BATTERY_I2C_DIAGNOSTIC_TAG, 0x121, (uint16_t)-EIO, 244, 0, 1, 0x1ca4, 0x0c0c},
    };
    for (size_t i = 0; i < sizeof(details) / sizeof(details[0]); i++) {
        struct defy_battery_frame frame = sample(), decoded;
        struct defy_battery_reading reading = {.level = 123};
        uint8_t data[DEFY_BATTERY_FRAME_SIZE];
        frame.sample.status = DEFY_BATTERY_SAMPLE_I2C_DIAGNOSTIC;
        memcpy(frame.sample.words, details[i], sizeof(frame.sample.words));
        assert(defy_battery_frame_encode(&frame, data) == 0);
        assert(defy_battery_frame_decode(data, sizeof(data), &decoded) == 0);
        assert(decoded.sample.status == DEFY_BATTERY_SAMPLE_I2C_DIAGNOSTIC);
        assert(memcmp(decoded.sample.words, details[i], sizeof(decoded.sample.words)) == 0);
        assert(defy_battery_sample_reading(&decoded.sample, &reading) == -ENODATA);
        assert(reading.level == 123);
    }
}

/* An unfiltered analog sample, as the radio's filter reads it, with the charger's identity alone. */
static struct defy_battery_frame analog(uint16_t threshold, uint16_t reference) {
    struct defy_battery_frame frame = {
        .side = 0, .session = 7,
        .sample = {.sequence = 8, .status = DEFY_BATTERY_SAMPLE_ANALOG_OK,
                   .words = {DEFY_BATTERY_ANALOG_CHARGER_TAG, threshold, reference, 0, 0x0094, 0,
                             0, 0x0100}},
    };
    assert(defy_battery_analog_mv(threshold, reference,
                                  &frame.sample.words[DEFY_BATTERY_ANALOG_MILLIVOLTS]) == 0);
    return frame;
}

static void test_analog_conversion_and_range(void) {
    uint16_t millivolts;
    assert(defy_battery_analog_mv(62, 2, &millivolts) == 0 && millivolts == 3529);
    assert(defy_battery_analog_mv(62, 4, &millivolts) == 0 && millivolts == 4676);
    assert(defy_battery_analog_mv(63, 2, &millivolts) == 0 && millivolts == 3585);
    assert(defy_battery_analog_mv(64, 2, &millivolts) == -ERANGE && millivolts == 3585);
    assert(defy_battery_analog_mv(62, 3, &millivolts) == -EINVAL && millivolts == 3585);
    for (uint16_t reference = 2; reference <= 4; reference += 2) {
        uint8_t previous = 0;
        for (uint16_t threshold = 0; threshold < 64; threshold++) {
            struct defy_battery_frame frame = analog(threshold, reference);
            struct defy_battery_reading reading = {.level = 123};
            uint16_t mv = frame.sample.words[DEFY_BATTERY_ANALOG_MILLIVOLTS];
            int err = defy_battery_sample_reading(&frame.sample, &reading);
            if (mv < 2000 || mv > 4600 || (reference == 4 && threshold == 63)) {
                assert(err == -ERANGE && reading.level == 123);
            } else {
                assert(err == 0);
                assert(reading.source == DEFY_BATTERY_SOURCE_ANALOG);
                assert(reading.millivolts == mv && reading.level >= 1 && reading.level <= 100);
                /* The percentage never falls as the voltage rises. */
                assert(reading.level >= previous);
                previous = reading.level;
            }
        }
    }
}

static void test_analog_validation(void) {
    /* Word 0 must name a format with the charger's registers: not 0, nor 0xa101 without them. */
    const uint16_t changes[][2] = {
        {0, 0}, {0, 0xa101}, {1, 64}, {1, 256 + 51}, {2, 3}, {2, 256 + 4}, {3, 3859},
        {4, 0x0194}, {5, 1}, {6, 1}, {7, 1},
    };
    for (size_t i = 0; i < sizeof(changes) / sizeof(changes[0]); i++) {
        struct defy_battery_frame frame = analog(51, 4);
        struct defy_battery_reading reading = {.level = 123};
        frame.sample.words[changes[i][0]] = changes[i][1];
        assert(defy_battery_sample_reading(&frame.sample, &reading) < 0);
        assert(reading.level == 123);
    }
    struct defy_battery_frame frame = analog(51, 4), decoded;
    struct defy_battery_reading reading;
    assert(defy_battery_sample_reading(&frame.sample, &reading) == 0);
    assert(reading.level == 50 && reading.millivolts == 3858);
    uint8_t bytes[DEFY_BATTERY_FRAME_SIZE];
    assert(defy_battery_frame_encode(&frame, bytes) == 0);
    assert(defy_battery_frame_decode(bytes, sizeof(bytes), &decoded) == 0);
    assert(decoded.sample.status == DEFY_BATTERY_SAMPLE_ANALOG_OK);
    assert(memcmp(decoded.sample.words, frame.sample.words, sizeof(frame.sample.words)) == 0);
    frame.sample.age_ms = DEFY_BATTERY_MAX_AGE_MS;
    assert(defy_battery_sample_reading(&frame.sample, &reading) == -ESTALE);
    frame.sample.age_ms = 0;
    frame.sample.sequence = 0;
    assert(defy_battery_sample_reading(&frame.sample, &reading) == -ENODATA);
    frame.sample.sequence = 1;
    frame.sample.status = DEFY_BATTERY_SAMPLE_ANALOG_ERROR;
    assert(defy_battery_frame_encode(&frame, bytes) == 0);
    assert(defy_battery_frame_decode(bytes, sizeof(bytes), &decoded) == 0);
    assert(decoded.sample.status == DEFY_BATTERY_SAMPLE_ANALOG_ERROR);
    assert(defy_battery_sample_reading(&decoded.sample, &reading) == -ENODATA);
}

static struct defy_battery_frame analog_charger(void) {
    struct defy_battery_frame frame = analog(51, 4);
    frame.sample.words[0] = DEFY_BATTERY_ANALOG_CHARGER_TAG;
    frame.sample.words[4] = 0x1094;
    frame.sample.words[5] = 0x8cc8;
    frame.sample.words[6] = 0x4479;
    frame.sample.words[7] = 0x7f70;
    return frame;
}

static void test_charger_roundtrip_and_unfiltered_percentages(void) {
    struct defy_battery_frame frame = analog_charger(), decoded;
    uint8_t bytes[DEFY_BATTERY_FRAME_SIZE];
    assert(defy_battery_frame_encode(&frame, bytes) == 0);
    assert(defy_battery_frame_decode(bytes, sizeof(bytes), &decoded) == 0);
    assert(memcmp(frame.sample.words, decoded.sample.words, sizeof(frame.sample.words)) == 0);
    struct defy_battery_charger charger;
    assert(defy_battery_sample_charger(&decoded.sample, &charger) == 0);
    assert(charger.status_valid && charger.state == DEFY_BATTERY_CHARGE_CHARGING);
    assert(charger.fault == 0 && !charger.boost);
    struct defy_battery_frame identity = analog(48, 4);
    memcpy(frame.sample.words + 1, identity.sample.words + 1, 3 * sizeof(uint16_t));
    struct defy_battery_reading first = {0}, second = {0};
    assert(defy_battery_sample_reading(&identity.sample, &first) == 0);
    assert(defy_battery_sample_reading(&frame.sample, &second) == 0);
    assert(first.level == 15 && first.level == second.level);
    assert(first.millivolts == second.millivolts);
    assert(defy_battery_sample_charger(&identity.sample, &charger) == 0);
    assert(!charger.status_valid);
}

static void test_charger_status_fields(void) {
    struct defy_battery_frame frame = analog_charger();
    struct defy_battery_charger charger;
    frame.sample.words[4] = 0x2094;
    assert(defy_battery_sample_charger(&frame.sample, &charger) == 0);
    assert(charger.state == DEFY_BATTERY_CHARGE_DONE);
}

static void test_charger_independent_voltage_validation(void) {
    struct defy_battery_frame frame = analog_charger();
    struct defy_battery_reading reading;
    struct defy_battery_charger charger;
    frame.sample.words[1] = 0;
    frame.sample.words[2] = 2;
    frame.sample.words[3] = 55;
    assert(defy_battery_sample_reading(&frame.sample, &reading) == -ERANGE);
    assert(defy_battery_sample_charger(&frame.sample, &charger) == 0);
    assert(charger.state == DEFY_BATTERY_CHARGE_CHARGING);
    frame.sample.status = DEFY_BATTERY_SAMPLE_ANALOG_ERROR;
    frame.sample.words[1] = (uint16_t)-ETIMEDOUT;
    frame.sample.words[2] = 0;
    frame.sample.words[3] = 0;
    assert(defy_battery_sample_reading(&frame.sample, &reading) == -ENODATA);
    assert(defy_battery_sample_charger(&frame.sample, &charger) == 0);
    frame = analog_charger();
    frame.sample.words[4] = 0x93;
    memset(frame.sample.words + 5, 0, 3 * sizeof(uint16_t));
    frame.sample.words[7] = 0x0100;
    assert(defy_battery_sample_charger(&frame.sample, &charger) == -ENODEV);
    assert(defy_battery_sample_reading(&frame.sample, &reading) == 0);
    assert(reading.level == 50);
}

static void expect_charger_error(struct defy_battery_sample *sample, int error) {
    struct defy_battery_charger charger, original;
    memset(&charger, 0x5a, sizeof(charger));
    memcpy(&original, &charger, sizeof(original));
    assert(defy_battery_sample_charger(sample, &charger) == error);
    assert(memcmp(&charger, &original, sizeof(charger)) == 0);
}

static void test_charger_validity_and_freshness(void) {
    struct defy_battery_frame frame = analog_charger();
    struct defy_battery_charger charger;
    const uint8_t raw[] = {0x94, 0x10, 0xc8, 0x8c, 0x79, 0x44, 0x70};
    for (unsigned count = 0; count <= sizeof(raw); count++) {
        memset(frame.sample.words + 4, 0, 4 * sizeof(uint16_t));
        for (unsigned i = 0; i < count; i++) {
            frame.sample.words[4 + i / 2] |= (uint16_t)raw[i] << (8 * (i % 2));
        }
        frame.sample.words[7] |= ((1u << count) - 1u) << 8;
        if (!count) {
            expect_charger_error(&frame.sample, -ENODATA);
        } else {
            assert(defy_battery_sample_charger(&frame.sample, &charger) == 0);
            assert(charger.status_valid == (count > 1));
            assert(charger.state == (count > 1 ? DEFY_BATTERY_CHARGE_CHARGING
                                               : DEFY_BATTERY_CHARGE_UNKNOWN));
        }
    }
    frame = analog_charger();
    frame.sample.age_ms = DEFY_BATTERY_MAX_AGE_MS - 1;
    assert(defy_battery_sample_charger(&frame.sample, &charger) == 0);
    frame.sample.age_ms++;
    expect_charger_error(&frame.sample, -ESTALE);
    frame = analog_charger();
    frame.sample.sequence = 0;
    expect_charger_error(&frame.sample, -ENODATA);
    frame = analog_charger();
    frame.sample.status = DEFY_BATTERY_SAMPLE_OK;
    expect_charger_error(&frame.sample, -ENODATA);
    frame = analog_charger();
    frame.sample.words[7] |= 0x8000;
    expect_charger_error(&frame.sample, -EPROTO);
    frame = analog_charger();
    frame.sample.words[7] &= ~0x0100;
    expect_charger_error(&frame.sample, -EPROTO);
    for (unsigned i = 1; i < sizeof(raw); i++) {
        frame = analog_charger();
        frame.sample.words[7] &= ~(1u << (8 + i));
        expect_charger_error(&frame.sample, -EPROTO);
    }
    frame = analog_charger();
    frame.sample.words[0] = 0xffff;
    expect_charger_error(&frame.sample, -EPROTO);
    expect_charger_error(NULL, -EINVAL);
    assert(defy_battery_sample_charger(&frame.sample, NULL) == -EINVAL);
}

static void test_filtered_reading_preserves_raw_voltage_and_charger(void) {
    struct defy_battery_frame frame = analog_charger(), decoded;
    frame.sample.words[0] = DEFY_BATTERY_ANALOG_FILTERED_TAG;
    frame.sample.words[1] |= 70u << 8;
    frame.sample.words[4] = 0x0294;
    uint8_t bytes[DEFY_BATTERY_FRAME_SIZE];
    assert(defy_battery_frame_encode(&frame, bytes) == 0);
    assert(defy_battery_frame_decode(bytes, sizeof(bytes), &decoded) == 0);
    struct defy_battery_reading reading;
    assert(defy_battery_sample_reading(&decoded.sample, &reading) == 0);
    assert(reading.level == 70 && reading.millivolts == 3858);
    struct defy_battery_charger charger;
    assert(defy_battery_sample_charger(&decoded.sample, &charger) == 0);
    assert(charger.state == DEFY_BATTERY_CHARGE_READY && charger.fault == 2);
    const uint8_t states[] = {0x10, 0x20, 0x31, 0x08, 0x01};
    for (size_t i = 0; i < sizeof(states); i++) {
        frame.sample.words[4] = ((uint16_t)states[i] << 8) | 0x94;
        assert(defy_battery_sample_reading(&frame.sample, &reading) == -EAGAIN);
    }
    frame.sample.words[4] = 0x0094;
    frame.sample.words[1] = 51 | DEFY_BATTERY_LEVEL_UNAVAILABLE << 8;
    assert(defy_battery_sample_reading(&frame.sample, &reading) == -EAGAIN);
    frame.sample.words[1] = 51 | 101u << 8;
    assert(defy_battery_sample_reading(&frame.sample, &reading) == -EPROTO);
    frame.sample.words[1] = 64 | 70u << 8;
    assert(defy_battery_sample_reading(&frame.sample, &reading) == -ERANGE);
    frame.sample.words[1] = 51 | 70u << 8;
    frame.sample.words[3]++;
    assert(defy_battery_sample_reading(&frame.sample, &reading) == -EPROTO);
}

int main(void) {
    test_readings();
    test_frames();
    test_charger_diagnostics();
    test_analog_conversion_and_range();
    test_analog_validation();
    test_charger_roundtrip_and_unfiltered_percentages();
    test_charger_status_fields();
    test_charger_independent_voltage_validation();
    test_charger_validity_and_freshness();
    test_filtered_reading_preserves_raw_voltage_and_charger();
    return 0;
}
