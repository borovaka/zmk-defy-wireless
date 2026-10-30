/*
 * USB report recovery on the hybrid Neuron (docs/power-and-wake.md). Wraps
 * ZMK's keyboard and consumer report senders, its mouse report sender with
 * CONFIG_ZMK_POINTING, usb_enable() and usb_wakeup_request():
 *
 * - A report the host may have missed, because the bus was suspended or the
 *   send failed with -EAGAIN or -EIO, goes again: at once after a resume, and
 *   with a back-off of 5 to 80 ms after a failed send.
 * - When output leaves USB, the host gets all-released reports, so that no
 *   key or mouse button stays held there.
 * - Only a key newly pressed after a suspend asks the host to wake, up to
 *   three times 100 ms apart. ZMK's own wake requests are refused.
 *
 * SPDX-License-Identifier: MIT
 */

#include <errno.h>
#include <string.h>

#include <zephyr/device.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/usb/usb_device.h>

#include <dygma/defy_hybrid.h>
#include <dygma/defy_usb_report_recovery.h>
#include <dygma/defy_wrap.h>
#include <zmk/endpoints.h>
#include <zmk/event_manager.h>
#include <zmk/events/endpoint_changed.h>
#include <zmk/hid.h>
#include <zmk/usb.h>
#include <zmk/usb_hid.h>

#ifdef CONFIG_DEFY_USB_POSITION_TRACE
#include <zmk/events/position_state_changed.h>
#endif

LOG_MODULE_REGISTER(defy_usb_recovery, CONFIG_ZMK_LOG_LEVEL);

enum report_page {
    KEYBOARD,
    CONSUMER,
#if IS_ENABLED(CONFIG_ZMK_POINTING)
    MOUSE,
#endif
    PAGE_COUNT
};

#define PAGE_MASK BIT_MASK(PAGE_COUNT)
/* One per half, by enum defy_wired_side. */
#define SIDE_COUNT (DEFY_WIRED_RIGHT + 1)
#define RETRY_MS 5
/* Caps the back-off, so a release waits at most this long once the host polls again. */
#define RETRY_MAX_MS 80
#define WAKE_RETRY_MS 100
#define WAKE_MAX_ATTEMPTS 3

DEFY_WRAP_REAL(zmk_usb_hid_send_keyboard_report);
DEFY_WRAP_REAL(zmk_usb_hid_send_consumer_report);
#if IS_ENABLED(CONFIG_ZMK_POINTING)
DEFY_WRAP_REAL(zmk_usb_hid_send_mouse_report);
#endif
DEFY_WRAP_REAL(usb_enable);
DEFY_WRAP_REAL(usb_wakeup_request);

static struct k_spinlock state_lock;
static struct {
    /*
     * Changes on every bus reset, disconnect, error, suspend and resume: a send
     * that spans one does not count.
     */
    uint32_t usb_generation;
    /* Changes whenever USB becomes or stops being the selected output. */
    uint32_t route_generation;
    /* Live sends per page, so that a retry does not clear a page sent again meanwhile. */
    uint32_t revision[PAGE_COUNT];
    /* Pages whose live report the host may not have. */
    uint8_t dirty;
    /* Pages owed an all-released report since USB stopped being the output. */
    uint8_t terminal;
    bool configured;
    bool suspended;
    bool selected;
    bool wake_denied;
    bool wake_pending;
    bool wake_sent;
    uint8_t wake_attempts;
    /* Back-off after -EAGAIN or -EIO, RETRY_MS to RETRY_MAX_MS. */
    uint16_t retry_delay_ms;
    int64_t suspended_at;
    int64_t retry_at;
    int64_t wake_at;
    /* Each half's last key bitmap, to tell a new press from a held key. */
    struct {
        bool valid;
        int64_t received;
        uint8_t bitmap[DEFY_WIRED_BITMAP_SIZE];
    } input[SIDE_COUNT];
} state;
static usb_dc_status_callback original_status_callback;

