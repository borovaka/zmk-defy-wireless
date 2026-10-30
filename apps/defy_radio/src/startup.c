/*
 * soc_reset_hook(), the radio's first code after a reset, ahead of Zephyr's
 * own start.
 *
 * rf_bootloader starts this firmware with a jump, through S113, and leaves
 * its SPI slave enabled, with EasyDMA pointed at the bootloader's buffers in
 * RAM. A transfer the RP2040 clocked while Zephyr clears and fills that RAM
 * would land in it. So with interrupts off, the hook waits for chip select
 * to go idle, takes the SPIS semaphore so that no transfer can start,
 * and disables the SPIS (boot.c). If that fails, or takes more than 20 ms,
 * it resets the radio. It also feeds a hardware watchdog left running by
 * Dygma's radio application (watchdog.c).
 *
 * Nordic's SoC support sets soc_reset_hook to SystemInit with PROVIDE() in
 * its linker script, which applies only while nothing else defines the
 * symbol. This definition replaces it, and calls SystemInit() itself once
 * the SPIS is quiet.
 *
 * SPDX-License-Identifier: MIT
 */

#include <nrf.h>

#include "boot.h"
#include "watchdog.h"

void soc_reset_hook(void) {
    __disable_irq();
    defy_radio_watchdog_feed();
    if (!defy_radio_boot_quiesce_spis(defy_radio_watchdog_feed)) {
        NVIC_SystemReset();
    }
    __DSB();
    __ISB();
    SystemInit();
}
