/*
 * A half's start-up display, the lowest of the displays defy_half_leds.c
 * stacks: every LED, dimly, in the colour of the furthest stage reached, and
 * once the radio has failed, a report on the per-key LEDs. docs/lighting.md,
 * "Start-up and fault colours", has the colours and the report's rows.
 *
 * The display goes dark two minutes after the last change, ten once the radio
 * has failed. Once the Neuron's lighting preference has arrived, and while the
 * radio is up or not yet asked, off keeps it dark and on keeps it lit while
 * the half reaches the Neuron.
 *
 * With logging, the stages reached are logged again every 5 s for the first 5
 * minutes, so that a USB log opened late still shows them.
 *
 * SPDX-License-Identifier: MIT
 */

#include <zephyr/drivers/led_strip.h>
#include <zephyr/init.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/util.h>

#include <zmk/event_manager.h>
#include <zmk/events/split_peripheral_status_changed.h>

#include <dygma/defy_fatal_leds.h>
#include <dygma/defy_hci_spi.h>
#include <dygma/defy_radio_factory.h>
#include <dygma/defy_startup.h>

#include "defy_half_leds_internal.h"

LOG_MODULE_REGISTER(defy_startup, LOG_LEVEL_INF);

#define LENGTH DEFY_HALF_LEDS_LENGTH
#define KEYS DEFY_HALF_LEDS_KEYS
BUILD_ASSERT(KEYS == DEFY_LIGHTING_KEY_LEDS, "a lighting view has a colour for every per-key LED");
#define LEVEL DEFY_HALF_LEDS_LEVEL

/* The failure report's row for the radio's first answer. */
#define ANSWER_ROW 3

#define DARK_AFTER K_MINUTES(2)
#define DARK_AFTER_FAILURE K_MINUTES(10)
#define CONNECTED_SHOW K_SECONDS(2)
#define BLINK_PERIOD K_MSEC(500)
#define LOG_PERIOD K_SECONDS(5)
#define LOG_UNTIL_MS (5 * 60 * 1000)

static const struct {
    const char *name;
    struct led_rgb colour;
} stages[] = {
    [DEFY_STARTUP_RUNNING] = {"ZMK running", {.b = LEVEL}},
    [DEFY_STARTUP_RADIO_BOOTLOADER] = {"radio bootloader answered", {.r = LEVEL, .g = LEVEL}},
    [DEFY_STARTUP_RADIO_INSTALLING] = {"installing radio firmware", {.r = LEVEL, .g = LEVEL / 4}},
    [DEFY_STARTUP_RADIO_READY] = {"Bluetooth up", {.g = LEVEL}},
    [DEFY_STARTUP_RADIO_FAILED] = {"radio not answering", {.r = LEVEL}},
};

static const struct led_rgb connected_colour = {.g = LEVEL, .b = LEVEL};

static enum defy_startup_stage stage = DEFY_STARTUP_RUNNING;
static enum defy_startup_stage before_failure = DEFY_STARTUP_RUNNING;
static uint32_t reached;
static size_t lit;
static bool connected;
/* The connection shows in cyan before the everyday colours. */
static bool connection_showing;
static bool dark;
static bool blink;

static void blink_toggle(struct k_work *work);
static void go_dark(struct k_work *work);
static void connection_shown(struct k_work *work);
static void log_reached(struct k_work *work);

static K_WORK_DELAYABLE_DEFINE(blink_work, blink_toggle);
static K_WORK_DELAYABLE_DEFINE(dark_work, go_dark);
static K_WORK_DELAYABLE_DEFINE(connection_work, connection_shown);
static K_WORK_DELAYABLE_DEFINE(log_work, log_reached);

/* The radio's first answer to a status poll other than 00 00 or ff ff. */
static struct {
    bool answered;
    uint8_t bytes[2];
} answer;

bool defy_startup_leds_allowed(void) {
    return stage == DEFY_STARTUP_RUNNING || stage == DEFY_STARTUP_RADIO_READY;
}

bool defy_startup_leds_hidden(bool preference_known, bool preference) {
    return preference_known && defy_startup_leds_allowed() ? !preference || (!connected && dark)
                                                           : dark;
}

static struct led_rgb everyday_colour(struct defy_lighting_colour colour) {
    return (struct led_rgb){.r = colour.r, .g = colour.g, .b = colour.b};
}

