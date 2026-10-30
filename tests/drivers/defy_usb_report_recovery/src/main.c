/* SPDX-License-Identifier: MIT */

#include <errno.h>
#include <string.h>

#include <zephyr/device.h>
#include <zephyr/kernel.h>
#include <zephyr/ztest.h>

#include <zmk/endpoints.h>
#include <zmk/event_manager.h>
#include <zmk/events/endpoint_changed.h>
#include <zmk/hid.h>
#include <zmk/usb.h>

#include "fake_uptime.h"

/* Scheduled delays convert to milliseconds exactly. */
BUILD_ASSERT(CONFIG_SYS_CLOCK_TICKS_PER_SEC == 1000);

static unsigned int scheduled;
static int64_t scheduled_ms;
static bool retry_pending;
static int64_t retry_due;
static int test_reschedule(struct k_work_delayable *work, k_timeout_t delay) {
    ARG_UNUSED(work);
    scheduled++;
    scheduled_ms = k_ticks_to_ms_floor64(delay.ticks);
    retry_pending = true;
    retry_due = now + scheduled_ms;
    return 1;
}

#define k_work_reschedule test_reschedule
#undef ZMK_SUBSCRIPTION
#define ZMK_SUBSCRIPTION(module, event)
#include "../../../../drivers/usb/defy_usb_report_recovery.c"
#undef k_uptime_get
#undef k_work_reschedule

int test_send_keyboard(void);
int test_send_consumer(void);
int test_send_mouse(void);
int test_usb_enable(usb_dc_status_callback callback);

DEVICE_DEFINE(test_hid, "HID_0", NULL, NULL, NULL, NULL, POST_KERNEL, 10, NULL);

static struct zmk_endpoint_instance selected_endpoint;
static struct zmk_hid_keyboard_report keyboard;
static struct zmk_hid_consumer_report consumer;
static struct zmk_hid_mouse_report mouse;
static enum usb_dc_status_code current_status;
static usb_dc_status_callback installed_callback;
static unsigned int forwarded_statuses;
static unsigned int wake_requests;
static unsigned int original_sends;
static unsigned int write_attempts;
static int wake_error;
static int next_write_error;
static bool busy;
static enum usb_dc_status_code status_during_write;
static bool resume_during_write;
static bool queue_switch_during_write;
static bool release_switch_during_write;
static bool switch_ran;
static bool maintenance;
static uint8_t physical[2][DEFY_WIRED_BITMAP_SIZE];
static enum usb_dc_status_code status_before_send;
static struct k_sem switched;

static struct {
    uint8_t data[32];
    uint32_t length;
    bool terminal;
} writes[64];
static unsigned int write_count;

struct zmk_endpoint_instance zmk_endpoint_get_selected(void) { return selected_endpoint; }

struct zmk_endpoint_changed *as_zmk_endpoint_changed(const zmk_event_t *event) {
    return &((struct zmk_endpoint_changed_event *)event)->data;
}

static void complete(void);

static void route(enum zmk_transport transport) {
    selected_endpoint.transport = transport;
    struct zmk_endpoint_changed_event event = {.data.endpoint = selected_endpoint};
    endpoint_changed(&event.header);
}

static void original_status(enum usb_dc_status_code status, const uint8_t *parameters) {
    ARG_UNUSED(parameters);
    current_status = status;
    forwarded_statuses++;
}

int usb_enable(usb_dc_status_callback callback) {
    installed_callback = callback;
    return 0;
}

int fake_wakeup_request(void) {
    wake_requests++;
    return wake_error;
}

bool defy_hybrid_central_is_maintenance(void) { return maintenance; }

static void input(uint8_t side, uint8_t position, bool pressed) {
    WRITE_BIT(physical[side][position / 8], position % 8, pressed);
    defy_usb_input_snapshot(side, physical[side], ++now);
}

static void status(enum usb_dc_status_code value) {
    if (value == USB_DC_RESET || value == USB_DC_DISCONNECTED) {
        busy = false;
    }
    installed_callback(value, NULL);
}

static void switch_back_work(struct k_work *work) {
    ARG_UNUSED(work);
    route(ZMK_TRANSPORT_USB);
    keyboard.body.keys[0] = 9;
    zassert_equal(test_send_keyboard(), -EAGAIN);
    switch_ran = true;
    k_sem_give(&switched);
}
static K_WORK_DEFINE(switch_work, switch_back_work);

