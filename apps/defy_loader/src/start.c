/*
 * soc_reset_hook(), the loader's first code after n2_bootloader starts it,
 * ahead of SystemInit() and the rest of Zephyr's start.
 *
 * It decides between ZMK and the drive before anything else runs
 * (boot_mode.c), and writes GPREGRET's new value: one more unconfirmed start
 * before a jump to ZMK, so that a ZMK that keeps failing gets the drive
 * after DEFY_LOADER_START_LIMIT starts. To start ZMK it points VTOR and the
 * main stack at ZMK's vector table and branches to ZMK's reset vector, with
 * the core, clocks and peripherals as n2_bootloader left them, so ZMK starts
 * as it would from n2_bootloader directly and runs its own SystemInit(). For
 * the drive, it runs SystemInit() and returns to Zephyr's start, which ends
 * in main.c.
 *
 * Nordic's SoC support sets soc_reset_hook to SystemInit with PROVIDE() in
 * its linker script, which applies only while nothing else defines the
 * symbol. This definition replaces it. CMakeLists.txt pins that linker
 * script and the Cortex-M reset.S that calls the hook.
 *
 * Zephyr has not set up RAM yet, so this file uses no static variables. It
 * never touches GPREGRET2 or UICR.
 *
 * SPDX-License-Identifier: MIT
 */

#include <stdint.h>

#include <nrf.h>
#include <zephyr/platform/hooks.h>
#include <zephyr/sys/util_macro.h>
#include <zephyr/toolchain.h>

#include "boot_mode.h"
#include "layout.h"

/* reset.S calls soc_reset_hook() only with this option. */
BUILD_ASSERT(IS_ENABLED(CONFIG_SOC_RESET_HOOK));
/* With the CMake linker generator, Nordic binds soc_reset_hook to SystemInit outside
 * platform_init.ld, and that binding would win over this file's. */
BUILD_ASSERT(!IS_ENABLED(CONFIG_CMAKE_LINKER_GENERATOR));

static FUNC_NORETURN void start_zmk(uint32_t stack, uint32_t reset) {
    SCB->VTOR = DEFY_LOADER_ZMK_START;
    __DSB();
    __ISB();
    __asm__ volatile("msr msp, %0\n\t"
                     "bx %1\n\t"
                     :
                     : "r"(stack), "r"(reset)
                     : "memory");
    CODE_UNREACHABLE;
}

void soc_reset_hook(void) {
    const volatile uint32_t *vectors = (const volatile uint32_t *)DEFY_LOADER_ZMK_START;
    uint32_t stack = vectors[0];
    uint32_t reset = vectors[1];
    uint32_t gpregret = NRF_POWER->GPREGRET;
    enum defy_loader_start mode = defy_loader_start_mode(gpregret, stack, reset);

    NRF_POWER->GPREGRET = defy_loader_start_gpregret(mode, gpregret);
    if (mode == DEFY_LOADER_START_ZMK) {
        start_zmk(stack, reset);
    }
    SystemInit();
}
