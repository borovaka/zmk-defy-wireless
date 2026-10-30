/* SPDX-License-Identifier: MIT */

#ifndef DEFY_TEST_NRF_PWM_H
#define DEFY_TEST_NRF_PWM_H

#include <stdbool.h>
#include <stdint.h>

#define NRF_PWM_PIN_NOT_CONNECTED 0xffffffffu
#define NRF_PWM_CHANNEL_COUNT 4
#define NRF_PWM_VALUES_LENGTH(array) (sizeof(array) / 2u)

typedef enum { NRF_PWM_TASK_SEQSTART0 } nrf_pwm_task_t;
typedef enum { NRF_PWM_MODE_UP } nrf_pwm_mode_t;
typedef enum { NRF_PWM_CLK_16MHz } nrf_pwm_clk_t;
typedef enum { NRF_PWM_LOAD_INDIVIDUAL } nrf_pwm_dec_load_t;
typedef enum { NRF_PWM_STEP_AUTO } nrf_pwm_dec_step_t;

typedef struct {
    uint16_t channel_0;
    uint16_t channel_1;
    uint16_t channel_2;
    uint16_t channel_3;
} nrf_pwm_values_individual_t;

typedef union {
    uint16_t const *p_raw;
    nrf_pwm_values_individual_t const *p_individual;
} nrf_pwm_values_t;

typedef struct {
    nrf_pwm_values_t values;
    uint16_t length;
} nrf_pwm_sequence_t;

/* Whether the PWM is on, its sequence, and what a playback would output: the sequence's
 * values when it last started. */
typedef struct {
    bool enabled;
    nrf_pwm_sequence_t const *sequence;
    unsigned int starts;
    nrf_pwm_values_individual_t played;
} NRF_PWM_Type;

/* The test's PWM sits at address 0 in its devicetree, so the fakes keep one register
 * set and only check that the loader passes that address. */
extern NRF_PWM_Type fake_pwm;
void fake_pwm_check(NRF_PWM_Type const *p_reg);

/* The pins, clock, period and decoder come from the devicetree, checked at build time. */
static inline void nrf_pwm_pins_set(NRF_PWM_Type *p_reg,
                                    uint32_t const out_pins[NRF_PWM_CHANNEL_COUNT]) {
    fake_pwm_check(p_reg);
}

static inline void nrf_pwm_configure(NRF_PWM_Type *p_reg, nrf_pwm_clk_t base_clock,
                                     nrf_pwm_mode_t mode, uint16_t top_value) {
    fake_pwm_check(p_reg);
}

static inline void nrf_pwm_decoder_set(NRF_PWM_Type *p_reg, nrf_pwm_dec_load_t dec_load,
                                       nrf_pwm_dec_step_t dec_step) {
    fake_pwm_check(p_reg);
}

static inline void nrf_pwm_sequence_set(NRF_PWM_Type *p_reg, uint8_t seq_id,
                                        nrf_pwm_sequence_t const *p_seq) {
    fake_pwm_check(p_reg);
    if (seq_id == 0) {
        fake_pwm.sequence = p_seq;
    }
}

static inline void nrf_pwm_enable(NRF_PWM_Type *p_reg) {
    fake_pwm_check(p_reg);
    fake_pwm.enabled = true;
}

static inline void nrf_pwm_disable(NRF_PWM_Type *p_reg) {
    fake_pwm_check(p_reg);
    fake_pwm.enabled = false;
}

static inline void nrf_pwm_task_trigger(NRF_PWM_Type *p_reg, nrf_pwm_task_t task) {
    fake_pwm_check(p_reg);
    if (task == NRF_PWM_TASK_SEQSTART0 && fake_pwm.enabled && fake_pwm.sequence) {
        ++fake_pwm.starts;
        fake_pwm.played = *fake_pwm.sequence->values.p_individual;
    }
}

#endif
