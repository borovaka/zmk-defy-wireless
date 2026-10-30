/* SPDX-License-Identifier: MIT */

#include <errno.h>
#include <string.h>

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/byteorder.h>

#include <dygma/defy_battery_central.h>
#include <dygma/defy_wired_protocol.h>

LOG_MODULE_REGISTER(defy_battery, CONFIG_DEFY_BATTERY_LOG_LEVEL);

struct battery_route_state {
    uint32_t epoch;
    uint32_t session;
    uint8_t side;
    uint8_t expected_side;
    bool live;
};

struct battery_side_state {
    struct defy_battery_frame frame;
    int64_t sampled_at;
    uint32_t retired_session;
    bool present;
    bool cached_valid;
    /* Set while the half charges and until its next level, which replaces the old one. */
    bool charging;
    uint8_t cached_level;
};

/* By link, then source. */
static struct battery_route_state routes[2][2];
static struct battery_side_state battery_sides[2];
static struct k_spinlock battery_lock;
static uint32_t battery_revision;

static bool route_valid(enum defy_link route, uint8_t source) {
    return route < ARRAY_SIZE(routes) && source < ARRAY_SIZE(routes[0]);
}

static bool side_known(uint8_t side) { return side <= DEFY_WIRED_RIGHT; }

/* Called with the battery lock held. Returns true when the host level may have changed. */
static bool cache_sample(struct battery_side_state *state,
                         const struct defy_battery_sample *sample) {
    struct defy_battery_charger charger;
    struct defy_battery_reading reading;
    bool powered = !defy_battery_sample_charger(sample, &charger) && charger.status_valid &&
                   !charger.boost && !charger.fault;
    if (powered && charger.state == DEFY_BATTERY_CHARGE_CHARGING) {
        /* The cached level predates the charge, and the voltage under charge reads high. */
        bool changed = !state->charging;
        state->charging = true;
        return changed;
    }
    if (powered && charger.state == DEFY_BATTERY_CHARGE_DONE) {
        reading.level = 100;
    } else if (defy_battery_sample_reading(sample, &reading)) {
        return false;
    }
    state->cached_level = reading.level;
    state->cached_valid = true;
    state->charging = false;
    return true;
}

/*
 * Called with the battery lock held. The live link now belongs to the half on
 * side. A session from the other half ends, with -EPROTO.
 */
static int identify_locked(struct battery_route_state *link, uint8_t side, bool *changed) {
    int err = 0;
    *changed = link->expected_side != side;
    if (link->session && link->side != side) {
        link->session = 0;
        err = -EPROTO;
    }
    link->side = side;
    link->expected_side = side;
    return err;
}

int defy_battery_central_route_live(enum defy_link route, uint8_t source, uint32_t epoch,
                                    uint8_t side) {
    if (!route_valid(route, source) || !epoch ||
        (!side_known(side) && side != DEFY_WIRED_SIDE_UNKNOWN)) {
        return -EINVAL;
    }
    int err = 0;
    bool changed = false;
    k_spinlock_key_t key = k_spin_lock(&battery_lock);
    struct battery_route_state *link = &routes[route][source];
    if (!link->live || link->epoch != epoch) {
        *link = (struct battery_route_state){.epoch = epoch, .side = side,
                                           .expected_side = side, .live = true};
        changed = true;
    } else if (side_known(side)) {
        err = identify_locked(link, side, &changed);
    }
    k_spin_unlock(&battery_lock, key);
    if (changed) {
        defy_battery_host_changed();
    }
    return err;
}

void defy_battery_central_disconnect(enum defy_link route, uint8_t source, uint32_t epoch) {
    if (!route_valid(route, source)) {
        return;
    }
    k_spinlock_key_t key = k_spin_lock(&battery_lock);
    struct battery_route_state *link = &routes[route][source];
    if (link->epoch == epoch) {
        link->live = false;
    }
    k_spin_unlock(&battery_lock, key);
    defy_battery_host_changed();
}

int defy_battery_central_route_identify(enum defy_link route, uint8_t source, uint32_t epoch,
                                        uint8_t side) {
    if (!route_valid(route, source) || !side_known(side) || !epoch) {
        return -EINVAL;
    }
    int err = 0;
    bool changed = false;
    k_spinlock_key_t key = k_spin_lock(&battery_lock);
    struct battery_route_state *link = &routes[route][source];
    if (!link->live || link->epoch != epoch) {
        err = -ENOTCONN;
    } else {
        err = identify_locked(link, side, &changed);
    }
    k_spin_unlock(&battery_lock, key);
    if (changed) {
        defy_battery_host_changed();
    }
    return err;
}

