/* SPDX-License-Identifier: MIT */

#include <errno.h>
#include <string.h>
#include <zephyr/irq.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/sys_io.h>
#include <zephyr/ztest.h>

static uint32_t fake_cycles;
static uint32_t fake_cycle_get_32(void);
static void fake_busy_wait(uint32_t microseconds);
static uint32_t fake_read32(mem_addr_t address);
static void fake_write32(uint32_t value, mem_addr_t address);

#undef k_cycle_get_32
#define k_cycle_get_32 fake_cycle_get_32
#undef k_cyc_to_us_floor64
#define k_cyc_to_us_floor64(cycles) ((uint64_t)(cycles))
#define k_busy_wait fake_busy_wait
#define sys_read32 fake_read32
#define sys_write32 fake_write32

#include "../../../../apps/defy_radio/src/battery_comp.c"

NRF_COMP_Type fake_comp;
NRF_FICR_Type fake_ficr;
static bool enabled;
static bool running;
static bool ready;
static bool configured;
static bool reset_expected;
static uint32_t interrupts;
static uint32_t shorts;
static uint32_t cleared_events;
static uint32_t trial_started;
static uint32_t ready_delay;
static unsigned int start_count;
static unsigned int sample_count;
static unsigned int stop_count;
static unsigned int disable_count;
static unsigned int wait_count;
static unsigned int reset_count;
static unsigned int shutdown_phase;
static int timeout_trial;
static int crossing_2v4;
static int crossing_vdd;
static nrfy_comp_config_t current;
static nrfy_comp_config_t trials[128];

static void check_context(const NRF_COMP_Type *reg) {
    zassert_equal(reg, NRF_COMP);
    unsigned int key = irq_lock();
    zassert_true(arch_irq_unlocked(key));
    irq_unlock(key);
}

static uint32_t fake_cycle_get_32(void) {
    return fake_cycles;
}

static void fake_busy_wait(uint32_t microseconds) {
    check_context(NRF_COMP);
    zassert_true(running);
    zassert_equal(microseconds, 1);
    fake_cycles += microseconds;
    wait_count++;
}

static uint32_t fake_read32(mem_addr_t address) {
    check_context(NRF_COMP);
    zassert_equal(address, 0x40013ffcu);
    zassert_equal(shutdown_phase, 3);
    shutdown_phase = 4;
    return 0;
}

static void fake_write32(uint32_t value, mem_addr_t address) {
    check_context(NRF_COMP);
    zassert_equal(address, 0x40013ffcu);
    zassert_true(reset_expected);
    zassert_false(enabled);
    zassert_false(running);
    if (value == 0) {
        zassert_equal(shutdown_phase, 2);
        shutdown_phase = 3;
        configured = false;
        ready = false;
        interrupts = 0;
        shorts = 0;
        cleared_events = 0;
        memset(&current, 0, sizeof(current));
        return;
    }
    zassert_equal(value, 1);
    zassert_equal(shutdown_phase, 4);
    shutdown_phase = 5;
    reset_count++;
}

void nrfy_comp_int_disable(NRF_COMP_Type *reg, uint32_t mask) {
    check_context(reg);
    interrupts &= ~mask;
}

void nrfy_comp_shorts_disable(NRF_COMP_Type *reg, uint32_t mask) {
    check_context(reg);
    shorts &= ~mask;
}

void nrfy_comp_task_trigger(NRF_COMP_Type *reg, nrf_comp_task_t task) {
    check_context(reg);
    if (task == NRF_COMP_TASK_STOP) {
        running = false;
        stop_count++;
        shutdown_phase = 1;
        return;
    }
    zassert_equal(task, NRF_COMP_TASK_START);
    zassert_true(enabled);
    zassert_false(running);
    zassert_false(ready);
    zassert_true(configured);
    zassert_equal(interrupts, 0);
    zassert_equal(shorts, 0);
    zassert_equal(cleared_events, 0xf);
    zassert_true(start_count < ARRAY_SIZE(trials));
    trials[start_count++] = current;
    running = true;
    trial_started = fake_cycles;
    cleared_events = 0;
}

void nrfy_comp_disable(NRF_COMP_Type *reg) {
    check_context(reg);
    zassert_false(running);
    zassert_equal(shutdown_phase, 1);
    shutdown_phase = 2;
    enabled = false;
    disable_count++;
}

