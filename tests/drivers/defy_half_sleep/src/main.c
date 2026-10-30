/* SPDX-License-Identifier: MIT */

#include <errno.h>
#include <setjmp.h>
#include <string.h>

#include <zephyr/device.h>
#include <zephyr/init.h>
#include <zephyr/kernel.h>
#include <zephyr/pm/device.h>
#include <zephyr/pm/device_runtime.h>
#include <zephyr/sys/reboot.h>
#include <zephyr/ztest.h>

#include <dygma/defy_battery.h>

struct fake_device {
    enum pm_device_state state;
    bool ready;
    bool busy;
    bool wake;
    bool runtime;
    int state_error;
};

static struct fake_device radio_data, matrix_data, led_data, key_led_data;

#define DEFINE_FAKE(label, data) DEVICE_DT_DEFINE(DT_NODELABEL(label), NULL, NULL, &data, NULL, POST_KERNEL, 10, NULL)

DEFINE_FAKE(radio, radio_data);
DEFINE_FAKE(kscan0, matrix_data);
DEFINE_FAKE(defy_leds, led_data);
DEFINE_FAKE(key_leds, key_led_data);

static bool fake_ready(const struct device *device) {
    return ((struct fake_device *)device->data)->ready;
}

static int fake_state(const struct device *device, enum pm_device_state *state) {
    struct fake_device *data = device->data;
    *state = data->state;
    return data->state_error;
}

static bool fake_busy(const struct device *device) {
    return ((struct fake_device *)device->data)->busy;
}

static bool fake_wake(const struct device *device) {
    return ((struct fake_device *)device->data)->wake;
}

static bool fake_runtime(const struct device *device) {
    return ((struct fake_device *)device->data)->runtime;
}

static unsigned int schedules;
static int64_t scheduled_ms;
static int fake_schedule(struct k_work_delayable *work, k_timeout_t delay) {
    ARG_UNUSED(work);
    schedules++;
    scheduled_ms = k_ticks_to_ms_floor64(delay.ticks);
    return 1;
}

#define device_is_ready fake_ready
#define pm_device_state_get fake_state
#define pm_device_is_busy fake_busy
#define pm_device_wakeup_is_enabled fake_wake
#define pm_device_runtime_is_enabled fake_runtime
#define k_work_reschedule fake_schedule
#include "../../../../power/defy_half_sleep.c"
#undef device_is_ready
#undef pm_device_state_get
#undef pm_device_is_busy
#undef pm_device_wakeup_is_enabled
#undef pm_device_runtime_is_enabled
#undef k_work_reschedule

enum action {
    PREPARE_TRANSPORT = 1,
    PREPARE_MATRIX,
    CHECK_PENDING,
    CANCEL_MATRIX,
    CANCEL_TRANSPORT,
    SUSPEND_DEVICES,
    COLD_REBOOT,
    ENTER_DORMANT,
};

static enum action actions[32];
static size_t action_count;
static int transport_error, matrix_error, cancel_error, suspend_error;
static int64_t idle_argument;
static bool pending_before_pm, pending_after_pm;
static bool pm_started, suspend_matrix, disable_matrix_wake;
static bool battery_overlay;
static const struct device *skip_suspend;
static const struct device *state_error_after_pm;
static jmp_buf terminal;

bool defy_half_leds_battery_active(void) { return battery_overlay; }

static int sample_error;
static int charger_error;
static enum defy_battery_charge_state charger_state;

int defy_battery_hci_get(struct defy_battery_sample *sample) {
    memset(sample, 0, sizeof(*sample));
    return sample_error;
}

int defy_battery_sample_charger(const struct defy_battery_sample *sample,
                                struct defy_battery_charger *charger) {
    ARG_UNUSED(sample);
    memset(charger, 0, sizeof(*charger));
    charger->status_valid = true;
    charger->state = charger_state;
    return charger_error;
}

static void record(enum action action) {
    zassert_true(action_count < ARRAY_SIZE(actions));
    actions[action_count++] = action;
}

int defy_hybrid_peripheral_prepare_sleep(int64_t min_idle_ms) {
    record(PREPARE_TRANSPORT);
    idle_argument = min_idle_ms;
    return transport_error;
}

void defy_hybrid_peripheral_cancel_sleep(void) { record(CANCEL_TRANSPORT); }

