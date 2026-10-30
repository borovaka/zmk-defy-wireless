/*
 * The radio install's part of a half's failure report, with
 * CONFIG_DEFY_RADIO_INSTALL: rows 1, 2, 3 and 5 from the bootloader sessions,
 * or after an install attempt its trace in their place. docs/lighting.md,
 * "Radio failure report", has the rows and the trace.
 *
 * SPDX-License-Identifier: MIT
 */

#include <errno.h>

#include <zephyr/drivers/led_strip.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/util.h>

#include <dygma/defy_radio_factory.h>
#include <dygma/defy_startup.h>

#include "defy_half_leds_internal.h"

LOG_MODULE_DECLARE(defy_startup, LOG_LEVEL_INF);

#define LEVEL DEFY_HALF_LEDS_LEVEL

/* The report's bottom row, the thumb keys. */
#define THUMB_ROW 4
/* When the first session began, per LED of the second and third rows. */
#define FINE_MS 20
#define COARSE_MS 200
/* Top-row keys 5, 6 and 7 of the trace: the command's colour, the result and the progress. */
#define COMMAND_PIXEL 4
#define RESULT_PIXEL 5
#define PROGRESS_PIXEL 6

/* The bootloader sessions, as defy_startup_radio_session() reports them. */
static struct {
    unsigned int count;
    int64_t first_ms;
    int first_error;
} sessions;

/* The latest install attempt, as defy_startup_radio_install() reports it. */
static struct {
    bool complete;
    bool attempted;
    bool erases_completed;
    bool finished;
    size_t written;
    size_t size;
    int error;
    int64_t started_ms;
    int64_t ended_ms;
    struct defy_radio_factory_trace trace;
    struct defy_radio_factory_info info;
} radio_trace;

static size_t error_lit(int err) {
    switch (err) {
    case -ETIMEDOUT:
        return 1;
    case -EPROTO:
        return 2;
    case -EIO:
        return 3;
    case -ENOTSUP:
        return 4;
    case -EILSEQ:
        return 5;
    case -EINVAL:
        return 6;
    default:
        return 7;
    }
}

void defy_radio_install_leds_row(size_t row, size_t *count, struct led_rgb *colour) {
    const uint8_t *rows = defy_half_leds_rows;

    switch (row) {
    case 0:
        if (sessions.count > 0) {
            *count = sessions.first_error == 0 ? rows[0] : error_lit(sessions.first_error);
            *colour = sessions.first_error == 0 ? (struct led_rgb){.g = LEVEL}
                                                : (struct led_rgb){.r = LEVEL};
        }
        break;
    case 1:
    case 2:
        if (sessions.count > 0) {
            *count = MIN(rows[row], (size_t)(sessions.first_ms / (row == 1 ? FINE_MS : COARSE_MS)));
            *colour = (struct led_rgb){.r = LEVEL, .g = LEVEL, .b = LEVEL};
        }
        break;
    case THUMB_ROW:
        *count = MIN(rows[THUMB_ROW], sessions.count);
        *colour = (struct led_rgb){.r = LEVEL, .b = LEVEL};
        break;
    default:
        break;
    }
}

bool defy_radio_install_leds_traced(void) { return radio_trace.complete; }

