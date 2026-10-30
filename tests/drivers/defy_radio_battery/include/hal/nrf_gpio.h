/* SPDX-License-Identifier: MIT */

#ifndef DEFY_TEST_NRF_GPIO_H
#define DEFY_TEST_NRF_GPIO_H

#include <stdint.h>

typedef struct { uint32_t IN; uint32_t PIN_CNF[32]; } NRF_GPIO_Type;
extern NRF_GPIO_Type fake_gpio0;
#define NRF_P0 (&fake_gpio0)

#endif
