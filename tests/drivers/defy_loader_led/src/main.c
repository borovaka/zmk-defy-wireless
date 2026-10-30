/* SPDX-License-Identifier: MIT */

#include <string.h>

#include <zephyr/ztest.h>
#include <zephyr/kernel.h>
#include <hal/nrf_pwm.h>

/* A clock the test sets in now. */
static uint32_t now;
static uint32_t test_uptime_32(void) { return now; }
#define k_uptime_get_32 test_uptime_32
#include "../../../../apps/defy_loader/src/led.c"
#undef k_uptime_get_32

/* 3% of the 1600 counts in 100 us, and nothing, both as active-high pulses. */
#define LIT (48u | BIT(15))
#define DARK BIT(15)

NRF_PWM_Type fake_pwm;
static bool startable;

void fake_pwm_check(NRF_PWM_Type const *p_reg) {
    zassert_equal_ptr(p_reg, (NRF_PWM_Type *)DT_REG_ADDR(DT_NODELABEL(test_pwm)));
}

bool defy_loader_zmk_startable(void) { return startable; }

/* Runs the LED's work now, a pending blink included. */
static void flush_led(void) {
    struct k_work_sync sync;
    k_work_flush_delayable(&led_work, &sync);
}

static void start_led(void) {
    defy_loader_led_init();
    flush_led();
}

static void block(void) {
    defy_loader_led_block();
    flush_led();
}

static void before(void *fixture) {
    ARG_UNUSED(fixture);
    struct k_work_sync sync;
    k_work_cancel_delayable_sync(&led_work, &sync);
    atomic_clear(&led_flags);
    atomic_clear(&last_block);
    memset(&duty, 0, sizeof(duty));
    memset(&fake_pwm, 0, sizeof(fake_pwm));
    now = 1000;
    startable = true;
}

static void after(void *fixture) {
    ARG_UNUSED(fixture);
    struct k_work_sync sync;
    k_work_cancel_delayable_sync(&led_work, &sync);
}

ZTEST(loader_led, test_startable_zmk_is_amber) {
    start_led();
    zassert_equal(fake_pwm.played.channel_0, LIT);
    zassert_equal(fake_pwm.played.channel_1, LIT);
}

ZTEST(loader_led, test_zmk_that_cannot_start_is_red) {
    startable = false;
    start_led();
    zassert_equal(fake_pwm.played.channel_0, DARK);
    zassert_equal(fake_pwm.played.channel_1, LIT);
}

ZTEST(loader_led, test_blocks_blink_green) {
    start_led();
    now = 1030;
    block();
    zassert_equal(fake_pwm.played.channel_0, LIT);
    zassert_equal(fake_pwm.played.channel_1, DARK);
    now = 1100;
    flush_led();
    zassert_equal(fake_pwm.played.channel_0, DARK);
    now = 1201;
    flush_led();
    zassert_equal(fake_pwm.played.channel_0, LIT);
    zassert_equal(fake_pwm.played.channel_1, DARK);
}

ZTEST(loader_led, test_blink_stops_a_second_after_the_last_block) {
    start_led();
    block();
    now = 1000 + COPY_IDLE_MS - 1;
    flush_led();
    zassert_equal(fake_pwm.played.channel_1, DARK);
    now = 1000 + COPY_IDLE_MS;
    flush_led();
    zassert_false(atomic_test_bit(&led_flags, LED_COPYING));
    zassert_equal(fake_pwm.played.channel_0, LIT);
    zassert_equal(fake_pwm.played.channel_1, LIT);
    zassert_false(k_work_delayable_is_pending(&led_work));
}

ZTEST(loader_led, test_each_block_extends_the_blink) {
    start_led();
    block();
    now = 1800;
    block();
    now = 2700;
    flush_led();
    zassert_equal(fake_pwm.played.channel_1, DARK);
    now = 2800;
    flush_led();
    zassert_equal(fake_pwm.played.channel_1, LIT);
}

ZTEST(loader_led, test_interrupted_copy_turns_red) {
    start_led();
    block();
    /* The copy has erased ZMK's first page. */
    startable = false;
    now += COPY_IDLE_MS;
    flush_led();
    zassert_equal(fake_pwm.played.channel_0, DARK);
    zassert_equal(fake_pwm.played.channel_1, LIT);
    block();
    zassert_equal(fake_pwm.played.channel_0, LIT);
    zassert_equal(fake_pwm.played.channel_1, DARK);
}

ZTEST(loader_led, test_off_stays_off) {
    start_led();
    block();
    defy_loader_led_off();
    zassert_false(fake_pwm.enabled);
    unsigned int starts = fake_pwm.starts;
    now += BLINK_MS;
    flush_led();
    now += COPY_IDLE_MS;
    block();
    zassert_false(fake_pwm.enabled);
    zassert_equal(fake_pwm.starts, starts);
}

ZTEST_SUITE(loader_led, NULL, NULL, before, after, NULL);