static void trace_state(const char *origin, int event) {
    if (!IS_ENABLED(CONFIG_DEFY_USB_REPORT_TRACE)) {
        return;
    }

    k_spinlock_key_t key = k_spin_lock(&state_lock);
    uint32_t usb_generation = state.usb_generation;
    uint32_t route_generation = state.route_generation;
    bool configured = state.configured;
    bool suspended = state.suspended;
    bool selected = state.selected;
    uint8_t dirty = state.dirty;
    uint8_t terminal = state.terminal;
    k_spin_unlock(&state_lock, key);
    LOG_INF("%s event=%d status=%d usb=%u route=%u configured=%u suspended=%u selected=%u "
            "dirty=%u terminal=%u",
            origin, event, zmk_usb_get_status(), usb_generation, route_generation, configured,
            suspended, selected, dirty, terminal);
}

static void trace_report(const char *origin, enum report_page page, bool live,
                         const char *phase, int error) {
    if (!IS_ENABLED(CONFIG_DEFY_USB_REPORT_TRACE)) {
        return;
    }

    uint8_t modifiers = 0;
    if (live && page == KEYBOARD) {
        modifiers = zmk_hid_get_keyboard_report()->body.modifiers;
    }
    LOG_INF("%s %s page=%u live=%u mods=%02x explicit=%02x err=%d",
            origin, phase, page, live, modifiers, zmk_hid_get_explicit_mods(), error);
}

/*
 * All-released reports, sent when output leaves USB. They stay in RAM and
 * never change, because USB may still hold their addresses for DMA.
 */
static struct zmk_hid_keyboard_report terminal_keyboard __aligned(4) = {
    .report_id = ZMK_HID_REPORT_ID_KEYBOARD,
};
static struct zmk_hid_consumer_report terminal_consumer __aligned(4) = {
    .report_id = ZMK_HID_REPORT_ID_CONSUMER,
};
#if IS_ENABLED(CONFIG_ZMK_POINTING)
static struct zmk_hid_mouse_report terminal_mouse __aligned(4) = {
    .report_id = ZMK_HID_REPORT_ID_MOUSE,
};
#endif

static void retry_reports(struct k_work *work);
static K_WORK_DELAYABLE_DEFINE(retry_work, retry_reports);

/*
 * ZMK clears the live mouse report's movement once it has sent it, so a retry
 * sends the buttons alone. A retry that preempts the input thread between ZMK
 * setting a movement and clearing it repeats that movement once.
 */
static int send_live(enum report_page page) {
    switch (page) {
    case KEYBOARD:
        return __real_zmk_usb_hid_send_keyboard_report();
    case CONSUMER:
        return __real_zmk_usb_hid_send_consumer_report();
#if IS_ENABLED(CONFIG_ZMK_POINTING)
    case MOUSE:
        return __real_zmk_usb_hid_send_mouse_report();
#endif
    default:
        return -EINVAL;
    }
}

static bool transient(int error) { return error == -EAGAIN || error == -EIO; }

static void reset_report_retry(void) {
    state.retry_at = 0;
    state.retry_delay_ms = 0;
}

static void defer_report_retry(void) {
    int64_t now = k_uptime_get();
    if (state.retry_at > now) {
        return;
    }
    state.retry_delay_ms = state.retry_delay_ms
                               ? MIN(state.retry_delay_ms * 2, RETRY_MAX_MS)
                               : RETRY_MS;
    state.retry_at = now + state.retry_delay_ms;
}

static void cancel_wake(void) {
    state.wake_pending = false;
    state.wake_at = 0;
}

/* A new bus state: a newly pressed key may ask the host to wake once more. */
static void reset_wake(void) {
    state.wake_denied = false;
    state.wake_sent = false;
    cancel_wake();
}

