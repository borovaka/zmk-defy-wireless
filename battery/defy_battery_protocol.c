/* SPDX-License-Identifier: MIT */

#include <dygma/defy_battery.h>
#include <dygma/defy_wired_frame.h>
#include <dygma/defy_wired_protocol.h>

#include <errno.h>
#include <string.h>

_Static_assert(DEFY_BATTERY_FRAME_SIZE == DEFY_WIRED_FRAME_SIZE,
               "a battery frame is a cable frame, 'DB'");
_Static_assert(DEFY_BATTERY_FRAME_WORDS_OFFSET + 2u * DEFY_BATTERY_WORD_COUNT ==
                   DEFY_WIRED_CRC_OFFSET,
               "the words end before the CRC");

/* The FAN5405's status register, 0x00. */
#define CHARGER_STAT_SHIFT 4
#define CHARGER_STAT_MASK 0x3u
#define CHARGER_BOOST 0x08u
#define CHARGER_FAULT_MASK 0x07u

/* The comparator: 64 thresholds against a 2.4 V or a VDD reference. The battery reaches the
 * comparator through a 0.68 divider. */
#define ANALOG_THRESHOLD_MAX 63u
#define REFERENCE_2V4_MV 2400u
#define REFERENCE_VDD_MV 3180u
#define DIVIDER_PER_MILLE 680u

/* The cell voltages a reading accepts. */
#define CELL_MIN_MV 2000u
#define CELL_MAX_MV 4600u

/* The gauge: FStat's DNR, data not ready, and ModelCfg's Refresh. */
#define GAUGE_FSTAT_DNR 0x0001u
#define GAUGE_MODELCFG_REFRESH 0x8000u

/* Percent for each of the comparator's 64 thresholds, by reference; docs/battery.md lists the
 * voltages. */
static const uint8_t analog_levels_2v4[64] = {
    1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1,
    1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1,
    1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1,
    1, 1, 1, 1, 1, 1, 1, 1, 1, 2, 2, 4, 6, 6, 8, 10,
};

static const uint8_t analog_levels_vdd[64] = {
    1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1,
    1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1,
    1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 2, 2, 4, 6, 10,
    15, 20, 40, 50, 70, 100, 100, 100, 100, 100, 100, 100, 100, 100, 100, 100,
};

int defy_battery_analog_mv(uint16_t threshold, uint16_t reference, uint16_t *millivolts) {
    if (millivolts == NULL) {
        return -EINVAL;
    }
    if (threshold > ANALOG_THRESHOLD_MAX) {
        return -ERANGE;
    }
    uint32_t reference_mv;
    if (reference == DEFY_BATTERY_ANALOG_REFERENCE_2V4) {
        reference_mv = REFERENCE_2V4_MV;
    } else if (reference == DEFY_BATTERY_ANALOG_REFERENCE_VDD) {
        reference_mv = REFERENCE_VDD_MV;
    } else {
        return -EINVAL;
    }
    *millivolts = (uint16_t)((reference_mv * (threshold + 1u) / ANALOG_THRESHOLD_MAX) * 1000u /
                             DIVIDER_PER_MILLE);
    return 0;
}

/* The charger's register i, packed two to a word from DEFY_BATTERY_ANALOG_CHARGER. */
static uint8_t charger_register(const uint16_t *words, size_t i) {
    return words[DEFY_BATTERY_ANALOG_CHARGER + i / 2] >> (8 * (i % 2));
}

/*
 * Checks an analogue sample's layout: DEFY_BATTERY_ANALOG_FILTERED_TAG as the
 * radio sends it, or DEFY_BATTERY_ANALOG_CHARGER_TAG, the raw layout its
 * filter reads. A charger register outside the valid mask must be zero.
 */
static int analog_format(const struct defy_battery_sample *sample) {
    const uint16_t *words = sample->words;
    if (words[DEFY_BATTERY_ANALOG_FORMAT] != DEFY_BATTERY_ANALOG_CHARGER_TAG &&
        words[DEFY_BATTERY_ANALOG_FORMAT] != DEFY_BATTERY_ANALOG_FILTERED_TAG) {
        return -EPROTO;
    }
    uint8_t valid = words[DEFY_BATTERY_ANALOG_CHARGER_VALID] >> 8;
    if ((valid & ~DEFY_BATTERY_CHARGER_VALID_MASK) ||
        (valid != 0 && !(valid & (1u << DEFY_BATTERY_CHARGER_ID)))) {
        return -EPROTO;
    }
    for (size_t i = 0; i < DEFY_BATTERY_CHARGER_REGISTER_COUNT; i++) {
        if (!(valid & (1u << i)) && charger_register(words, i) != 0) {
            return -EPROTO;
        }
    }
    return 0;
}