void defy_radio_install_leds_draw_trace(struct led_rgb *frame) {
    const uint8_t *rows = defy_half_leds_rows;
    const struct defy_radio_factory_trace *trace = &radio_trace.trace;
    uint8_t nibbles[] = {trace->rx[0] >> 4, trace->rx[0] & 15,
                         trace->rx[1] >> 4, trace->rx[1] & 15,
                         trace->phase, radio_trace.error == 0 ? 0 : error_lit(radio_trace.error)};
    struct led_rgb colour = trace->command == 'B' ? (struct led_rgb){.b = LEVEL}
                            : trace->command == 'I' ? (struct led_rgb){.g = LEVEL, .b = LEVEL}
                                                    : (struct led_rgb){.r = LEVEL, .b = LEVEL};
    size_t offset = 0;

    for (size_t row = 0; row < DEFY_LIGHTING_ROWS; row++) {
        for (size_t key = 0; key < rows[row]; key++) {
            size_t nibble = row == THUMB_ROW ? 4 + key / 4 : row;
            bool on = (row == THUMB_ROW || key < 4) && (nibbles[nibble] & BIT(key % 4));

            frame[offset + key] = on ? colour : (struct led_rgb){0};
        }
        offset += rows[row];
    }
    frame[COMMAND_PIXEL] = colour;
    frame[RESULT_PIXEL] =
        radio_trace.error == 0 ? (struct led_rgb){.g = LEVEL} : (struct led_rgb){.r = LEVEL};

    const uint8_t command_leds[] = {11, 12, 13, 18, 19, 20, 25, 26};

    for (size_t bit = 0; bit < ARRAY_SIZE(command_leds); bit++) {
        frame[command_leds[bit]] = trace->command & BIT(bit)
            ? (struct led_rgb){.r = LEVEL, .g = LEVEL, .b = LEVEL}
            : (struct led_rgb){0};
    }
    bool written = radio_trace.size > 0 && radio_trace.written == radio_trace.size;

    frame[PROGRESS_PIXEL] =
        written && radio_trace.finished ? (struct led_rgb){.g = LEVEL}
        : written                       ? (struct led_rgb){.g = LEVEL, .b = LEVEL}
        : radio_trace.erases_completed  ? (struct led_rgb){.r = LEVEL, .g = LEVEL / 4}
        : radio_trace.attempted         ? (struct led_rgb){.r = LEVEL, .b = LEVEL}
                                        : (struct led_rgb){.b = LEVEL};
}

void defy_radio_install_leds_progress(size_t written, size_t size) {
    radio_trace.written = written;
    radio_trace.size = size;
}

void defy_radio_install_leds_log(void) {
    if (!radio_trace.complete) {
        return;
    }
    LOG_INF("Radio trace: result %d command %02x phase %u TX %02x %02x RX %02x %02x length %zu exchanges %u start %lld end %lld",
            radio_trace.error, radio_trace.trace.command, radio_trace.trace.phase,
            radio_trace.trace.tx[0], radio_trace.trace.tx[1],
            radio_trace.trace.rx[0], radio_trace.trace.rx[1], radio_trace.trace.length,
            radio_trace.trace.exchanges, radio_trace.started_ms, radio_trace.ended_ms);
    LOG_INF("Radio install: attempted %u erases complete %u written %zu/%zu finish sent %u",
            radio_trace.attempted, radio_trace.erases_completed,
            radio_trace.written, radio_trace.size, radio_trace.finished);
    if (radio_trace.trace.phase == DEFY_RADIO_PHASE_INFO) {
        LOG_INF("Radio INFO: device %08x boot %08x hardware %08x version %08x start %08x",
                radio_trace.info.device_id, radio_trace.info.bootloader_version,
                radio_trace.info.hardware_version, radio_trace.info.program_version,
                radio_trace.info.program_start);
    }
}

void defy_startup_radio_session(int err) {
    k_mutex_lock(&defy_half_leds_lock, K_FOREVER);
    int64_t now = k_uptime_get();
    if (err == 0) {
        radio_trace.complete = false;
        radio_trace.written = 0;
        radio_trace.size = 0;
        radio_trace.started_ms = now;
    }
    if (sessions.count == 0) {
        sessions.first_ms = now;
        sessions.first_error = err;
    }
    sessions.count++;
    LOG_INF("Bootloader session %u at %lld ms: %d", sessions.count, now, err);
    k_mutex_unlock(&defy_half_leds_lock);
}

void defy_startup_radio_install(const struct defy_radio_factory *factory, int err, bool attempted) {
    k_mutex_lock(&defy_half_leds_lock, K_FOREVER);
    radio_trace.complete = true;
    radio_trace.attempted = attempted;
    /* Progress is reported only after the erases. */
    radio_trace.erases_completed = radio_trace.size > 0;
    radio_trace.finished = factory->finish_transmitted;
    radio_trace.error = err;
    radio_trace.ended_ms = k_uptime_get();
    radio_trace.trace = factory->trace;
    radio_trace.info = factory->info;
    k_mutex_unlock(&defy_half_leds_lock);
}
