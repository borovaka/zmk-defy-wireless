/*
 * Quiescing the SPI slave rf_bootloader leaves enabled, for startup.c. It
 * runs before Zephyr starts, so it reaches the registers only through
 * boot_io.h, and times itself with SysTick counting core clock cycles, its
 * interrupt off.
 *
 * boot_io.h comes in angle brackets, which skip this file's own directory:
 * tests/host/defy_radio_boot puts its own boot_io.h, which fakes the
 * registers, ahead of this directory on the include path.
 *
 * SPDX-License-Identifier: MIT
 */

#include "boot.h"
#include "boot_registers.h"
#include <boot_io.h>
#include <stddef.h>

#define QUIESCE_TIMEOUT_CYCLES (DEFY_RADIO_BOOT_QUIESCE_TIMEOUT_US * DEFY_BOOT_CORE_CYCLES_PER_US)
#define WATCHDOG_FEED_CYCLES (1000u * DEFY_BOOT_CORE_CYCLES_PER_US)
/* SysTick reads the same value this many times in a row: it is not counting. */
#define CLOCK_STALL_LIMIT 8u

static void stop_clock(void) {
    defy_radio_boot_write(DEFY_BOOT_SYSTICK_CTRL, 0);
    defy_radio_boot_write(DEFY_BOOT_SYSTICK_LOAD, 0);
    defy_radio_boot_write(DEFY_BOOT_SYSTICK_VALUE, 0);
    defy_radio_boot_write(DEFY_BOOT_ICSR, DEFY_BOOT_CLEAR_PENDING_SYSTICK);
}

static uint32_t clock_value(void) {
    return defy_radio_boot_read(DEFY_BOOT_SYSTICK_VALUE) & DEFY_BOOT_SYSTICK_MASK;
}

static uint32_t elapsed_cycles(uint32_t start, uint32_t current) {
    return (start - current) & DEFY_BOOT_SYSTICK_MASK;
}

bool defy_radio_boot_quiesce_spis(void (*keep_alive)(void)) {
    uint32_t enabled = defy_radio_boot_read(DEFY_BOOT_SPIS_ENABLE);

    if (enabled == 0) {
        return true;
    }
    if (enabled != DEFY_BOOT_SPIS_ENABLED) {
        return false;
    }

    uint32_t pin = defy_radio_boot_read(DEFY_BOOT_SPIS_CSN);

    if (pin >= 32u) {
        return false;
    }
    stop_clock();
    defy_radio_boot_write(DEFY_BOOT_SYSTICK_LOAD, DEFY_BOOT_SYSTICK_MASK);
    defy_radio_boot_write(DEFY_BOOT_SYSTICK_CTRL,
                          DEFY_BOOT_SYSTICK_ENABLE | DEFY_BOOT_SYSTICK_CORE_CLOCK);
    uint32_t started = clock_value();
    uint32_t previous = started;
    uint32_t last_feed = 0;
    unsigned int stalled = 0;
    bool quiesced = false;

    for (;;) {
        uint32_t current = clock_value();
        uint32_t elapsed = elapsed_cycles(started, current);

        if (elapsed >= QUIESCE_TIMEOUT_CYCLES) {
            break;
        }
        if (current == previous) {
            if (++stalled >= CLOCK_STALL_LIMIT) {
                break;
            }
        } else {
            stalled = 0;
        }
        previous = current;
        if (keep_alive != NULL && elapsed - last_feed >= WATCHDOG_FEED_CYCLES) {
            keep_alive();
            last_feed = elapsed;
        }
        if ((defy_radio_boot_read(DEFY_BOOT_GPIO_IN) & (1u << pin)) == 0) {
            continue;
        }

        defy_radio_boot_write(DEFY_BOOT_SPIS_ACQUIRE, 1u);
        if ((defy_radio_boot_read(DEFY_BOOT_SPIS_SEMSTAT) & DEFY_BOOT_SPIS_SEMSTAT_MASK) !=
                DEFY_BOOT_SPIS_CPU_OWNER ||
            (defy_radio_boot_read(DEFY_BOOT_GPIO_IN) & (1u << pin)) == 0) {
            continue;
        }
        if (elapsed_cycles(started, clock_value()) >= QUIESCE_TIMEOUT_CYCLES) {
            break;
        }

        defy_radio_boot_write(DEFY_BOOT_SPIS_INTENCLR, UINT32_MAX);
        defy_radio_boot_write(DEFY_BOOT_SPIS_ENABLE, 0);
        quiesced = defy_radio_boot_read(DEFY_BOOT_SPIS_ENABLE) == 0;
        break;
    }
    stop_clock();
    return quiesced;
}