int defy_battery_central_route_side(enum defy_link route, uint8_t source, uint32_t epoch,
                                    uint8_t *side) {
    if (!route_valid(route, source) || !side || !epoch) {
        return -EINVAL;
    }
    int err = 0;
    k_spinlock_key_t key = k_spin_lock(&battery_lock);
    const struct battery_route_state *link = &routes[route][source];
    if (!link->live || link->epoch != epoch) {
        err = -ENOTCONN;
    } else if (!link->session || !side_known(link->side) ||
               (side_known(link->expected_side) && link->side != link->expected_side)) {
        err = -ENODATA;
    } else {
        *side = link->side;
    }
    k_spin_unlock(&battery_lock, key);
    return err;
}

/* Logs a new sample; without logging, decodes nothing. */
static void log_sample(const struct defy_battery_frame *frame) {
    if (!IS_ENABLED(CONFIG_LOG)) {
        return;
    }
    const struct defy_battery_sample *sample = &frame->sample;
    const uint16_t *words = sample->words;
    struct defy_battery_reading reading;
    int valid;

    switch (sample->status) {
    case DEFY_BATTERY_SAMPLE_I2C_DIAGNOSTIC:
        if (words[DEFY_BATTERY_I2C_TAG] == DEFY_BATTERY_I2C_CHARGER_TAG) {
            uint16_t gauge = words[DEFY_BATTERY_I2C_GAUGE_TRACE];
            uint16_t charger = words[DEFY_BATTERY_I2C_CHARGER_TRACE];
            int charger_error = (int16_t)words[DEFY_BATTERY_I2C_CHARGER_ERRNO];
            uint16_t raw = words[DEFY_BATTERY_I2C_CHARGER_VALUE];
            LOG_INF("Battery I2C side=%u age=%u seq=%u tag=d102 gaddr=36 greg=%02x gerr=%d gn=%u gev=%u gt=%u gms=%u caddr=6a creg=%02x cerr=%d raw=%04x cn=%u cev=%u ct=%u cms=%u match=%d",
                    frame->side, sample->age_ms, sample->sequence,
                    words[DEFY_BATTERY_I2C_PROBE_REGISTERS] & 0xff,
                    (int16_t)words[DEFY_BATTERY_I2C_GAUGE_ERRNO], gauge >> 8,
                    gauge & 0xf, (gauge >> 4) & 0xf,
                    words[DEFY_BATTERY_I2C_PROBE_ELAPSED_MS] & 0xff,
                    words[DEFY_BATTERY_I2C_PROBE_REGISTERS] >> 8, charger_error, raw,
                    charger >> 8, charger & 0xf, (charger >> 4) & 0xf,
                    words[DEFY_BATTERY_I2C_PROBE_ELAPSED_MS] >> 8,
                    charger_error || raw > UINT8_MAX
                        ? -1
                        : (raw & DEFY_BATTERY_CHARGER_ID_MASK) == DEFY_BATTERY_CHARGER_ID_VALUE);
        } else {
            LOG_INF("Battery I2C side=%u age=%u seq=%u tag=%04x reg=%02x errno=%d us=%u events=%u event=%u type=%u pins=%04x cnf8=%04x",
                    frame->side, sample->age_ms, sample->sequence,
                    words[DEFY_BATTERY_I2C_TAG], words[DEFY_BATTERY_I2C_REGISTER_EVENTS] & 0xff,
                    (int16_t)words[DEFY_BATTERY_I2C_ERRNO],
                    (uint32_t)words[DEFY_BATTERY_I2C_ELAPSED_LOW] |
                        (uint32_t)words[DEFY_BATTERY_I2C_ELAPSED_HIGH] << 16,
                    words[DEFY_BATTERY_I2C_REGISTER_EVENTS] >> 8,
                    words[DEFY_BATTERY_I2C_EVENT_TYPE] & 0xff,
                    words[DEFY_BATTERY_I2C_EVENT_TYPE] >> 8,
                    words[DEFY_BATTERY_I2C_PINS], words[DEFY_BATTERY_I2C_PIN_CNF_LOW8]);
        }
        break;
    case DEFY_BATTERY_SAMPLE_ANALOG_OK:
        valid = defy_battery_sample_reading(sample, &reading);
        LOG_INF("Battery analog side=%u age=%u seq=%u tag=%04x threshold=%u ref=%u raw_mv=%u valid=%d level=%d mv=%d",
                frame->side, sample->age_ms, sample->sequence,
                words[DEFY_BATTERY_ANALOG_FORMAT],
                words[DEFY_BATTERY_ANALOG_FORMAT] == DEFY_BATTERY_ANALOG_FILTERED_TAG
                    ? words[DEFY_BATTERY_ANALOG_THRESHOLD] & 0xffu
                    : words[DEFY_BATTERY_ANALOG_THRESHOLD],
                words[DEFY_BATTERY_ANALOG_REFERENCE], words[DEFY_BATTERY_ANALOG_MILLIVOLTS],
                valid, valid ? -1 : reading.level, valid ? -1 : reading.millivolts);
        break;
    case DEFY_BATTERY_SAMPLE_ANALOG_ERROR:
        LOG_INF("Battery analog side=%u age=%u seq=%u tag=%04x errno=%d",
                frame->side, sample->age_ms, sample->sequence, words[0], (int16_t)words[1]);
        break;
    default:
        valid = defy_battery_sample_reading(sample, &reading);
        LOG_INF("Battery side=%u status=%u age=%u seq=%u raw=%04x,%04x,%04x,%04x,%04x,%04x,%04x,%04x level=%d mv=%d",
                frame->side, sample->status, sample->age_ms, sample->sequence,
                words[0], words[1], words[2], words[3], words[4], words[5], words[6], words[7],
                valid ? -1 : reading.level, valid ? -1 : reading.millivolts);
        break;
    }
}