int hid_int_ep_write(const struct device *device, const uint8_t *data, uint32_t length,
                     uint32_t *written) {
    ARG_UNUSED(written);
    zassert_equal(device, DEVICE_GET(test_hid));
    write_attempts++;
    if (status_during_write != USB_DC_UNKNOWN) {
        enum usb_dc_status_code value = status_during_write;
        status_during_write = USB_DC_UNKNOWN;
        status(value);
    }
    if (resume_during_write) {
        resume_during_write = false;
        status(USB_DC_SUSPEND);
        status(USB_DC_RESUME);
    }
    if (release_switch_during_write) {
        /* Output leaves USB, and every release reaches the host, before this write. */
        release_switch_during_write = false;
        route(ZMK_TRANSPORT_BLE);
        for (int i = 0; i < PAGE_COUNT; i++) {
            retry_reports(&retry_work.work);
            complete();
        }
        zassert_false(state.terminal);
    }
    if (next_write_error) {
        int error = next_write_error;
        next_write_error = 0;
        return error;
    }
    if (current_status == USB_DC_RESET || current_status == USB_DC_DISCONNECTED) {
        return -ENODEV;
    }
    if (current_status == USB_DC_SUSPEND || busy) {
        return -EAGAIN;
    }
    zassert_true(write_count < ARRAY_SIZE(writes));
    zassert_true(length <= sizeof(writes[0].data));
    memcpy(writes[write_count].data, data, length);
    writes[write_count].length = length;
    writes[write_count].terminal = data == (uint8_t *)&terminal_keyboard ||
                                  data == (uint8_t *)&terminal_consumer ||
                                  data == (uint8_t *)&terminal_mouse;
    write_count++;
    busy = true;
    if (queue_switch_during_write) {
        queue_switch_during_write = false;
        k_work_submit(&switch_work);
        k_sleep(K_MSEC(2));
        zassert_false(switch_ran);
        zassert_equal(selected_endpoint.transport, ZMK_TRANSPORT_BLE);
    }
    return 0;
}

int zmk_usb_hid_send_keyboard_report(void) {
    original_sends++;
    if (status_before_send != USB_DC_UNKNOWN) {
        enum usb_dc_status_code value = status_before_send;
        status_before_send = USB_DC_UNKNOWN;
        status(value);
    }
    if (current_status == USB_DC_SUSPEND) {
        return usb_wakeup_request();
    }
    return hid_int_ep_write(DEVICE_GET(test_hid), (uint8_t *)&keyboard, sizeof(keyboard), NULL);
}

int zmk_usb_hid_send_consumer_report(void) {
    original_sends++;
    if (current_status == USB_DC_SUSPEND) {
        return usb_wakeup_request();
    }
    return hid_int_ep_write(DEVICE_GET(test_hid), (uint8_t *)&consumer, sizeof(consumer), NULL);
}

/* As ZMK's input listener sends: the movement goes once, then ZMK clears it. */
int zmk_usb_hid_send_mouse_report(void) {
    original_sends++;
    if (current_status == USB_DC_SUSPEND) {
        return usb_wakeup_request();
    }
    return hid_int_ep_write(DEVICE_GET(test_hid), (uint8_t *)&mouse, sizeof(mouse), NULL);
}

static int send_mouse(zmk_mouse_button_flags_t buttons, int16_t d_x) {
    mouse.body.buttons = buttons;
    mouse.body.d_x = d_x;
    int error = test_send_mouse();
    mouse.body.d_x = 0;
    return error;
}

static void complete(void) {
    busy = false;
}

/* Advances the clock, then runs the retry work only if the driver scheduled it by then. */
static void run_retry(int64_t advance) {
    now += advance;
    if (retry_pending && retry_due <= now) {
        retry_pending = false;
        retry_reports(&retry_work.work);
    }
}

/* Runs scheduled retries a millisecond at a time until the write lands or a second passes. */
static int64_t run_until_written(unsigned int count) {
    int64_t start = now;
    while (write_count < count && now - start < 1000) {
        run_retry(1);
    }
    zassert_equal(write_count, count);
    return now - start;
}

static void reset(void *fixture) {
    ARG_UNUSED(fixture);
    memset(&state, 0, sizeof(state));
    memset(&keyboard, 0, sizeof(keyboard));
    memset(&consumer, 0, sizeof(consumer));
    memset(writes, 0, sizeof(writes));
    keyboard.report_id = ZMK_HID_REPORT_ID_KEYBOARD;
    consumer.report_id = ZMK_HID_REPORT_ID_CONSUMER;
    memset(&mouse, 0, sizeof(mouse));
    mouse.report_id = ZMK_HID_REPORT_ID_MOUSE;
    now = 1000;
    scheduled = 0;
    scheduled_ms = 0;
    retry_pending = false;
    retry_due = 0;
    forwarded_statuses = 0;
    wake_requests = 0;
    original_sends = 0;
    write_attempts = 0;
    wake_error = 0;
    next_write_error = 0;
    write_count = 0;
    busy = false;
    status_during_write = USB_DC_UNKNOWN;
    resume_during_write = false;
    queue_switch_during_write = false;
    release_switch_during_write = false;
    switch_ran = false;
    maintenance = false;
    status_before_send = USB_DC_UNKNOWN;
    memset(physical, 0, sizeof(physical));
    k_sem_init(&switched, 0, 1);
    selected_endpoint.transport = ZMK_TRANSPORT_NONE;
    zassert_ok(test_usb_enable(original_status));
    zassert_equal(installed_callback, usb_status_changed);
    status(USB_DC_CONFIGURED);
    route(ZMK_TRANSPORT_USB);
    defy_usb_input_snapshot(0, physical[0], now);
    defy_usb_input_snapshot(1, physical[1], now);
}