/* Every LED in the colour of the stage, or once connected, the everyday colours. */
static void draw_stage(struct led_rgb *frame, bool hidden,
                       const struct defy_lighting_view *everyday) {
    bool settled = connected && !connection_showing && everyday && defy_startup_leds_allowed();

    for (size_t i = 0; i < LENGTH; i++) {
        struct led_rgb colour = stages[stage].colour;

        if (hidden) {
            colour = (struct led_rgb){0};
        } else if (settled) {
            colour = everyday_colour(i < KEYS ? everyday->keys[i] : everyday->underglow);
        } else if (connected) {
            colour = connected_colour;
        } else if (stage == DEFY_STARTUP_RADIO_FAILED && !blink) {
            colour = stages[before_failure].colour;
        } else if (stage == DEFY_STARTUP_RADIO_INSTALLING && i >= lit) {
            colour = (struct led_rgb){0};
        }
        frame[i] = colour;
    }
}

static void answer_row(size_t *count, struct led_rgb *colour) {
    if (!answer.answered) {
        return;
    }
    if (defy_radio_factory_is_status(answer.bytes)) {
        *count = answer.bytes[1] - DEFY_RADIO_STATE_SUCCEEDED + 1;
        *colour = stages[DEFY_STARTUP_RADIO_BOOTLOADER].colour;
    } else {
        bool controller = answer.bytes[0] == DEFY_HCI_SPI_READY_NOW &&
                          answer.bytes[1] == DEFY_HCI_SPI_SANITY_CHECK;

        *count = defy_half_leds_rows[ANSWER_ROW];
        *colour = controller ? (struct led_rgb){.g = LEVEL} : (struct led_rgb){.b = LEVEL};
    }
}

/* The per-key LEDs' report once the radio has failed. */
static void draw_report(struct led_rgb *frame) {
    size_t i = 0;

    for (size_t r = 0; r < ARRAY_SIZE(defy_half_leds_rows); r++) {
        size_t count = 0;
        struct led_rgb colour = {0};

        if (r == ANSWER_ROW) {
            answer_row(&count, &colour);
        } else if (IS_ENABLED(CONFIG_DEFY_RADIO_INSTALL)) {
            defy_radio_install_leds_row(r, &count, &colour);
        }
        for (size_t k = 0; k < defy_half_leds_rows[r]; k++, i++) {
            frame[i] = k < count ? colour : (struct led_rgb){0};
        }
    }
}

void defy_startup_leds_draw(struct led_rgb *frame, bool hidden,
                            const struct defy_lighting_view *everyday) {
    draw_stage(frame, hidden, everyday);
    if (stage != DEFY_STARTUP_RADIO_FAILED || hidden) {
        return;
    }
    if (IS_ENABLED(CONFIG_DEFY_RADIO_INSTALL) && defy_radio_install_leds_traced()) {
        defy_radio_install_leds_draw_trace(frame);
    } else if (!connected) {
        draw_report(frame);
    }
}

/*
 * Called with the lock held, after a change worth seeing. The failure's blink stops while
 * the display is dark, and starts again here.
 */
static void changed(void) {
    dark = false;
    defy_half_leds_show();
    k_work_reschedule(&dark_work,
                      stage == DEFY_STARTUP_RADIO_FAILED ? DARK_AFTER_FAILURE : DARK_AFTER);
    if (stage == DEFY_STARTUP_RADIO_FAILED) {
        k_work_schedule(&blink_work, BLINK_PERIOD);
    }
}

static void blink_toggle(struct k_work *work) {
    ARG_UNUSED(work);

    k_mutex_lock(&defy_half_leds_lock, K_FOREVER);
    if (stage == DEFY_STARTUP_RADIO_FAILED && !dark) {
        blink = !blink;
        defy_half_leds_show();
        k_work_reschedule(&blink_work, BLINK_PERIOD);
    }
    k_mutex_unlock(&defy_half_leds_lock);
}

static void go_dark(struct k_work *work) {
    ARG_UNUSED(work);

    k_mutex_lock(&defy_half_leds_lock, K_FOREVER);
    dark = true;
    defy_half_leds_show();
    k_mutex_unlock(&defy_half_leds_lock);
}

static void connection_shown(struct k_work *work) {
    ARG_UNUSED(work);

    k_mutex_lock(&defy_half_leds_lock, K_FOREVER);
    if (connection_showing) {
        connection_showing = false;
        defy_half_leds_show();
    }
    k_mutex_unlock(&defy_half_leds_lock);
}

static void log_reached(struct k_work *work) {
    ARG_UNUSED(work);

    k_mutex_lock(&defy_half_leds_lock, K_FOREVER);
    for (size_t i = 0; i < ARRAY_SIZE(stages); i++) {
        if (reached & BIT(i)) {
            LOG_INF("Start-up reached: %s", stages[i].name);
        }
    }
    LOG_INF("Start-up now: %s%s", stages[stage].name, connected ? ", connected" : "");
    if (IS_ENABLED(CONFIG_DEFY_RADIO_INSTALL)) {
        defy_radio_install_leds_log();
    }
    k_mutex_unlock(&defy_half_leds_lock);

    if (k_uptime_get() < LOG_UNTIL_MS) {
        k_work_reschedule(&log_work, LOG_PERIOD);
    }
}