int defy_battery_central_accept(enum defy_link route, uint8_t source, uint32_t epoch,
                                const uint8_t *data, size_t length, uint32_t extra_age_ms) {
    if (!route_valid(route, source)) {
        return -EINVAL;
    }
    struct defy_battery_frame frame;
    if (defy_battery_frame_decode(data, length, &frame)) {
        return -EBADMSG;
    }
    frame.sample.age_ms =
        MIN((uint64_t)UINT32_MAX, (uint64_t)frame.sample.age_ms + extra_age_ms);
    int err = 0;
    bool changed = false;
    int64_t sampled_at = k_uptime_get() - frame.sample.age_ms;
    k_spinlock_key_t key = k_spin_lock(&battery_lock);
    struct battery_route_state *link = &routes[route][source];
    struct battery_side_state *state = &battery_sides[frame.side];
    const struct battery_route_state *other = &routes[route][source ^ 1];
    if (!link->live || link->epoch != epoch) {
        err = -ENOTCONN;
    } else if ((side_known(link->expected_side) && link->expected_side != frame.side) ||
               (link->session && link->side != frame.side)) {
        err = -EPROTO;
    } else if (other->live && other->side == frame.side &&
               (other->session || side_known(other->expected_side))) {
        err = -EADDRINUSE;
    } else if (state->present && state->frame.session == frame.session) {
        int32_t delta = (int32_t)(frame.sample.sequence - state->frame.sample.sequence);
        if (delta < 0) {
            err = -ESTALE;
        } else if (delta == 0) {
            if (frame.sample.status != state->frame.sample.status ||
                memcmp(frame.sample.words, state->frame.sample.words,
                       sizeof(frame.sample.words))) {
                err = -EPROTO;
            } else {
                state->sampled_at = MIN(state->sampled_at, sampled_at);
            }
        } else {
            changed = true;
        }
    } else if (state->present &&
               (state->retired_session == frame.session ||
                (link->session == frame.session && link->session != state->frame.session))) {
        err = -ESTALE;
    } else {
        if (state->present) {
            state->retired_session = state->frame.session;
        }
        changed = true;
    }
    if (!err) {
        link->side = frame.side;
        link->session = frame.session;
        if (changed) {
            state->frame = frame;
            state->sampled_at = sampled_at;
            state->present = true;
            if (cache_sample(state, &frame.sample)) {
                battery_revision++;
            }
        }
    }
    k_spin_unlock(&battery_lock, key);
    if (changed && !err) {
        log_sample(&frame);
    }
    if (!err) {
        defy_battery_host_changed();
    }
    return err;
}