static void assert_released(unsigned int index) {
    zassert_true(index < write_count);
    for (uint32_t i = 1; i < writes[index].length; i++) {
        zassert_equal(writes[index].data[i], 0);
    }
}

static const uint8_t report_ids[PAGE_COUNT] = {
    [KEYBOARD] = ZMK_HID_REPORT_ID_KEYBOARD,
    [CONSUMER] = ZMK_HID_REPORT_ID_CONSUMER,
    [MOUSE] = ZMK_HID_REPORT_ID_MOUSE,
};

static int send_page(enum report_page page) {
    switch (page) {
    case KEYBOARD:
        return test_send_keyboard();
    case CONSUMER:
        return test_send_consumer();
    case MOUSE:
        return test_send_mouse();
    default:
        return -EINVAL;
    }
}

ZTEST(defy_usb_report_recovery, test_held_modifier_resubmitted_after_resume) {
    status(USB_DC_SUSPEND);
    input(0, 0, true);
    keyboard.body.modifiers = BIT(1);
    zassert_ok(test_send_keyboard());
    zassert_equal(wake_requests, 0);
    zassert_equal(write_count, 0);
    run_retry(0);
    zassert_equal(wake_requests, 1);
    status(USB_DC_RESUME);
    run_retry(0);
    zassert_equal(write_count, 1);
    zassert_equal(writes[0].data[1], BIT(1));
    zassert_false(state.dirty & BIT(KEYBOARD));
    complete();
    keyboard.body.modifiers = 0;
    zassert_ok(test_send_keyboard());
    assert_released(1);
}

ZTEST(defy_usb_report_recovery, test_modifier_held_before_suspend_is_refreshed) {
    keyboard.body.modifiers = BIT(1);
    zassert_ok(test_send_keyboard());
    complete();
    status(USB_DC_SUSPEND);
    status(USB_DC_RESUME);
    run_retry(0);
    zassert_equal(write_count, 2);
    zassert_equal(writes[1].data[1], BIT(1));
}

ZTEST(defy_usb_report_recovery, test_resume_during_normal_send_preserves_refresh) {
    keyboard.body.modifiers = BIT(1);
    resume_during_write = true;
    zassert_ok(test_send_keyboard());
    zassert_true(state.dirty & BIT(KEYBOARD));
    complete();
    run_retry(0);
    zassert_equal(write_count, 2);
    zassert_equal(writes[1].data[1], BIT(1));
}

ZTEST(defy_usb_report_recovery, test_resume_during_retry_preserves_refresh) {
    keyboard.body.modifiers = BIT(1);
    next_write_error = -EIO;
    zassert_equal(test_send_keyboard(), -EIO);
    resume_during_write = true;
    run_retry(RETRY_MS);
    zassert_true(state.dirty & BIT(KEYBOARD));
    complete();
    run_retry(0);
    zassert_equal(write_count, 2);
    zassert_equal(writes[1].data[1], BIT(1));
}

ZTEST(defy_usb_report_recovery, test_resume_during_terminal_preserves_release_obligation) {
    route(ZMK_TRANSPORT_BLE);
    resume_during_write = true;
    run_retry(0);
    zassert_true(state.terminal);
    zassert_false(state.dirty);
    zassert_false(state.selected);
    complete();
    for (int i = 0; i < PAGE_COUNT; i++) {
        run_retry(RETRY_MS);
        complete();
    }
    zassert_false(state.terminal);
    for (unsigned int i = 0; i < write_count; i++) {
        assert_released(i);
    }
}

ZTEST(defy_usb_report_recovery, test_consumer_state_recovers) {
    status(USB_DC_SUSPEND);
    consumer.body.keys[0] = 0xe9;
    zassert_ok(test_send_consumer());
    status(USB_DC_RESUME);
    run_retry(0);
    zassert_equal(write_count, 1);
    assert_released(0);
    complete();
    run_retry(0);
    zassert_mem_equal(writes[1].data, &consumer, sizeof(consumer));
    complete();
    consumer.body.keys[0] = 0;
    next_write_error = -EIO;
    zassert_equal(test_send_consumer(), -EIO);
    run_retry(RETRY_MS);
    assert_released(2);
}