int defy_battery_sample_charger(const struct defy_battery_sample *sample,
                                struct defy_battery_charger *charger) {
    if (sample == NULL || charger == NULL) {
        return -EINVAL;
    }
    if (sample->sequence == 0 ||
        (sample->status != DEFY_BATTERY_SAMPLE_ANALOG_OK &&
         sample->status != DEFY_BATTERY_SAMPLE_ANALOG_ERROR)) {
        return -ENODATA;
    }
    if (sample->age_ms >= DEFY_BATTERY_MAX_AGE_MS) {
        return -ESTALE;
    }
    int err = analog_format(sample);
    if (err != 0) {
        return err;
    }
    const uint16_t *words = sample->words;
    uint8_t valid = words[DEFY_BATTERY_ANALOG_CHARGER_VALID] >> 8;
    if (!(valid & (1u << DEFY_BATTERY_CHARGER_ID))) {
        return -ENODATA;
    }
    if ((charger_register(words, DEFY_BATTERY_CHARGER_ID) & DEFY_BATTERY_CHARGER_ID_MASK) !=
        DEFY_BATTERY_CHARGER_ID_VALUE) {
        return -ENODEV;
    }
    struct defy_battery_charger decoded = {
        .state = DEFY_BATTERY_CHARGE_UNKNOWN,
        .status_valid = valid & (1u << DEFY_BATTERY_CHARGER_STATUS),
    };
    if (decoded.status_valid) {
        uint8_t status = charger_register(words, DEFY_BATTERY_CHARGER_STATUS);
        decoded.state = (status >> CHARGER_STAT_SHIFT) & CHARGER_STAT_MASK;
        decoded.fault = status & CHARGER_FAULT_MASK;
        decoded.boost = (status & CHARGER_BOOST) != 0;
    }
    *charger = decoded;
    return 0;
}

static int analog_reading(const struct defy_battery_sample *sample,
                          struct defy_battery_reading *reading) {
    int err = analog_format(sample);
    if (err != 0) {
        return err;
    }
    const uint16_t *words = sample->words;
    uint16_t threshold = words[DEFY_BATTERY_ANALOG_THRESHOLD];
    uint16_t reference = words[DEFY_BATTERY_ANALOG_REFERENCE];
    bool filtered = words[DEFY_BATTERY_ANALOG_FORMAT] == DEFY_BATTERY_ANALOG_FILTERED_TAG;
    uint8_t level = threshold >> 8;
    if (filtered) {
        threshold &= 0xffu;
        if (level > 100 && level != DEFY_BATTERY_LEVEL_UNAVAILABLE) {
            return -EPROTO;
        }
    }
    uint16_t millivolts;
    err = defy_battery_analog_mv(threshold, reference, &millivolts);
    if (err != 0) {
        return err;
    }
    if (words[DEFY_BATTERY_ANALOG_MILLIVOLTS] != millivolts) {
        return -EPROTO;
    }
    if ((reference == DEFY_BATTERY_ANALOG_REFERENCE_VDD && threshold == ANALOG_THRESHOLD_MAX) ||
        millivolts < CELL_MIN_MV || millivolts > CELL_MAX_MV) {
        return -ERANGE;
    }
    if (filtered) {
        struct defy_battery_charger charger;
        err = defy_battery_sample_charger(sample, &charger);
        if (err) {
            return err;
        }
        if (level == DEFY_BATTERY_LEVEL_UNAVAILABLE || !charger.status_valid ||
            charger.boost || charger.state != DEFY_BATTERY_CHARGE_READY ||
            (charger.fault != 0 && charger.fault != DEFY_BATTERY_CHARGER_FAULT_SLEEP)) {
            return -EAGAIN;
        }
    }
    *reading = (struct defy_battery_reading){
        .source = DEFY_BATTERY_SOURCE_ANALOG,
        .level = filtered ? level : reference == DEFY_BATTERY_ANALOG_REFERENCE_2V4
                     ? analog_levels_2v4[threshold] : analog_levels_vdd[threshold],
        .millivolts = millivolts,
    };
    return 0;
}

