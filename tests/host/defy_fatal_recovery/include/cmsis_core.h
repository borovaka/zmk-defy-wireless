/* SPDX-License-Identifier: MIT */

#pragma once
#include <stdint.h>
struct fake_systick { uint32_t CTRL; };
struct fake_nvic { uint32_t ICER[1], ICPR[1]; };
struct fake_scb { uint32_t ICSR; };
extern struct fake_systick fake_systick;
extern struct fake_nvic fake_nvic;
extern struct fake_scb fake_scb;
#define SysTick (&fake_systick)
#define NVIC (&fake_nvic)
#define SCB (&fake_scb)
#define SCB_ICSR_PENDSTCLR_Msk (1u << 25)
#define SCB_ICSR_PENDSVCLR_Msk (1u << 27)
void test_wfi(void);
void test_dsb(void);
#define __WFI() test_wfi()
#define __DSB() test_dsb()