ZTEST(defy_usb_report_recovery, test_spurious_resume_does_not_resend) {
    status(USB_DC_RESUME);
    run_retry(0);
    zassert_equal(write_count, 0);
}

ZTEST(defy_usb_report_recovery, test_persistent_errors_back_off_without_losing_release) {
    const int errors[] = {-EAGAIN, -EIO};
    for (size_t i = 0; i < ARRAY_SIZE(errors); i++) {
        reset(NULL);
        keyboard.body.keys[0] = 4;
        next_write_error = errors[i];
        zassert_equal(test_send_keyboard(), errors[i]);
        for (int elapsed = 0; elapsed < 10000; elapsed++) {
            next_write_error = errors[i];
            run_retry(1);
        }
        zassert_true(write_attempts >= 10000 / RETRY_MAX_MS);
        zassert_true(write_attempts <= 10000 / RETRY_MAX_MS + 8);
        zassert_equal(write_count, 0);
        zassert_true(state.dirty & BIT(KEYBOARD));

        keyboard.body.keys[0] = 0;
        next_write_error = errors[i];
        zassert_equal(test_send_keyboard(), errors[i]);
        next_write_error = 0;
        zassert_true(run_until_written(1) <= 100);
        assert_released(0);
        zassert_false(state.dirty);

        complete();
        next_write_error = errors[i];
        zassert_equal(test_send_keyboard(), errors[i]);
        run_retry(5);
        zassert_equal(write_count, 2);
    }
}

ZTEST(defy_usb_report_recovery, test_new_reports_and_duplicate_status_preserve_backoff) {
    busy = true;
    zassert_equal(test_send_keyboard(), -EAGAIN);
    run_retry(5);
    unsigned int attempts = write_attempts;
    now += 3;
    keyboard.body.keys[0] = 4;
    zassert_equal(test_send_keyboard(), -EAGAIN);
    status(USB_DC_CONFIGURED);
    status(USB_DC_RESUME);
    route(ZMK_TRANSPORT_USB);
    run_retry(0);
    zassert_equal(write_attempts, attempts + 1);
    complete();
    run_retry(6);
    zassert_equal(write_count, 0);
    run_retry(1);
    zassert_equal(write_count, 1);
    zassert_mem_equal(writes[0].data, &keyboard, sizeof(keyboard));
}

ZTEST(defy_usb_report_recovery, test_fresh_success_recovers_other_page_without_old_delay) {
    consumer.body.keys[0] = 0xe9;
    zassert_ok(test_send_consumer());
    consumer.body.keys[0] = 0;
    zassert_equal(test_send_consumer(), -EAGAIN);
    for (int elapsed = 0; elapsed < 3000; elapsed++) {
        run_retry(1);
    }
    zassert_equal(state.retry_delay_ms, RETRY_MAX_MS);
    zassert_true(scheduled_ms > 0);

    complete();
    keyboard.body.keys[0] = 4;
    zassert_ok(test_send_keyboard());
    zassert_equal(write_count, 2);
    zassert_mem_equal(writes[1].data, &keyboard, sizeof(keyboard));
    zassert_equal(state.retry_at, 0);
    zassert_equal(state.retry_delay_ms, 0);
    zassert_true(state.dirty & BIT(CONSUMER));
    zassert_equal(scheduled_ms, 0);

    complete();
    run_retry(0);
    zassert_equal(write_count, 3);
    assert_released(2);
    zassert_false(state.dirty);

    complete();
    next_write_error = -EIO;
    zassert_equal(test_send_keyboard(), -EIO);
    run_retry(5);
    zassert_equal(write_count, 4);
}

ZTEST(defy_usb_report_recovery, test_backoff_preserves_terminal_release) {
    keyboard.body.keys[0] = 4;
    zassert_ok(test_send_keyboard());
    route(ZMK_TRANSPORT_BLE);
    keyboard.body.keys[0] = 9;
    for (int elapsed = 0; elapsed < 10000; elapsed++) {
        run_retry(1);
    }
    zassert_true(write_attempts >= 10000 / RETRY_MAX_MS);
    zassert_true(write_attempts <= 10000 / RETRY_MAX_MS + 8);
    zassert_equal(write_count, 1);
    zassert_equal(state.terminal, PAGE_MASK);
    complete();
    zassert_true(run_until_written(2) <= 100);
    zassert_true(writes[1].terminal);
    assert_released(1);
    for (int page = CONSUMER; page < PAGE_COUNT; page++) {
        complete();
        run_retry(0);
        zassert_true(writes[page + 1].terminal);
        assert_released(page + 1);
    }
    zassert_false(state.terminal);
    zassert_equal(keyboard.body.keys[0], 9);
}

