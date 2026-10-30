/*
 * The registers boot.c uses, by address: the SPI slave SPIS0, GPIO port 0,
 * and the core's SysTick and ICSR. Plain numbers rather than Nordic's and
 * CMSIS's structures, so that the host test can fake every access through
 * boot_io.h.
 *
 * SPDX-License-Identifier: MIT
 */

#ifndef DEFY_RADIO_BOOT_REGISTERS_H
#define DEFY_RADIO_BOOT_REGISTERS_H

#define DEFY_BOOT_SPIS_ACQUIRE 0x40003024u
#define DEFY_BOOT_SPIS_INTENCLR 0x40003308u
#define DEFY_BOOT_SPIS_SEMSTAT 0x40003400u
#define DEFY_BOOT_SPIS_ENABLE 0x40003500u
#define DEFY_BOOT_SPIS_CSN 0x40003514u
#define DEFY_BOOT_GPIO_IN 0x50000510u
#define DEFY_BOOT_SYSTICK_CTRL 0xe000e010u
#define DEFY_BOOT_SYSTICK_LOAD 0xe000e014u
#define DEFY_BOOT_SYSTICK_VALUE 0xe000e018u
#define DEFY_BOOT_ICSR 0xe000ed04u

#define DEFY_BOOT_SPIS_ENABLED 2u
#define DEFY_BOOT_SPIS_SEMSTAT_MASK 3u
#define DEFY_BOOT_SPIS_CPU_OWNER 1u
#define DEFY_BOOT_SYSTICK_ENABLE 1u
#define DEFY_BOOT_SYSTICK_CORE_CLOCK 4u
#define DEFY_BOOT_SYSTICK_MASK 0x00ffffffu
#define DEFY_BOOT_CLEAR_PENDING_SYSTICK (1u << 25)
#define DEFY_BOOT_CORE_CYCLES_PER_US 64u

#endif