void nrfy_comp_enable(NRF_COMP_Type *reg) {
    check_context(reg);
    zassert_false(enabled);
    zassert_true(configured);
    enabled = true;
}

void nrfy_comp_periph_configure(NRF_COMP_Type *reg, const nrfy_comp_config_t *config) {
    check_context(reg);
    zassert_false(enabled);
    zassert_false(running);
    zassert_equal(shutdown_phase, reset_expected ? 5 : 2);
    zassert_equal(config->input, NRF_COMP_INPUT_1);
    zassert_equal(config->main_mode, NRF_COMP_MAIN_MODE_SE);
    zassert_equal(config->speed_mode, NRF_COMP_SP_MODE_NORMAL);
    zassert_equal(config->hyst, NRF_COMP_HYST_NO_HYST);
    zassert_equal(config->threshold.th_up, config->threshold.th_down);
    zassert_true(config->threshold.th_up <= 63);
    zassert_true(config->reference == NRF_COMP_REF_INT_2V4 ||
                 config->reference == NRF_COMP_REF_VDD);
    current = *config;
    configured = true;
}

void nrfy_comp_event_clear(NRF_COMP_Type *reg, nrf_comp_event_t event) {
    check_context(reg);
    zassert_false(running);
    cleared_events |= BIT(event);
    if (event == NRF_COMP_EVENT_READY) {
        ready = false;
    }
}

bool nrfy_comp_event_check(const NRF_COMP_Type *reg, nrf_comp_event_t event) {
    check_context(reg);
    zassert_true(enabled);
    zassert_true(running);
    zassert_equal(event, NRF_COMP_EVENT_READY);
    if ((int)start_count - 1 != timeout_trial &&
        (uint32_t)(fake_cycles - trial_started) >= ready_delay) {
        ready = true;
    }
    return ready;
}

uint32_t nrfy_comp_sample(NRF_COMP_Type *reg) {
    check_context(reg);
    zassert_true(ready);
    zassert_true(enabled);
    zassert_true(running);
    sample_count++;
    int crossing = current.reference == NRF_COMP_REF_INT_2V4 ? crossing_2v4 : crossing_vdd;
    return current.threshold.th_up <= crossing;
}

static void before(void *fixture) {
    ARG_UNUSED(fixture);
    fake_cycles = 0;
    enabled = true;
    running = true;
    ready = true;
    configured = true;
    reset_expected = true;
    fake_ficr.INFO.PART = FICR_INFO_PART_PART_N52820;
    fake_ficr.INFO.VARIANT = FICR_INFO_VARIANT_VARIANT_AAD0;
    interrupts = UINT32_MAX;
    shorts = UINT32_MAX;
    cleared_events = 0;
    trial_started = 0;
    ready_delay = 0;
    start_count = 0;
    sample_count = 0;
    stop_count = 0;
    disable_count = 0;
    wait_count = 0;
    reset_count = 0;
    shutdown_phase = 0;
    timeout_trial = -1;
    crossing_2v4 = 47;
    crossing_vdd = 38;
    memset(trials, 0, sizeof(trials));
}

static void assert_stopped(unsigned int completed) {
    zassert_false(enabled);
    zassert_false(running);
    zassert_equal(interrupts, 0);
    zassert_equal(shorts, 0);
    zassert_equal(start_count, completed);
    zassert_equal(stop_count, 1 + completed * 2);
    zassert_equal(disable_count, 1 + completed);
    zassert_equal(reset_count, reset_expected ? 1 + completed : 0);
    zassert_equal(shutdown_phase, reset_expected ? 5 : 2);
    if (reset_expected) {
        zassert_false(configured);
    }
}

static void assert_sweep(unsigned int start, uint8_t last, nrf_comp_ref_t reference) {
    for (int threshold = 63; threshold >= last; threshold--) {
        const nrfy_comp_config_t *trial = &trials[start++];
        zassert_equal(trial->reference, reference);
        zassert_equal(trial->threshold.th_up, threshold);
    }
}

ZTEST(defy_radio_battery_comp, test_descends_to_first_high_result) {
    struct defy_battery_comp_reading reading = {0};
    zassert_ok(defy_radio_battery_comp_read(&reading));
    zassert_equal(reading.reference, DEFY_BATTERY_ANALOG_REFERENCE_2V4);
    zassert_equal(reading.threshold, 47);
    assert_sweep(0, 47, NRF_COMP_REF_INT_2V4);
    assert_stopped(17);
    zassert_equal(sample_count, 17);
}

