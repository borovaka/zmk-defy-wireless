/* SPDX-License-Identifier: MIT */

#pragma once

void __disable_irq(void);
void __DSB(void);
void __ISB(void);
void SystemInit(void);
__attribute__((noreturn)) void NVIC_SystemReset(void);
