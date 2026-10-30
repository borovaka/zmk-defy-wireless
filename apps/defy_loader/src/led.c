/*
 * The Neuron's status LED in drive mode: amber while ZMK can start, as on a
 * drive shown on request, red while it cannot, and green blinking fast while
 * UF2 blocks for ZMK are written. The brightness is ZMK's default; the loader
 * is built without the keymap's settings.
 *
 * Zephyr's PWM driver would take 3 KB of the loader's 48 KiB, so this file
 * sets up the PWM peripheral through nrfx's register functions. The pins,
 * channels and period come from the led_green and led_red nodes in the plain
 * board's devicetree, which ZMK drives through Zephyr's driver.
 *
 * SPDX-License-Identifier: MIT
 */

#include <zephyr/devicetree.h>
#include <zephyr/dt-bindings/pinctrl/nrf-pinctrl.h>
#include <zephyr/dt-bindings/pwm/pwm.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/atomic.h>
#include <hal/nrf_gpio.h>
#include <hal/nrf_pwm.h>

#include "loader.h"

/* CONFIG_DEFY_NEURON_STATUS_LED_BRIGHTNESS's default in ZMK, in percent. */
#define BRIGHTNESS 3
/* Green's half period while blocks are written. */
#define BLINK_MS 100
/* The blink stops this long after the last block, give or take a blink. */
#define COPY_IDLE_MS 1000

#define GREEN DT_NODELABEL(led_green)
#define RED DT_NODELABEL(led_red)
#define PWM_NODE DT_PWMS_CTLR(GREEN)
/* The pins of the PWM's default state, in channel order. */
#define PINS DT_CHILD(DT_PINCTRL_BY_IDX(PWM_NODE, 0, 0), group1)
#define PSEL(channel) DT_PROP_BY_IDX(PINS, psels, channel)
#define PSEL_PIN(channel) ((PSEL(channel) >> NRF_PIN_POS) & NRF_PIN_MSK)
#define PSEL_FUN(channel) ((PSEL(channel) >> NRF_FUN_POS) & NRF_FUN_MSK)
/* The period in counts of the 16 MHz clock, which COUNTERTOP holds in 15 bits. */
#define TOP (DT_PWMS_PERIOD(GREEN) * 16ull / 1000)
/* An active-high pulse of value counts, as in Zephyr's driver. */
#define DUTY(value) ((uint16_t)((value) | BIT(15)))

BUILD_ASSERT(DT_SAME_NODE(DT_PWMS_CTLR(RED), PWM_NODE));
BUILD_ASSERT(DT_PWMS_CHANNEL(GREEN) == 0 && DT_PWMS_CHANNEL(RED) == 1);
BUILD_ASSERT(DT_PWMS_PERIOD(RED) == DT_PWMS_PERIOD(GREEN));
BUILD_ASSERT(DT_PWMS_FLAGS(GREEN) == PWM_POLARITY_NORMAL &&
             DT_PWMS_FLAGS(RED) == PWM_POLARITY_NORMAL);
BUILD_ASSERT(DT_PROP_LEN(PINS, psels) == 2 && PSEL_FUN(0) == NRF_FUN_PWM_OUT0 &&
             PSEL_FUN(1) == NRF_FUN_PWM_OUT1);
BUILD_ASSERT(TOP > 0 && TOP <= 0x7fff);

static NRF_PWM_Type *const pwm = (NRF_PWM_Type *)DT_REG_ADDR(PWM_NODE);
/* The PWM reads the sequence from RAM at each start. */
static nrf_pwm_values_individual_t duty;
static const nrf_pwm_sequence_t sequence = {
    .values.p_individual = &duty,
    .length = NRF_PWM_VALUES_LENGTH(duty),
};

enum {
    LED_READY,
    LED_COPYING,
    LED_OFF,
};

static atomic_t led_flags;
/* k_uptime_get_32() at the last block. */
static atomic_t last_block;

/* One playback of the sequence: the PWM then holds its values. */
static void show(bool red, bool green) {
    duty.channel_0 = DUTY(green ? TOP * BRIGHTNESS / 100 : 0);
    duty.channel_1 = DUTY(red ? TOP * BRIGHTNESS / 100 : 0);
    nrf_pwm_task_trigger(pwm, NRF_PWM_TASK_SEQSTART0);
}

static void update_led(struct k_work *work);
static K_WORK_DELAYABLE_DEFINE(led_work, update_led);

static void update_led(struct k_work *work) {
    ARG_UNUSED(work);
    if (!atomic_test_bit(&led_flags, LED_READY) || atomic_test_bit(&led_flags, LED_OFF)) {
        return;
    }
    uint32_t now = k_uptime_get_32();
    if (atomic_test_bit(&led_flags, LED_COPYING) &&
        now - (uint32_t)atomic_get(&last_block) < COPY_IDLE_MS) {
        show(false, (now / BLINK_MS) % 2 == 0);
        k_work_schedule(&led_work, K_MSEC(BLINK_MS - now % BLINK_MS));
        return;
    }
    /* A block that arrives now finds the flag clear and runs this again. */
    atomic_clear_bit(&led_flags, LED_COPYING);
    show(true, defy_loader_zmk_startable());
}

void defy_loader_led_init(void) {
    uint32_t pins[NRF_PWM_CHANNEL_COUNT] = {PSEL_PIN(0), PSEL_PIN(1), NRF_PWM_PIN_NOT_CONNECTED,
                                            NRF_PWM_PIN_NOT_CONNECTED};
    /* Low while the PWM is off, as Zephyr's pin control leaves them. */
    for (int i = 0; i < 2; i++) {
        nrf_gpio_pin_clear(pins[i]);
        nrf_gpio_cfg_output(pins[i]);
    }
    nrf_pwm_pins_set(pwm, pins);
    nrf_pwm_configure(pwm, NRF_PWM_CLK_16MHz, NRF_PWM_MODE_UP, TOP);
    nrf_pwm_decoder_set(pwm, NRF_PWM_LOAD_INDIVIDUAL, NRF_PWM_STEP_AUTO);
    nrf_pwm_sequence_set(pwm, 0, &sequence);
    nrf_pwm_enable(pwm);
    atomic_set_bit(&led_flags, LED_READY);
    k_work_schedule(&led_work, K_NO_WAIT);
}

void defy_loader_led_block(void) {
    atomic_set(&last_block, (atomic_val_t)k_uptime_get_32());
    if (!atomic_test_and_set_bit(&led_flags, LED_COPYING)) {
        k_work_schedule(&led_work, K_NO_WAIT);
    }
}

void defy_loader_led_off(void) {
    atomic_set_bit(&led_flags, LED_OFF);
    /* The pins go back to their GPIO setting, low. */
    nrf_pwm_disable(pwm);
}