static void select_route(bool selected) {
    if (selected == state.selected) {
        return;
    }
    bool was_selected = state.selected;
    /*
     * Releases the USB host never received are still owed on return. ZMK cleared the
     * live report on the switch, so sending it settles them.
     */
    uint8_t owed = selected && state.configured ? state.terminal : 0;
    state.selected = selected;
    state.route_generation++;
    state.dirty = owed;
    state.terminal = was_selected && !selected && state.configured ? PAGE_MASK : 0;
    reset_report_retry();
    cancel_wake();
}

void defy_usb_input_reset(void) {
    k_spinlock_key_t key = k_spin_lock(&state_lock);
    memset(state.input, 0, sizeof(state.input));
    cancel_wake();
    k_spin_unlock(&state_lock, key);
}

void defy_usb_input_snapshot(uint8_t side, const uint8_t bitmap[DEFY_WIRED_BITMAP_SIZE],
                             int64_t received) {
    if (side >= ARRAY_SIZE(state.input) || !bitmap) {
        return;
    }
    bool selected = zmk_endpoint_get_selected().transport == ZMK_TRANSPORT_USB;
    k_spinlock_key_t key = k_spin_lock(&state_lock);
    select_route(selected);
    if (defy_hybrid_central_is_maintenance() ||
        (state.input[side].valid && received < state.input[side].received)) {
        k_spin_unlock(&state_lock, key);
        return;
    }
    bool pressed = false;
    for (size_t i = 0; i < DEFY_WIRED_BITMAP_SIZE; i++) {
        pressed |= (bitmap[i] & ~state.input[side].bitmap[i]) != 0;
    }
    bool wake = state.input[side].valid && pressed && state.configured && state.selected &&
                state.suspended && received > state.suspended_at && !state.wake_pending &&
                !state.wake_sent && !state.wake_denied;
    memcpy(state.input[side].bitmap, bitmap, DEFY_WIRED_BITMAP_SIZE);
    state.input[side].valid = true;
    state.input[side].received = received;
    if (wake) {
        state.wake_pending = true;
        state.wake_attempts = 0;
        state.wake_at = 0;
    }
    k_spin_unlock(&state_lock, key);
    if (wake) {
        k_work_reschedule(&retry_work, K_NO_WAIT);
    }
}

/* ZMK asks the host to wake on any key event; only retry_wake() may. */
int __wrap_usb_wakeup_request(void) { return -EAGAIN; }
DEFY_WRAP_CHECK(usb_wakeup_request);

static int send_terminal(enum report_page page) {
    const struct device *device = device_get_binding("HID_0");
    if (!device) {
        return -ENODEV;
    }
    switch (page) {
    case KEYBOARD:
        return hid_int_ep_write(device, (uint8_t *)&terminal_keyboard,
                                sizeof(terminal_keyboard), NULL);
    case CONSUMER:
        return hid_int_ep_write(device, (uint8_t *)&terminal_consumer,
                                sizeof(terminal_consumer), NULL);
#if IS_ENABLED(CONFIG_ZMK_POINTING)
    case MOUSE:
        return hid_int_ep_write(device, (uint8_t *)&terminal_mouse, sizeof(terminal_mouse),
                                NULL);
#endif
    default:
        return -EINVAL;
    }
}

static int send_report(enum report_page page) {
    bool selected = zmk_endpoint_get_selected().transport == ZMK_TRANSPORT_USB;
    k_spinlock_key_t key = k_spin_lock(&state_lock);
    select_route(selected);
    uint32_t usb_generation = state.usb_generation;
    uint32_t route_generation = state.route_generation;
    bool suspended = state.suspended;
    k_spin_unlock(&state_lock, key);

    trace_report("send", page, true, "before", 0);
    int error = suspended ? 0 : send_live(page);
    trace_report("send", page, true, "after", error);
    bool retry = false;

    key = k_spin_lock(&state_lock);
    if (state.usb_generation == usb_generation &&
        state.route_generation == route_generation && state.configured && state.selected) {
        state.revision[page]++;
        if (suspended || state.suspended || transient(error)) {
            state.dirty |= BIT(page);
            retry = true;
            if (!state.suspended && transient(error)) {
                defer_report_retry();
            }
        } else if (!error) {
            state.dirty &= ~BIT(page);
            reset_report_retry();
            retry = state.dirty != 0;
        }
    } else if (!error && !suspended && state.usb_generation == usb_generation &&
               state.route_generation != route_generation && state.configured &&
               !state.selected) {
        /*
         * Output left USB while this send ran, on a thread other than the
         * endpoint change's, so the old host may have it after its release.
         */
        state.terminal |= BIT(page);
        retry = true;
    }
    k_spin_unlock(&state_lock, key);
    if (retry) {
        k_work_reschedule(&retry_work, K_NO_WAIT);
    }
    return error;
}

