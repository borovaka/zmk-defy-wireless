/* SPDX-License-Identifier: MIT */

#ifndef DEFY_TEST_NRF_POWER_H
#define DEFY_TEST_NRF_POWER_H

#include <stdint.h>

typedef struct { uint32_t GPREGRET[2]; } NRF_POWER_Type;
extern NRF_POWER_Type fake_power;
#define NRF_POWER (&fake_power)

static inline void nrf_power_gpregret_set(NRF_POWER_Type *p_reg, uint32_t reg_num, uint32_t val) {
    p_reg->GPREGRET[reg_num] = val & 0xff;
}

static inline uint32_t nrf_power_gpregret_get(NRF_POWER_Type const *p_reg, uint32_t reg_num) {
    return p_reg->GPREGRET[reg_num];
}

#endif