int defy_half_power_prepare(void) {
    record(PREPARE_MATRIX);
    return matrix_error;
}

bool defy_half_power_pending(void) {
    record(CHECK_PENDING);
    return pm_started ? pending_after_pm : pending_before_pm;
}

int defy_half_power_cancel(void) {
    record(CANCEL_MATRIX);
    return cancel_error;
}

FUNC_NORETURN void defy_half_power_enter(void) {
    record(ENTER_DORMANT);
    longjmp(terminal, ENTER_DORMANT);
}

void sys_reboot(int type) {
    zassert_equal(type, SYS_REBOOT_COLD);
    record(COLD_REBOOT);
    longjmp(terminal, COLD_REBOOT);
}

int zmk_pm_suspend_devices(void) {
    record(SUSPEND_DEVICES);
    pm_started = true;
    for (size_t i = 0; i < ARRAY_SIZE(outputs); i++) {
        struct fake_device *data = outputs[i]->data;
        if (outputs[i] != skip_suspend) {
            data->state = PM_DEVICE_STATE_SUSPENDED;
        }
        if (outputs[i] == state_error_after_pm) {
            data->state_error = -EIO;
        }
    }
    if (suspend_matrix) {
        matrix_data.state = PM_DEVICE_STATE_SUSPENDED;
    }
    if (disable_matrix_wake) {
        matrix_data.wake = false;
    }
    return suspend_error;
}

static int run_poll(void) {
    int result = setjmp(terminal);
    if (result == 0) {
        sleep_poll(&sleep_work.work);
    }
    return result;
}

static void expect_actions(const enum action *expected, size_t count) {
    zassert_equal(action_count, count);
    zassert_mem_equal(actions, expected, count * sizeof(*expected));
}

#define EXPECT_ACTIONS(...) expect_actions((enum action[]){__VA_ARGS__}, sizeof((enum action[]){__VA_ARGS__}) / sizeof(enum action))

static void reset(void *fixture) {
    ARG_UNUSED(fixture);
    struct fake_device active = {.state = PM_DEVICE_STATE_ACTIVE, .ready = true};
    radio_data = active;
    matrix_data = active;
    matrix_data.wake = true;
    led_data = active;
    key_led_data = active;
    action_count = 0;
    transport_error = matrix_error = cancel_error = suspend_error = 0;
    idle_argument = 0;
    schedules = 0;
    scheduled_ms = 0;
    pending_before_pm = pending_after_pm = false;
    pm_started = suspend_matrix = disable_matrix_wake = false;
    battery_overlay = false;
    sample_error = charger_error = 0;
    charger_state = DEFY_BATTERY_CHARGE_READY;
    skip_suspend = state_error_after_pm = NULL;
}

ZTEST(defy_half_sleep, test_battery_overlay_vetoes_prepare_until_expired) {
    battery_overlay = true;
    zassert_equal(run_poll(), 0);
    EXPECT_ACTIONS(CANCEL_TRANSPORT);
    zassert_false(pm_started);
    zassert_equal(idle_argument, 0);
    zassert_equal(schedules, 1);
    zassert_equal(scheduled_ms, 1000);

    battery_overlay = false;
    zassert_equal(run_poll(), ENTER_DORMANT);
    EXPECT_ACTIONS(CANCEL_TRANSPORT, PREPARE_TRANSPORT, PREPARE_MATRIX, CHECK_PENDING, SUSPEND_DEVICES,
                   CHECK_PENDING, ENTER_DORMANT);
    zassert_equal(schedules, 1);
    zassert_equal(idle_argument, CONFIG_DEFY_HALF_SLEEP_TIMEOUT_MS);
}

ZTEST(defy_half_sleep, test_active_charger_vetoes_sleep) {
    const enum defy_battery_charge_state states[] = {DEFY_BATTERY_CHARGE_CHARGING,
                                                     DEFY_BATTERY_CHARGE_DONE};

    for (size_t i = 0; i < ARRAY_SIZE(states); i++) {
        reset(NULL);
        charger_state = states[i];
        zassert_equal(run_poll(), 0);
        EXPECT_ACTIONS(CANCEL_TRANSPORT);
        zassert_false(pm_started);
        zassert_equal(scheduled_ms, 1000);
    }

    charger_state = DEFY_BATTERY_CHARGE_READY;
    zassert_equal(run_poll(), ENTER_DORMANT);
}