int __wrap_zmk_usb_hid_send_keyboard_report(void) { return send_report(KEYBOARD); }
DEFY_WRAP_CHECK(zmk_usb_hid_send_keyboard_report);

int __wrap_zmk_usb_hid_send_consumer_report(void) { return send_report(CONSUMER); }
DEFY_WRAP_CHECK(zmk_usb_hid_send_consumer_report);

#if IS_ENABLED(CONFIG_ZMK_POINTING)
/* Called on Zephyr's input thread, where ZMK's input listener runs. */
int __wrap_zmk_usb_hid_send_mouse_report(void) { return send_report(MOUSE); }
DEFY_WRAP_CHECK(zmk_usb_hid_send_mouse_report);
#endif

/*
 * While suspended: asks the host to wake for a newly pressed key, up to
 * WAKE_MAX_ATTEMPTS times. Runs under state_lock. Returns how long until the
 * next attempt, or 0 if there is none.
 */
static int64_t retry_wake(int64_t now) {
    if (!state.selected || !state.wake_pending || state.wake_denied || state.wake_sent ||
        defy_hybrid_central_is_maintenance()) {
        cancel_wake();
        return 0;
    }
    int64_t delay = state.wake_at - now;
    if (delay > 0) {
        return delay;
    }
    state.wake_attempts++;
    int error = __real_usb_wakeup_request();
    bool retry = error == -EAGAIN && state.wake_attempts < WAKE_MAX_ATTEMPTS;
    if (!error) {
        state.wake_sent = true;
    } else if (!retry) {
        state.wake_denied = true;
    }
    if (!retry) {
        cancel_wake();
        return 0;
    }
    state.wake_at = k_uptime_get() + WAKE_RETRY_MS;
    return WAKE_RETRY_MS;
}

static void retry_reports(struct k_work *work) {
    ARG_UNUSED(work);
    int64_t now = k_uptime_get();
    bool selected = zmk_endpoint_get_selected().transport == ZMK_TRANSPORT_USB;
    k_spinlock_key_t key = k_spin_lock(&state_lock);
    select_route(selected);

    if (!state.configured) {
        k_spin_unlock(&state_lock, key);
        return;
    }
    if (state.suspended) {
        int64_t delay = retry_wake(now);
        k_spin_unlock(&state_lock, key);
        if (delay > 0) {
            k_work_reschedule(&retry_work, K_MSEC(delay));
        }
        return;
    }
    uint8_t pending = state.selected ? state.dirty : state.terminal;
    uint32_t usb_generation = state.usb_generation;
    uint32_t route_generation = state.route_generation;
    if (!pending) {
        k_spin_unlock(&state_lock, key);
        return;
    }
    if (state.retry_at > now) {
        int64_t delay = state.retry_at - now;
        k_spin_unlock(&state_lock, key);
        k_work_reschedule(&retry_work, K_MSEC(delay));
        return;
    }
    enum report_page page = find_lsb_set(pending) - 1;
    uint32_t revision = state.revision[page];
    bool live = state.selected;
    k_spin_unlock(&state_lock, key);

    /*
     * Endpoint changes also run on the system work queue, so the route cannot
     * change before this send.
     */
    trace_report("retry", page, live, "before", 0);
    int error = live ? send_live(page) : send_terminal(page);
    trace_report("retry", page, live, "after", error);
    bool again = false;
    int delay = 0;
    key = k_spin_lock(&state_lock);
    if (state.usb_generation == usb_generation &&
        state.route_generation == route_generation && state.configured) {
        if (!error && !state.suspended) {
            reset_report_retry();
        }
        if (!error && !state.suspended && state.revision[page] == revision) {
            if (live) {
                state.dirty &= ~BIT(page);
            } else {
                state.terminal &= ~BIT(page);
            }
        }
        if (transient(error)) {
            defer_report_retry();
            delay = MAX(0, state.retry_at - k_uptime_get());
        }
        again = (!error || transient(error)) &&
                (state.selected ? state.dirty : state.terminal);
    }
    k_spin_unlock(&state_lock, key);
    if (again) {
        k_work_reschedule(&retry_work, K_MSEC(delay));
    }
}