ZTEST(defy_usb_report_recovery, test_resume_restarts_report_recovery_without_backoff) {
    busy = true;
    zassert_equal(test_send_keyboard(), -EAGAIN);
    for (int elapsed = 0; elapsed < 3000; elapsed++) {
        run_retry(1);
    }
    status(USB_DC_SUSPEND);
    complete();
    status(USB_DC_RESUME);
    for (int page = 0; page < PAGE_COUNT; page++) {
        run_retry(0);
        zassert_equal(write_count, page + 1);
        assert_released(page);
        complete();
    }
    zassert_false(state.dirty);
}

ZTEST(defy_usb_report_recovery, test_reconfiguration_cancels_old_backoff) {
    const enum usb_dc_status_code transitions[] = {USB_DC_RESET, USB_DC_DISCONNECTED,
                                                   USB_DC_ERROR};
    for (size_t i = 0; i < ARRAY_SIZE(transitions); i++) {
        reset(NULL);
        busy = true;
        zassert_equal(test_send_keyboard(), -EAGAIN);
        for (int elapsed = 0; elapsed < 3000; elapsed++) {
            run_retry(1);
        }
        status(transitions[i]);
        complete();
        status(USB_DC_CONFIGURED);
        run_retry(1000);
        zassert_equal(write_count, 0);
        next_write_error = -EIO;
        zassert_equal(test_send_keyboard(), -EIO);
        run_retry(5);
        zassert_equal(write_count, 1);
    }
}

ZTEST(defy_usb_report_recovery, test_wake_retry_is_rate_limited) {
    status(USB_DC_SUSPEND);
    wake_error = -EAGAIN;
    input(0, 0, true);
    zassert_ok(test_send_keyboard());
    run_retry(0);
    run_retry(WAKE_RETRY_MS - 1);
    zassert_equal(wake_requests, 1);
    run_retry(1);
    zassert_equal(wake_requests, 2);
    run_retry(0);
    zassert_equal(wake_requests, 2);
}

ZTEST(defy_usb_report_recovery, test_reset_cancels_pending_and_terminal_recovery) {
    status(USB_DC_SUSPEND);
    input(0, 0, true);
    zassert_ok(test_send_keyboard());
    status(USB_DC_RESET);
    zassert_equal(state.dirty, 0);
    zassert_equal(state.terminal, 0);
    run_retry(WAKE_RETRY_MS);
    zassert_equal(wake_requests, 0);
    status(USB_DC_CONFIGURED);
    run_retry(0);
    zassert_equal(write_count, 0);
}

ZTEST(defy_usb_report_recovery, test_disconnect_cancels_busy_terminal) {
    route(ZMK_TRANSPORT_BLE);
    run_retry(0);
    zassert_true(busy);
    zassert_true(state.terminal);
    status(USB_DC_DISCONNECTED);
    zassert_equal(state.terminal, 0);
    status(USB_DC_CONFIGURED);
    run_retry(0);
    zassert_equal(write_count, 1);
}

ZTEST(defy_usb_report_recovery, test_reset_during_retry_cannot_restore_dirty_state) {
    next_write_error = -EIO;
    zassert_equal(test_send_keyboard(), -EIO);
    status_during_write = USB_DC_RESET;
    run_retry(RETRY_MS);
    zassert_equal(state.dirty, 0);
    zassert_false(state.configured);
}

ZTEST(defy_usb_report_recovery, test_switch_away_during_suspend_does_not_wake_old_host) {
    status(USB_DC_SUSPEND);
    input(0, 0, true);
    zassert_ok(test_send_keyboard());
    route(ZMK_TRANSPORT_BLE);
    run_retry(WAKE_RETRY_MS);
    zassert_equal(wake_requests, 0);
    zassert_equal(write_count, 0);
    status(USB_DC_RESUME);
    run_retry(0);
    assert_released(0);
    complete();
    run_retry(0);
    assert_released(1);
}

ZTEST(defy_usb_report_recovery, test_switch_back_cancels_pending_terminal) {
    route(ZMK_TRANSPORT_BLE);
    route(ZMK_TRANSPORT_USB);
    keyboard.body.keys[0] = 9;
    zassert_ok(test_send_keyboard());
    run_retry(RETRY_MS);
    zassert_equal(write_count, 1);
    zassert_false(writes[0].terminal);
    zassert_equal(state.terminal, 0);
}

static void execute_retry_work(struct k_work *work) {
    ARG_UNUSED(work);
    retry_reports(&retry_work.work);
}
static K_WORK_DEFINE(execute_work, execute_retry_work);