ZTEST(defy_half_sleep, test_missing_or_stale_charger_status_allows_sleep) {
    charger_state = DEFY_BATTERY_CHARGE_CHARGING;
    sample_error = -ENODATA;
    zassert_equal(run_poll(), ENTER_DORMANT);

    reset(NULL);
    charger_state = DEFY_BATTERY_CHARGE_CHARGING;
    charger_error = -ESTALE;
    zassert_equal(run_poll(), ENTER_DORMANT);

    reset(NULL);
    charger_state = DEFY_BATTERY_CHARGE_FAULT;
    zassert_equal(run_poll(), ENTER_DORMANT);
}

ZTEST(defy_half_sleep, test_pending_ble_shutdown_keeps_matrix_active_until_completion) {
    transport_error = -EINPROGRESS;
    zassert_equal(run_poll(), 0);
    zassert_equal(run_poll(), 0);
    EXPECT_ACTIONS(PREPARE_TRANSPORT, PREPARE_TRANSPORT);
    zassert_equal(schedules, 2);
    zassert_equal(scheduled_ms, 20);
    zassert_false(pm_started);
    zassert_true(matrix_ready());
    zassert_true(outputs_ready());
    zassert_equal(idle_argument, CONFIG_DEFY_HALF_SLEEP_TIMEOUT_MS);

    transport_error = 0;
    zassert_equal(run_poll(), ENTER_DORMANT);
    EXPECT_ACTIONS(PREPARE_TRANSPORT, PREPARE_TRANSPORT, PREPARE_TRANSPORT, PREPARE_MATRIX,
                   CHECK_PENDING, SUSPEND_DEVICES, CHECK_PENDING, ENTER_DORMANT);
    zassert_equal(schedules, 2);
}

ZTEST(defy_half_sleep, test_held_or_live_cable_or_recent_activity_veto) {
    transport_error = -EBUSY;
    zassert_equal(run_poll(), 0);
    EXPECT_ACTIONS(PREPARE_TRANSPORT);
    zassert_equal(idle_argument, 900000);
    zassert_equal(schedules, 1);
    zassert_equal(scheduled_ms, 1000);
}

ZTEST(defy_half_sleep, test_unarmed_matrix_restores_transport) {
    matrix_error = -EBUSY;
    zassert_equal(run_poll(), 0);
    EXPECT_ACTIONS(PREPARE_TRANSPORT, PREPARE_MATRIX, CANCEL_TRANSPORT);
    zassert_equal(schedules, 1);
}

ZTEST(defy_half_sleep, test_key_before_pm_restores_matrix_then_transport) {
    pending_before_pm = true;
    zassert_equal(run_poll(), 0);
    EXPECT_ACTIONS(PREPARE_TRANSPORT, PREPARE_MATRIX, CHECK_PENDING, CANCEL_MATRIX,
                   CANCEL_TRANSPORT);
    zassert_equal(schedules, 1);
}

ZTEST(defy_half_sleep, test_matrix_rollback_failure_reboots) {
    pending_before_pm = true;
    cancel_error = -EIO;
    zassert_equal(run_poll(), COLD_REBOOT);
    EXPECT_ACTIONS(PREPARE_TRANSPORT, PREPARE_MATRIX, CHECK_PENDING, CANCEL_MATRIX, COLD_REBOOT);
    zassert_equal(schedules, 0);
}

static void assert_preflight_rollback(void) {
    zassert_equal(run_poll(), 0);
    EXPECT_ACTIONS(PREPARE_TRANSPORT, PREPARE_MATRIX, CANCEL_MATRIX, CANCEL_TRANSPORT);
    zassert_false(pm_started);
    zassert_equal(schedules, 1);
}

ZTEST(defy_half_sleep, test_preflight_requires_every_output_ready) {
    for (size_t i = 0; i < ARRAY_SIZE(outputs); i++) {
        reset(NULL);
        ((struct fake_device *)outputs[i]->data)->ready = false;
        assert_preflight_rollback();
    }
}