static void usb_status_changed(enum usb_dc_status_code status, const uint8_t *parameters) {
    if (status == USB_DC_SOF) {
        /* Every frame: pass it on without taking the lock. */
        if (original_status_callback) {
            original_status_callback(status, parameters);
        }
        return;
    }
    k_spinlock_key_t key = k_spin_lock(&state_lock);
    switch (status) {
    case USB_DC_RESET:
    case USB_DC_DISCONNECTED:
    case USB_DC_ERROR:
        state.usb_generation++;
        state.dirty = 0;
        state.terminal = 0;
        state.configured = false;
        state.suspended = false;
        reset_wake();
        reset_report_retry();
        break;
    case USB_DC_CONFIGURED:
        if (!state.configured || state.suspended) {
            reset_report_retry();
        }
        state.configured = true;
        state.suspended = false;
        reset_wake();
        break;
    case USB_DC_SUSPEND:
        if (!state.suspended) {
            state.usb_generation++;
            state.suspended = true;
            state.suspended_at = k_uptime_get();
            reset_wake();
        }
        break;
    case USB_DC_RESUME:
        if (state.suspended) {
            reset_report_retry();
            state.usb_generation++;
            if (state.selected) {
                state.dirty |= PAGE_MASK;
            }
        }
        state.suspended = false;
        reset_wake();
        break;
    default:
        break;
    }
    k_spin_unlock(&state_lock, key);
    if (original_status_callback) {
        original_status_callback(status, parameters);
    }
    trace_state("usb", status);
    k_work_reschedule(&retry_work, K_NO_WAIT);
}

int __wrap_usb_enable(usb_dc_status_callback callback) {
    original_status_callback = callback;
    return __real_usb_enable(usb_status_changed);
}
DEFY_WRAP_CHECK(usb_enable);

static int endpoint_changed(const zmk_event_t *event) {
    const struct zmk_endpoint_changed *changed = as_zmk_endpoint_changed(event);
    if (!changed) {
        return ZMK_EV_EVENT_BUBBLE;
    }
    k_spinlock_key_t key = k_spin_lock(&state_lock);
    select_route(changed->endpoint.transport == ZMK_TRANSPORT_USB);
    k_spin_unlock(&state_lock, key);
    trace_state("endpoint", changed->endpoint.transport);
    k_work_reschedule(&retry_work, K_NO_WAIT);
    return ZMK_EV_EVENT_BUBBLE;
}

ZMK_LISTENER(defy_usb_report_recovery, endpoint_changed);
ZMK_SUBSCRIPTION(defy_usb_report_recovery, zmk_endpoint_changed);

#ifdef CONFIG_DEFY_USB_POSITION_TRACE
static int position_changed(const zmk_event_t *event) {
    const struct zmk_position_state_changed *changed = as_zmk_position_state_changed(event);
    if (changed) {
        LOG_INF("position source=%u index=%u pressed=%u", changed->source,
                changed->position, changed->state);
    }
    return ZMK_EV_EVENT_BUBBLE;
}

ZMK_LISTENER(defy_usb_position_trace, position_changed);
ZMK_SUBSCRIPTION(defy_usb_position_trace, zmk_position_state_changed);
#endif