ZTEST(defy_usb_report_recovery, test_switch_back_cannot_interleave_terminal_write) {
    route(ZMK_TRANSPORT_BLE);
    queue_switch_during_write = true;
    k_work_submit(&execute_work);
    zassert_ok(k_sem_take(&switched, K_SECONDS(1)));
    zassert_equal(write_count, 1);
    zassert_true(writes[0].terminal);
    zassert_equal(state.terminal, 0);
    zassert_true(state.dirty);
    complete();
    run_retry(RETRY_MS);
    zassert_equal(write_count, 2);
    zassert_false(writes[1].terminal);
    zassert_equal(writes[1].data[3], 9);
    for (int page = CONSUMER; page < PAGE_COUNT; page++) {
        complete();
        run_retry(RETRY_MS);
        zassert_equal(write_count, page + 2);
        zassert_false(writes[page + 1].terminal);
        zassert_equal(writes[page + 1].data[0], report_ids[page]);
        assert_released(page + 1);
    }
    complete();
    run_retry(RETRY_MS);
    zassert_equal(write_count, PAGE_COUNT + 1);
    zassert_false(state.dirty);
}

ZTEST(defy_usb_report_recovery, test_route_round_trip_keeps_release_owed_to_usb_host) {
    keyboard.body.keys[0] = 4;
    zassert_ok(test_send_keyboard());
    keyboard.body.keys[0] = 0;
    for (int page = 0; page < PAGE_COUNT; page++) {
        zassert_equal(send_page(page), -EAGAIN);
    }
    run_retry(RETRY_MS);
    zassert_equal(state.dirty, PAGE_MASK);
    route(ZMK_TRANSPORT_BLE);
    run_retry(0);
    zassert_equal(state.terminal, PAGE_MASK);
    route(ZMK_TRANSPORT_USB);
    zassert_equal(state.terminal, 0);
    for (int page = 0; page < PAGE_COUNT; page++) {
        complete();
        run_until_written(page + 2);
        zassert_false(writes[page + 1].terminal);
        zassert_equal(writes[page + 1].data[0], report_ids[page]);
        assert_released(page + 1);
    }
    zassert_false(state.dirty);
}

ZTEST(defy_usb_report_recovery, test_delivered_terminal_needs_no_resend_on_return) {
    route(ZMK_TRANSPORT_BLE);
    for (int page = 0; page < PAGE_COUNT; page++) {
        run_retry(0);
        complete();
    }
    zassert_equal(write_count, PAGE_COUNT);
    zassert_false(state.terminal);
    route(ZMK_TRANSPORT_USB);
    zassert_false(state.dirty);
    run_retry(RETRY_MS);
    zassert_equal(write_count, PAGE_COUNT);
}

ZTEST(defy_usb_report_recovery, test_configured_initial_selection_without_event) {
    state.selected = false;
    selected_endpoint.transport = ZMK_TRANSPORT_USB;
    status(USB_DC_SUSPEND);
    keyboard.body.keys[0] = 4;
    zassert_ok(test_send_keyboard());
    zassert_true(state.dirty);
    status(USB_DC_RESUME);
    run_retry(0);
    zassert_equal(write_count, 1);
    zassert_equal(writes[0].data[3], 4);
}

ZTEST(defy_usb_report_recovery, test_sof_does_not_schedule_recovery) {
    unsigned int before = scheduled;
    unsigned int forwarded = forwarded_statuses;
    status(USB_DC_SOF);
    zassert_equal(scheduled, before);
    zassert_equal(forwarded_statuses, forwarded + 1);
}

ZTEST(defy_usb_report_recovery, test_old_dirty_report_cannot_wake) {
    keyboard.body.keys[0] = 4;
    next_write_error = -EIO;
    zassert_equal(test_send_keyboard(), -EIO);
    status(USB_DC_SUSPEND);
    run_retry(RETRY_MS);
    run_retry(WAKE_RETRY_MS);
    zassert_equal(wake_requests, 0);
    zassert_true(state.dirty);
    status(USB_DC_RESUME);
    run_retry(0);
    zassert_equal(write_count, 1);
    zassert_equal(writes[0].data[3], 4);
}

ZTEST(defy_usb_report_recovery, test_release_and_empty_reports_cannot_wake) {
    input(0, 0, true);
    status(USB_DC_SUSPEND);
    input(0, 0, false);
    zassert_ok(test_send_keyboard());
    zassert_ok(test_send_consumer());
    run_retry(0);
    zassert_equal(wake_requests, 0);
    zassert_equal(usb_wakeup_request(), -EAGAIN);
    zassert_equal(wake_requests, 0);
}

ZTEST(defy_usb_report_recovery, test_suspend_during_send_cannot_bypass_wake_guard) {
    status_before_send = USB_DC_SUSPEND;
    keyboard.body.keys[0] = 4;
    zassert_equal(test_send_keyboard(), -EAGAIN);
    run_retry(WAKE_RETRY_MS);
    zassert_equal(wake_requests, 0);
    status(USB_DC_RESUME);
    run_retry(0);
    zassert_equal(write_count, 1);
    zassert_equal(writes[0].data[3], 4);
}