ZTEST(defy_half_sleep, test_preflight_rejects_busy_runtime_and_wakeup_outputs) {
    for (size_t i = 0; i < ARRAY_SIZE(outputs); i++) {
        for (int flag = 0; flag < 3; flag++) {
            reset(NULL);
            struct fake_device *data = outputs[i]->data;
            data->busy = flag == 0;
            data->runtime = flag == 1;
            data->wake = flag == 2;
            assert_preflight_rollback();
        }
    }
}

ZTEST(defy_half_sleep, test_preflight_rejects_inactive_or_unknown_outputs) {
    for (size_t i = 0; i < ARRAY_SIZE(outputs); i++) {
        reset(NULL);
        ((struct fake_device *)outputs[i]->data)->state = PM_DEVICE_STATE_SUSPENDED;
        assert_preflight_rollback();
        reset(NULL);
        ((struct fake_device *)outputs[i]->data)->state_error = -ENOSYS;
        assert_preflight_rollback();
    }
}

ZTEST(defy_half_sleep, test_preflight_matrix_must_be_active_wake_enabled_and_unmanaged) {
    matrix_data.state = PM_DEVICE_STATE_SUSPENDED;
    assert_preflight_rollback();
    reset(NULL);
    matrix_data.wake = false;
    assert_preflight_rollback();
    reset(NULL);
    matrix_data.runtime = true;
    assert_preflight_rollback();
    reset(NULL);
    matrix_data.ready = false;
    assert_preflight_rollback();
}

ZTEST(defy_half_sleep, test_pm_failure_reboots_without_rollback) {
    suspend_error = -ETIMEDOUT;
    zassert_equal(run_poll(), COLD_REBOOT);
    EXPECT_ACTIONS(PREPARE_TRANSPORT, PREPARE_MATRIX, CHECK_PENDING, SUSPEND_DEVICES, COLD_REBOOT);
    zassert_equal(schedules, 0);
}

ZTEST(defy_half_sleep, test_pm_success_but_skipped_output_reboots) {
    for (size_t i = 0; i < ARRAY_SIZE(outputs); i++) {
        reset(NULL);
        skip_suspend = outputs[i];
        zassert_equal(run_poll(), COLD_REBOOT);
        EXPECT_ACTIONS(PREPARE_TRANSPORT, PREPARE_MATRIX, CHECK_PENDING, SUSPEND_DEVICES,
                       COLD_REBOOT);
        zassert_equal(schedules, 0);
    }
}

ZTEST(defy_half_sleep, test_pm_state_read_failure_reboots) {
    state_error_after_pm = outputs[0];
    zassert_equal(run_poll(), COLD_REBOOT);
    EXPECT_ACTIONS(PREPARE_TRANSPORT, PREPARE_MATRIX, CHECK_PENDING, SUSPEND_DEVICES, COLD_REBOOT);
}

ZTEST(defy_half_sleep, test_matrix_suspended_or_wake_disabled_after_pm_reboots) {
    suspend_matrix = true;
    zassert_equal(run_poll(), COLD_REBOOT);
    EXPECT_ACTIONS(PREPARE_TRANSPORT, PREPARE_MATRIX, CHECK_PENDING, SUSPEND_DEVICES, COLD_REBOOT);
    reset(NULL);
    disable_matrix_wake = true;
    zassert_equal(run_poll(), COLD_REBOOT);
    EXPECT_ACTIONS(PREPARE_TRANSPORT, PREPARE_MATRIX, CHECK_PENDING, SUSPEND_DEVICES, COLD_REBOOT);
}

ZTEST(defy_half_sleep, test_key_during_pm_reboots_without_restarting_radio) {
    pending_after_pm = true;
    zassert_equal(run_poll(), COLD_REBOOT);
    EXPECT_ACTIONS(PREPARE_TRANSPORT, PREPARE_MATRIX, CHECK_PENDING, SUSPEND_DEVICES,
                   CHECK_PENDING, COLD_REBOOT);
    zassert_equal(schedules, 0);
}

ZTEST(defy_half_sleep, test_only_verified_suspended_outputs_enter_dormant) {
    zassert_equal(run_poll(), ENTER_DORMANT);
    EXPECT_ACTIONS(PREPARE_TRANSPORT, PREPARE_MATRIX, CHECK_PENDING, SUSPEND_DEVICES,
                   CHECK_PENDING, ENTER_DORMANT);
    zassert_equal(schedules, 0);
}

ZTEST_SUITE(defy_half_sleep, NULL, NULL, reset, NULL, NULL);
