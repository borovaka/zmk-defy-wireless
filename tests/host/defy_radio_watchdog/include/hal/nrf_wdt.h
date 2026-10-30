/* SPDX-License-Identifier: MIT */

#pragma once

#include <stdbool.h>
#include <stdint.h>

#define NRF_WDT_CHANNEL_NUMBER 8
#define NRF_WDT_RR_VALUE UINT32_C(0x6e524635)

typedef struct {
    uint32_t TASKS_START;
    uint32_t RUNSTATUS;
    uint32_t REQSTATUS;
    uint32_t CRV;
    uint32_t RREN;
    uint32_t CONFIG;
    uint32_t RR[NRF_WDT_CHANNEL_NUMBER];
} NRF_WDT_Type;

typedef enum {
    NRF_WDT_RR0,
    NRF_WDT_RR1,
    NRF_WDT_RR2,
    NRF_WDT_RR3,
    NRF_WDT_RR4,
    NRF_WDT_RR5,
    NRF_WDT_RR6,
    NRF_WDT_RR7,
} nrf_wdt_rr_register_t;

extern NRF_WDT_Type watchdog_registers;
extern unsigned int reload_count[NRF_WDT_CHANNEL_NUMBER];

#define NRF_WDT (&watchdog_registers)

static inline bool nrf_wdt_started_check(const NRF_WDT_Type *reg) {
    return (reg->RUNSTATUS & 1U) != 0;
}

static inline bool nrf_wdt_reload_request_enable_check(const NRF_WDT_Type *reg,
                                                      nrf_wdt_rr_register_t rr) {
    return (reg->RREN & (1U << rr)) != 0;
}

static inline void nrf_wdt_reload_request_set(NRF_WDT_Type *reg, nrf_wdt_rr_register_t rr) {
    reg->RR[rr] = NRF_WDT_RR_VALUE;
    reload_count[rr]++;
}