ZTEST(defy_usb_report_recovery, test_wake_success_is_consumed_until_next_suspend) {
    status(USB_DC_SUSPEND);
    input(0, 0, true);
    run_retry(0);
    zassert_equal(wake_requests, 1);
    input(1, 35, true);
    zassert_ok(test_send_keyboard());
    status(USB_DC_SUSPEND);
    for (int i = 0; i < 5; i++) {
        run_retry(WAKE_RETRY_MS);
    }
    zassert_equal(wake_requests, 1);
    status(USB_DC_RESUME);
    status(USB_DC_SUSPEND);
    input(0, 1, true);
    run_retry(0);
    zassert_equal(wake_requests, 2);
}

ZTEST(defy_usb_report_recovery, test_wake_retry_budget_survives_duplicate_suspend) {
    status(USB_DC_SUSPEND);
    wake_error = -EAGAIN;
    input(0, 0, true);
    for (int i = 0; i < WAKE_MAX_ATTEMPTS + 3; i++) {
        run_retry(WAKE_RETRY_MS);
        status(USB_DC_SUSPEND);
    }
    input(1, 35, true);
    run_retry(WAKE_RETRY_MS);
    zassert_equal(wake_requests, WAKE_MAX_ATTEMPTS);
    zassert_true(state.wake_denied);
}

ZTEST(defy_usb_report_recovery, test_nontransient_wake_errors_stop_retries) {
    const int errors[] = {-EACCES, -ENOTSUP, -EIO};
    for (size_t i = 0; i < ARRAY_SIZE(errors); i++) {
        reset(NULL);
        status(USB_DC_SUSPEND);
        wake_error = errors[i];
        input(0, 0, true);
        run_retry(0);
        status(USB_DC_SUSPEND);
        input(0, 1, true);
        run_retry(WAKE_RETRY_MS);
        zassert_equal(wake_requests, 1);
    }
}

ZTEST(defy_usb_report_recovery, test_layer_key_can_wake_without_hid_report) {
    status(USB_DC_SUSPEND);
    input(0, 20, true);
    zassert_equal(state.dirty, 0);
    run_retry(0);
    zassert_equal(wake_requests, 1);
    zassert_equal(original_sends, 0);
}

ZTEST(defy_usb_report_recovery, test_short_physical_tap_can_wake_without_replay) {
    status(USB_DC_SUSPEND);
    input(0, 0, true);
    keyboard.body.keys[0] = 4;
    zassert_ok(test_send_keyboard());
    input(0, 0, false);
    keyboard.body.keys[0] = 0;
    zassert_ok(test_send_keyboard());
    run_retry(0);
    zassert_equal(wake_requests, 1);
    status(USB_DC_RESUME);
    run_retry(0);
    assert_released(0);
}

ZTEST(defy_usb_report_recovery, test_queued_before_suspend_input_cannot_wake) {
    int64_t received = now;
    now += 10;
    status(USB_DC_SUSPEND);
    physical[0][0] = BIT(0);
    defy_usb_input_snapshot(0, physical[0], received);
    zassert_ok(test_send_keyboard());
    run_retry(0);
    zassert_equal(wake_requests, 0);
}

ZTEST(defy_usb_report_recovery, test_restored_held_state_cannot_wake) {
    input(0, 0, true);
    status(USB_DC_SUSPEND);
    defy_usb_input_snapshot(0, physical[0], ++now);
    keyboard.body.keys[0] = 4;
    zassert_ok(test_send_keyboard());
    run_retry(0);
    zassert_equal(wake_requests, 0);
    input(0, 0, false);
    input(0, 0, true);
    run_retry(0);
    zassert_equal(wake_requests, 1);
}

ZTEST(defy_usb_report_recovery, test_first_snapshot_after_reset_is_baseline_only) {
    status(USB_DC_SUSPEND);
    input(0, 0, true);
    defy_usb_input_reset();
    defy_usb_input_snapshot(0, physical[0], ++now);
    run_retry(0);
    zassert_equal(wake_requests, 0);
    input(0, 0, false);
    input(0, 0, true);
    run_retry(0);
    zassert_equal(wake_requests, 1);
}

ZTEST(defy_usb_report_recovery, test_stale_snapshot_cannot_replace_newer_baseline) {
    int64_t old = now;
    input(0, 0, true);
    status(USB_DC_SUSPEND);
    const uint8_t released[DEFY_WIRED_BITMAP_SIZE] = {0};
    defy_usb_input_snapshot(0, released, old);
    defy_usb_input_snapshot(0, physical[0], ++now);
    run_retry(0);
    zassert_equal(wake_requests, 0);
}