ZTEST(defy_radio_battery_comp, test_saturated_2v4_repeats_with_vdd) {
    crossing_2v4 = 63;
    struct defy_battery_comp_reading reading = {0};
    zassert_ok(defy_radio_battery_comp_read(&reading));
    zassert_equal(reading.reference, DEFY_BATTERY_ANALOG_REFERENCE_VDD);
    zassert_equal(reading.threshold, 38);
    assert_sweep(0, 63, NRF_COMP_REF_INT_2V4);
    assert_sweep(1, 38, NRF_COMP_REF_VDD);
    assert_stopped(27);
}

ZTEST(defy_radio_battery_comp, test_threshold_62_does_not_fall_back) {
    crossing_2v4 = 62;
    struct defy_battery_comp_reading reading = {0};
    zassert_ok(defy_radio_battery_comp_read(&reading));
    zassert_equal(reading.reference, DEFY_BATTERY_ANALOG_REFERENCE_2V4);
    zassert_equal(reading.threshold, 62);
    assert_stopped(2);
}

ZTEST(defy_radio_battery_comp, test_low_endpoint_stops_even_when_result_remains_low) {
    crossing_2v4 = -1;
    struct defy_battery_comp_reading reading = {0xff, 0xff};
    zassert_ok(defy_radio_battery_comp_read(&reading));
    zassert_equal(reading.threshold, 0);
    zassert_equal(reading.reference, DEFY_BATTERY_ANALOG_REFERENCE_2V4);
    assert_sweep(0, 0, NRF_COMP_REF_INT_2V4);
    assert_stopped(64);
}

ZTEST(defy_radio_battery_comp, test_delayed_ready_is_awaited_with_irqs_enabled) {
    ready_delay = 12;
    struct defy_battery_comp_reading reading = {0};
    zassert_ok(defy_radio_battery_comp_read(&reading));
    zassert_equal(reading.threshold, 47);
    zassert_equal(wait_count, 17 * 12);
    zassert_equal(fake_cycles, 17 * 12);
    assert_stopped(17);
}

ZTEST(defy_radio_battery_comp, test_ready_timeout_preserves_output_and_stops) {
    timeout_trial = 0;
    struct defy_battery_comp_reading reading = {0xa5, 0x5a};
    zassert_equal(defy_radio_battery_comp_read(&reading), -ETIMEDOUT);
    zassert_equal(reading.threshold, 0xa5);
    zassert_equal(reading.reference, 0x5a);
    zassert_equal(wait_count, 1000);
    zassert_equal(sample_count, 0);
    assert_stopped(1);
}

ZTEST(defy_radio_battery_comp, test_cycle_counter_wrap_keeps_timeout_bounded) {
    fake_cycles = UINT32_MAX - 10;
    timeout_trial = 0;
    struct defy_battery_comp_reading reading = {0};
    zassert_equal(defy_radio_battery_comp_read(&reading), -ETIMEDOUT);
    zassert_equal(wait_count, 1000);
    assert_stopped(1);
}

/* Erratum 275 affects the nRF52820's AAC and AAD builds, whatever the variant's last character. */
ZTEST(defy_radio_battery_comp, test_reset_register_follows_identity) {
    static const struct {
        uint32_t part;
        uint32_t variant;
        bool reset;
    } identities[] = {
        {0x52820, 0x41414330, true},
        {0x52820, 0x41414431, true},
        {0x52820, 0x41414243, false},
        {0x52820, 0x41414530, false},
        {0x52820, UINT32_MAX, false},
        {0x52833, 0x41414430, false},
        {UINT32_MAX, UINT32_MAX, false},
    };
    for (size_t i = 0; i < ARRAY_SIZE(identities); i++) {
        before(NULL);
        fake_ficr.INFO.PART = identities[i].part;
        fake_ficr.INFO.VARIANT = identities[i].variant;
        reset_expected = identities[i].reset;
        struct defy_battery_comp_reading reading = {0};
        zassert_ok(defy_radio_battery_comp_read(&reading));
        zassert_equal(reading.threshold, 47);
        assert_stopped(17);
    }
}

ZTEST_SUITE(defy_radio_battery_comp, NULL, NULL, before, NULL, NULL);
