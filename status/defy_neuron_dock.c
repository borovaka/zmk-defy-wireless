/*
 * Which halves are cabled to the Neuron, and whether it sits in the dock, read
 * from the halves' reset lines, as stock firmware reads them.
 *
 * The lines are the run-gpios of the board's dygma,defy-neuron-cables node.
 * Both stay inputs without pull: a cabled half pulls its line high through
 * the pull-up on its RP2040's RUN pin. Only the half updater drives them, low,
 * to reset the halves; while it holds them they are not sampled and the last
 * levels stand.
 *
 * From the place it also tells whether the Neuron runs from a half's battery,
 * which the lighting state carries to the halves, so that a cable keeps no
 * half awake then.
 *
 * SPDX-License-Identifier: MIT
 */

#include <zephyr/device.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/init.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/atomic.h>

#include <zmk/usb.h>

#include <dygma/defy_lighting.h>
#include <dygma/defy_neuron_cables.h>
#include <dygma/defy_neuron_dock.h>
#include <dygma/defy_update_guard.h>

#define SAMPLE_MS 250
/*
 * How long the place must read docked or alone before the Neuron counts as on
 * battery. Just after a start it reads alone for up to about a second on a
 * computer too, before USB has enumerated and the cables have two samples.
 */
#define ON_BATTERY_SETTLE_MS 5000

/* In cable bit order: the left half's line, then the right's. */
static const struct gpio_dt_spec lines[] = DEFY_NEURON_RUN_LINES;

/*
 * status: the last levels in bits 0-1, the debounced ones in bits 2-3, then
 * READY, HELD and ON_BATTERY.
 */
#define LEVELS 0x3
#define CABLES_SHIFT 2
#define READY BIT(4)
#define HELD BIT(5)
#define ON_BATTERY BIT(6)

static atomic_t status;
static bool sampled;
/* When the place began to read docked or alone, or -1. */
static int64_t battery_since = -1;

static void sample(struct k_work *work);
static K_WORK_DELAYABLE_DEFINE(sample_work, sample);

static bool lines_held(void) {
    return IS_ENABLED(CONFIG_DEFY_HALF_UPDATER) && defy_update_held(DEFY_UPDATE_HALVES);
}

static uint8_t cables_of(atomic_val_t state) { return (state >> CABLES_SHIFT) & LEVELS; }

static enum defy_neuron_place place_of(atomic_val_t state, enum zmk_usb_conn_state usb) {
    if (usb == ZMK_USB_CONN_HID) {
        return DEFY_NEURON_COMPUTER;
    }
    uint8_t cables = cables_of(state);
    if (cables == DEFY_NEURON_CABLE_LEFT || cables == DEFY_NEURON_CABLE_RIGHT) {
        return DEFY_NEURON_DOCKED;
    }
    return usb == ZMK_USB_CONN_POWERED ? DEFY_NEURON_CHARGER : DEFY_NEURON_ALONE;
}

/*
 * Sets ON_BATTERY in state once the place has read docked or alone for
 * ON_BATTERY_SETTLE_MS, and clears it as soon as it reads computer or charger.
 * While the half updater holds the lines the wait starts again, so a hold
 * never sets it.
 */
static atomic_val_t with_power(atomic_val_t state, bool held) {
    enum defy_neuron_place place = place_of(state, zmk_usb_get_conn_state());
    if (place != DEFY_NEURON_DOCKED && place != DEFY_NEURON_ALONE) {
        battery_since = -1;
        return state & ~ON_BATTERY;
    }
    if (held) {
        battery_since = -1;
        return state;
    }
    int64_t now = k_uptime_get();
    if (battery_since < 0) {
        battery_since = now;
    }
    return now - battery_since >= ON_BATTERY_SETTLE_MS ? state | ON_BATTERY : state;
}

static void sample(struct k_work *work) {
    ARG_UNUSED(work);
    atomic_val_t last = atomic_get(&status);
    atomic_val_t next;
    bool held = lines_held();
    if (held) {
        sampled = false;
        next = last | HELD;
    } else {
        uint8_t levels = 0;
        for (size_t i = 0; i < ARRAY_SIZE(lines); i++) {
            levels |= gpio_pin_get_dt(&lines[i]) > 0 ? BIT(i) : 0;
        }
        uint8_t cables = cables_of(last);
        if (sampled) {
            /* A line's level counts once two samples in a row agree. */
            uint8_t steady = ~(levels ^ last) & LEVELS;
            cables = (cables & ~steady) | (levels & steady);
        }
        sampled = true;
        next = READY | levels | cables << CABLES_SHIFT | (last & ON_BATTERY);
    }
    next = with_power(next, held);
    atomic_set(&status, next);
    k_work_schedule(&sample_work, K_MSEC(SAMPLE_MS));
    if (IS_ENABLED(CONFIG_DEFY_LIGHTING) && ((next ^ last) & ON_BATTERY)) {
        defy_lighting_central_power_changed();
    }
}

uint8_t defy_neuron_cables(void) { return cables_of(atomic_get(&status)); }

enum defy_neuron_place defy_neuron_place(void) {
    return place_of(atomic_get(&status), zmk_usb_get_conn_state());
}

bool defy_neuron_on_battery(void) { return atomic_get(&status) & ON_BATTERY; }

size_t defy_neuron_dock_status(uint8_t *out, size_t capacity) {
    if (!out || capacity < DEFY_NEURON_DOCK_STATUS_SIZE) {
        return 0;
    }
    atomic_val_t state = atomic_get(&status);
    enum zmk_usb_conn_state usb = zmk_usb_get_conn_state();
    out[0] = DEFY_NEURON_DOCK_STATUS_VERSION;
    out[1] = state & LEVELS;
    out[2] = cables_of(state);
    out[3] = (state & READY ? BIT(0) : 0) | (state & HELD ? BIT(1) : 0);
    out[4] = usb;
    out[5] = place_of(state, usb);
    out[6] = 0;
    out[7] = 0;
    return DEFY_NEURON_DOCK_STATUS_SIZE;
}

/* Before the half updater's thread starts, at 99, so no reset pulse is cut short. */
static int dock_init(void) {
    for (size_t i = 0; i < ARRAY_SIZE(lines); i++) {
        if (!gpio_is_ready_dt(&lines[i]) || gpio_pin_configure_dt(&lines[i], GPIO_INPUT)) {
            return 0;
        }
    }
    atomic_set(&status, READY);
    k_work_schedule(&sample_work, K_NO_WAIT);
    return 0;
}

SYS_INIT(dock_init, APPLICATION, 50);
