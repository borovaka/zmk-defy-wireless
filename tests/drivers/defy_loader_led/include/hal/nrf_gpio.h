/* SPDX-License-Identifier: MIT */

#ifndef DEFY_TEST_NRF_GPIO_H
#define DEFY_TEST_NRF_GPIO_H

#include <stdint.h>

/* The loader sets the LED pins low as outputs before the PWM takes them. */
static inline void nrf_gpio_pin_clear(uint32_t pin_number) {}

static inline void nrf_gpio_cfg_output(uint32_t pin_number) {}

#endif