int defy_battery_sample_reading(const struct defy_battery_sample *sample,
                                struct defy_battery_reading *reading) {
    if (sample == NULL || reading == NULL) {
        return -EINVAL;
    }
    if (sample->sequence == 0 || (sample->status != DEFY_BATTERY_SAMPLE_OK &&
                                sample->status != DEFY_BATTERY_SAMPLE_ANALOG_OK)) {
        return -ENODATA;
    }
    if (sample->age_ms >= DEFY_BATTERY_MAX_AGE_MS) {
        return -ESTALE;
    }
    if (sample->status == DEFY_BATTERY_SAMPLE_ANALOG_OK) {
        return analog_reading(sample, reading);
    }
    const uint16_t *words = sample->words;
    if (words[DEFY_BATTERY_DEVNAME] != DEFY_BATTERY_GAUGE_DEVNAME ||
        words[DEFY_BATTERY_DESIGNCAP] != DEFY_BATTERY_GAUGE_DESIGNCAP) {
        return -ENODEV;
    }
    if ((words[DEFY_BATTERY_FSTAT] & GAUGE_FSTAT_DNR) ||
        (words[DEFY_BATTERY_MODELCFG] & GAUGE_MODELCFG_REFRESH)) {
        return -EAGAIN;
    }
    /* VCell is 78.125 microvolts per bit, RepSOC 1/256 % per bit. */
    uint32_t microvolts = (uint32_t)words[DEFY_BATTERY_VCELL] * 625u / 8u;
    if (words[DEFY_BATTERY_REPSOC] > 100u * 256u || microvolts < CELL_MIN_MV * 1000u ||
        microvolts > CELL_MAX_MV * 1000u) {
        return -ERANGE;
    }
    *reading = (struct defy_battery_reading){
        .source = DEFY_BATTERY_SOURCE_GAUGE,
        .level = (uint8_t)(words[DEFY_BATTERY_REPSOC] / 256u),
        .millivolts = (uint16_t)(microvolts / 1000u),
    };
    return 0;
}

int defy_battery_frame_encode(const struct defy_battery_frame *frame,
                              uint8_t data[DEFY_BATTERY_FRAME_SIZE]) {
    if (frame == NULL || data == NULL || frame->side > DEFY_WIRED_RIGHT || frame->session == 0 ||
        frame->sample.sequence == 0 || frame->sample.status > DEFY_BATTERY_SAMPLE_ANALOG_ERROR) {
        return -EINVAL;
    }
    defy_wired_frame_start(data, 'B');
    data[DEFY_BATTERY_FRAME_VERSION_OFFSET] = DEFY_BATTERY_VERSION;
    data[DEFY_BATTERY_FRAME_SIDE_OFFSET] = frame->side;
    defy_wired_put32(data + DEFY_BATTERY_FRAME_SESSION_OFFSET, frame->session);
    defy_wired_put32(data + DEFY_BATTERY_FRAME_SEQUENCE_OFFSET, frame->sample.sequence);
    uint32_t seconds = frame->sample.age_ms / 1000u + (frame->sample.age_ms % 1000u != 0);
    data[DEFY_BATTERY_FRAME_AGE_OFFSET] = seconds > UINT8_MAX ? UINT8_MAX : (uint8_t)seconds;
    data[DEFY_BATTERY_FRAME_STATUS_OFFSET] = frame->sample.status;
    for (size_t i = 0; i < DEFY_BATTERY_WORD_COUNT; i++) {
        defy_wired_put16(data + DEFY_BATTERY_FRAME_WORDS_OFFSET + 2 * i, frame->sample.words[i]);
    }
    defy_wired_frame_seal(data);
    return 0;
}

int defy_battery_frame_decode(const uint8_t *data, size_t length,
                              struct defy_battery_frame *frame) {
    if (data == NULL || frame == NULL || length != DEFY_BATTERY_FRAME_SIZE) {
        return -EINVAL;
    }
    if (!defy_wired_frame_crc_ok(data)) {
        return -EBADMSG;
    }
    if (!defy_wired_frame_is(data, 'B') ||
        data[DEFY_BATTERY_FRAME_VERSION_OFFSET] != DEFY_BATTERY_VERSION ||
        data[DEFY_BATTERY_FRAME_SIDE_OFFSET] > DEFY_WIRED_RIGHT ||
        defy_wired_get32(data + DEFY_BATTERY_FRAME_SESSION_OFFSET) == 0 ||
        defy_wired_get32(data + DEFY_BATTERY_FRAME_SEQUENCE_OFFSET) == 0 ||
        data[DEFY_BATTERY_FRAME_STATUS_OFFSET] > DEFY_BATTERY_SAMPLE_ANALOG_ERROR) {
        return -EPROTO;
    }
    struct defy_battery_frame decoded = {
        .side = data[DEFY_BATTERY_FRAME_SIDE_OFFSET],
        .session = defy_wired_get32(data + DEFY_BATTERY_FRAME_SESSION_OFFSET),
        .sample = {.sequence = defy_wired_get32(data + DEFY_BATTERY_FRAME_SEQUENCE_OFFSET),
                   .age_ms = (uint32_t)data[DEFY_BATTERY_FRAME_AGE_OFFSET] * 1000u,
                   .status = data[DEFY_BATTERY_FRAME_STATUS_OFFSET]},
    };
    for (size_t i = 0; i < DEFY_BATTERY_WORD_COUNT; i++) {
        decoded.sample.words[i] =
            defy_wired_get16(data + DEFY_BATTERY_FRAME_WORDS_OFFSET + 2 * i);
    }
    *frame = decoded;
    return 0;
}