/* Called with the lock held. */
static void enter(enum defy_startup_stage next) {
    if (next == DEFY_STARTUP_RADIO_FAILED && stage != DEFY_STARTUP_RADIO_FAILED) {
        before_failure = stage;
    }
    stage = next;
    reached |= BIT(next);
    LOG_INF("Start-up: %s", stages[next].name);
}

void defy_startup_report(enum defy_startup_stage next) {
    if (next >= ARRAY_SIZE(stages)) {
        return;
    }

    k_mutex_lock(&defy_half_leds_lock, K_FOREVER);
    enter(next);
    lit = 0;
    blink = true;
    changed();
    k_mutex_unlock(&defy_half_leds_lock);
}

void defy_startup_progress(size_t written, size_t size) {
    size_t count =
        size == 0 ? LENGTH : MAX((size_t)1, (size_t)((uint64_t)written * LENGTH / size));

    k_mutex_lock(&defy_half_leds_lock, K_FOREVER);
    if (IS_ENABLED(CONFIG_DEFY_RADIO_INSTALL)) {
        defy_radio_install_leds_progress(written, size);
    }
    if (stage != DEFY_STARTUP_RADIO_INSTALLING) {
        enter(DEFY_STARTUP_RADIO_INSTALLING);
        lit = 0;
    }
    if (count != lit) {
        lit = count;
        changed();
    }
    if (written == size) {
        LOG_INF("Radio firmware written: %zu bytes", size);
    }
    k_mutex_unlock(&defy_half_leds_lock);
}

void defy_startup_radio_answer(uint8_t first, uint8_t second) {
    k_mutex_lock(&defy_half_leds_lock, K_FOREVER);
    if (!answer.answered && !(first == second && (first == 0 || first == 0xff))) {
        answer.answered = true;
        answer.bytes[0] = first;
        answer.bytes[1] = second;
    }
    k_mutex_unlock(&defy_half_leds_lock);
}

void defy_startup_split_connected(bool now_connected) {
    k_mutex_lock(&defy_half_leds_lock, K_FOREVER);
    if (connected != now_connected) {
        connected = now_connected;
        connection_showing = connected;
        if (connected) {
            k_work_reschedule(&connection_work, CONNECTED_SHOW);
        } else {
            k_work_cancel_delayable(&connection_work);
        }
        LOG_INF("Start-up: %s the Neuron", connected ? "connected to" : "disconnected from");
        changed();
    }
    k_mutex_unlock(&defy_half_leds_lock);
}

/*
 * A half with one transport takes the link from ZMK's event. On a hybrid half that
 * event covers Bluetooth alone, so the hybrid transport calls
 * defy_startup_split_connected() with cable and Bluetooth combined instead.
 */
#if IS_ENABLED(CONFIG_ZMK_SPLIT) && !IS_ENABLED(CONFIG_ZMK_SPLIT_ROLE_CENTRAL) &&                  \
    !IS_ENABLED(CONFIG_DEFY_HYBRID)
static int on_split_status(const zmk_event_t *event) {
    const struct zmk_split_peripheral_status_changed *status =
        as_zmk_split_peripheral_status_changed(event);

    if (status != NULL) {
        defy_startup_split_connected(status->connected);
    }
    return ZMK_EV_EVENT_BUBBLE;
}

ZMK_LISTENER(defy_startup, on_split_status);
ZMK_SUBSCRIPTION(defy_startup, zmk_split_peripheral_status_changed);
#endif

/*
 * After the LED drivers, before the radio driver's catch_bootloader_at_start() at
 * APPLICATION 1.
 */
static int defy_startup_init(void) {
    k_mutex_lock(&defy_half_leds_lock, K_FOREVER);
    if (!defy_half_leds_start()) {
        LOG_ERR("LEDs not ready: start-up shows in the log only");
        if (IS_ENABLED(CONFIG_DEFY_FATAL_LEDS)) {
            /*
             * Lights every per-key LED through the registers instead, and leaves them
             * lit. SPI0 is free: Bluetooth starts later.
             */
            defy_fatal_leds_show(DEFY_FATAL_LEDS_LEVEL, DEFY_FATAL_LEDS_LEVEL,
                                 DEFY_FATAL_LEDS_LEVEL, DEFY_FATAL_LEDS_LEVEL);
        }
    }
    enter(DEFY_STARTUP_RUNNING);
    changed();
    k_mutex_unlock(&defy_half_leds_lock);

    if (IS_ENABLED(CONFIG_LOG)) {
        k_work_reschedule(&log_work, LOG_PERIOD);
    }
    return 0;
}

SYS_INIT(defy_startup_init, APPLICATION, 0);