int defy_battery_central_get(uint8_t side, struct defy_battery_record *record) {
    if (!side_known(side) || !record) {
        return -EINVAL;
    }
    memset(record, 0, sizeof(*record));
    k_spinlock_key_t key = k_spin_lock(&battery_lock);
    const struct battery_side_state *state = &battery_sides[side];
    record->present = state->present;
    record->frame = state->frame;
    if (state->present) {
        record->frame.sample.age_ms = MIN((uint64_t)UINT32_MAX,
                                          (uint64_t)MAX(0, k_uptime_get() - state->sampled_at));
        for (size_t route = 0; route < ARRAY_SIZE(routes); route++) {
            for (size_t source = 0; source < ARRAY_SIZE(routes[route]); source++) {
                const struct battery_route_state *link = &routes[route][source];
                if (link->live && link->side == side && link->session == state->frame.session) {
                    record->route_mask |= BIT(route);
                }
            }
        }
    }
    k_spin_unlock(&battery_lock, key);
    record->connected = record->route_mask != 0;
    record->fresh = record->present && record->frame.sample.age_ms < DEFY_BATTERY_MAX_AGE_MS;
    record->validation_error = record->present
        ? defy_battery_sample_reading(&record->frame.sample, &record->reading) : -ENODATA;
    record->valid = record->validation_error == 0;
    return 0;
}

int defy_battery_central_aggregate(uint8_t *level, uint32_t *revision) {
    if (!level) {
        return -EINVAL;
    }
    k_spinlock_key_t key = k_spin_lock(&battery_lock);
    uint8_t lowest = UINT8_MAX;
    bool known = true;
    bool both_charging = battery_sides[0].charging && battery_sides[1].charging;
    for (size_t side = 0; side < ARRAY_SIZE(battery_sides); side++) {
        const struct battery_side_state *state = &battery_sides[side];
        /*
         * A charging half is left out unless both charge. Then the lowest level cached
         * before the charge stands, since a charging half only rises from its own.
         */
        if (state->charging && !both_charging) {
            continue;
        }
        if (state->cached_valid) {
            lowest = MIN(lowest, state->cached_level);
        } else if (!state->charging) {
            known = false;
        }
    }
    int err = -ENODATA;
    if (known && lowest != UINT8_MAX) {
        *level = lowest;
        if (revision) {
            *revision = battery_revision;
        }
        err = 0;
    }
    k_spin_unlock(&battery_lock, key);
    return err;
}

/* The flags byte of the diagnostics and the snapshot. */
static uint8_t record_flags(const struct defy_battery_record *record) {
    return record->present | record->connected << 1 | record->fresh << 2 | record->valid << 3;
}

int defy_battery_central_diagnostic(uint8_t side,
                                    uint8_t data[DEFY_BATTERY_DIAGNOSTIC_SIZE]) {
    if (!data) {
        return -EINVAL;
    }
    struct defy_battery_record record;
    int err = defy_battery_central_get(side, &record);
    if (err) {
        return err;
    }
    memset(data, 0, DEFY_BATTERY_DIAGNOSTIC_SIZE);
    data[0] = DEFY_BATTERY_VERSION;
    data[1] = side;
    data[2] = record_flags(&record);
    if (record.present) {
        return defy_battery_frame_encode(&record.frame, data + 4);
    }
    return 0;
}

size_t defy_battery_central_snapshot(uint8_t *data, size_t capacity) {
    if (!data || capacity < DEFY_BATTERY_SNAPSHOT_SIZE) {
        return 0;
    }
    memset(data, 0, DEFY_BATTERY_SNAPSHOT_SIZE);
    data[0] = DEFY_BATTERY_SNAPSHOT_VERSION;
    data[1] = ARRAY_SIZE(battery_sides);
    sys_put_le16(DEFY_BATTERY_SNAPSHOT_RECORD_SIZE, data + 2);
    sys_put_le32((uint32_t)k_uptime_get(), data + 4);
    for (uint8_t side = 0; side < ARRAY_SIZE(battery_sides); side++) {
        struct defy_battery_record record;
        defy_battery_central_get(side, &record);
        uint8_t *entry =
            data + DEFY_BATTERY_SNAPSHOT_HEADER + side * DEFY_BATTERY_SNAPSHOT_RECORD_SIZE;
        entry[0] = side;
        entry[1] = record_flags(&record);
        entry[2] = record.route_mask;
        entry[3] = record.reading.source;
        sys_put_le32(record.present ? record.frame.sample.age_ms : UINT32_MAX, entry + 4);
        sys_put_le32((uint32_t)record.validation_error, entry + 8);
        entry[12] = record.valid ? record.reading.level : UINT8_MAX;
        sys_put_le16(record.valid ? record.reading.millivolts : UINT16_MAX, entry + 14);
        if (record.present && defy_battery_frame_encode(&record.frame, entry + 16)) {
            return 0;
        }
    }
    return DEFY_BATTERY_SNAPSHOT_SIZE;
}