ZTEST(defy_usb_report_recovery, test_maintenance_blocks_already_queued_wake) {
    status(USB_DC_SUSPEND);
    input(0, 0, true);
    maintenance = true;
    run_retry(0);
    input(0, 1, true);
    run_retry(0);
    zassert_equal(wake_requests, 0);
    defy_usb_input_reset();
    maintenance = false;
    defy_usb_input_snapshot(0, physical[0], ++now);
    run_retry(0);
    zassert_equal(wake_requests, 0);
}

ZTEST(defy_usb_report_recovery, test_disconnect_and_resume_cancel_queued_wake) {
    status(USB_DC_SUSPEND);
    input(0, 0, true);
    status(USB_DC_DISCONNECTED);
    run_retry(0);
    zassert_equal(wake_requests, 0);
    status(USB_DC_CONFIGURED);
    status(USB_DC_SUSPEND);
    input(0, 1, true);
    status(USB_DC_RESUME);
    status(USB_DC_SUSPEND);
    run_retry(0);
    zassert_equal(wake_requests, 0);
}

ZTEST(defy_usb_report_recovery, test_bluetooth_press_cannot_wake_after_usb_selection) {
    route(ZMK_TRANSPORT_BLE);
    status(USB_DC_SUSPEND);
    input(0, 0, true);
    route(ZMK_TRANSPORT_USB);
    run_retry(0);
    zassert_equal(wake_requests, 0);
    input(0, 1, true);
    run_retry(0);
    zassert_equal(wake_requests, 1);
}

static const struct zmk_hid_mouse_report *mouse_write(unsigned int index) {
    zassert_true(index < write_count);
    zassert_equal(writes[index].length, sizeof(struct zmk_hid_mouse_report));
    zassert_equal(writes[index].data[0], ZMK_HID_REPORT_ID_MOUSE);
    return (const struct zmk_hid_mouse_report *)writes[index].data;
}

ZTEST(defy_usb_report_recovery, test_mouse_button_held_through_suspend_is_resent_without_movement) {
    status(USB_DC_SUSPEND);
    input(0, 0, true);
    zassert_ok(send_mouse(BIT(0), 12));
    zassert_equal(write_count, 0);
    zassert_true(state.dirty & BIT(MOUSE));
    run_retry(0);
    zassert_equal(wake_requests, 1);
    status(USB_DC_RESUME);
    for (int i = 0; i < PAGE_COUNT; i++) {
        run_until_written(i + 1);
        complete();
    }
    const struct zmk_hid_mouse_report *resent = mouse_write(MOUSE);
    zassert_false(writes[MOUSE].terminal);
    zassert_equal(resent->body.buttons, BIT(0));
    zassert_equal(resent->body.d_x, 0);
    zassert_false(state.dirty);
}

ZTEST(defy_usb_report_recovery, test_failed_mouse_release_is_retried) {
    zassert_ok(send_mouse(BIT(0), 0));
    complete();
    next_write_error = -EIO;
    zassert_equal(send_mouse(0, 3), -EIO);
    zassert_equal(state.dirty, BIT(MOUSE));
    run_retry(RETRY_MS);
    zassert_equal(write_count, 2);
    assert_released(1);
    zassert_false(state.dirty);
}

ZTEST(defy_usb_report_recovery, test_output_switch_releases_mouse_buttons_on_usb_host) {
    zassert_ok(send_mouse(BIT(1), 0));
    complete();
    route(ZMK_TRANSPORT_BLE);
    mouse.body.buttons = BIT(1);
    for (int i = 1; i <= PAGE_COUNT; i++) {
        run_until_written(i + 1);
        complete();
    }
    mouse_write(PAGE_COUNT);
    zassert_true(writes[PAGE_COUNT].terminal);
    assert_released(PAGE_COUNT);
    zassert_false(state.terminal);
}

ZTEST(defy_usb_report_recovery, test_mouse_send_overtaken_by_output_switch_is_released_again) {
    release_switch_during_write = true;
    zassert_ok(send_mouse(BIT(0), 0));
    zassert_equal(write_count, PAGE_COUNT + 1);
    zassert_false(writes[PAGE_COUNT].terminal);
    zassert_equal(mouse_write(PAGE_COUNT)->body.buttons, BIT(0));
    zassert_equal(state.terminal, BIT(MOUSE));
    complete();
    run_until_written(PAGE_COUNT + 2);
    zassert_true(writes[PAGE_COUNT + 1].terminal);
    mouse_write(PAGE_COUNT + 1);
    assert_released(PAGE_COUNT + 1);
    zassert_false(state.terminal);
}

ZTEST_SUITE(defy_usb_report_recovery, NULL, NULL, reset, NULL, NULL);
