/*
 * The RP2040's DORMANT state on a half, woken by a key and left through a
 * watchdog reset: for its sleep, soc/rp2040/poweroff.c, and for holding it
 * after repeated faults, diagnostics/defy_fatal_recovery.c. Also the key
 * matrix's pins, which both arm as the wake source, and the watchdog scratch
 * registers both keep through the reset.
 *
 * The clock sequence follows pico-extras' pico_sleep (BSD-3-Clause):
 * sleep_run_from_dormant_source() and sleep_goto_dormant_until_pin(). Nothing
 * needs restoring after the wake, because the watchdog resets the chip.
 *
 * Every function here is inline and has no stack protector, so that the fault
 * path can run them from the reset hook, before Zephyr starts.
 *
 * SPDX-License-Identifier: MIT
 */

#ifndef DYGMA_DEFY_RP2040_DORMANT_H
#define DYGMA_DEFY_RP2040_DORMANT_H

#include <stdint.h>

#include <zephyr/devicetree.h>
#include <zephyr/dt-bindings/gpio/gpio.h>
#include <zephyr/sys/util.h>
#include <zephyr/toolchain.h>

#include <hardware/structs/clocks.h>
#include <hardware/structs/io_bank0.h>
#include <hardware/structs/pll.h>
#include <hardware/structs/psm.h>
#include <hardware/structs/rosc.h>
#include <hardware/structs/watchdog.h>
#include <hardware/structs/xosc.h>

/*
 * Watchdog scratch registers, which keep their values through a watchdog
 * reset. The boot ROM reads 4 to 7 after one: its magic in 4 makes it jump to
 * the entry point the others hold instead of starting from flash.
 */
enum defy_rp2040_scratch {
    /* defy_fatal_recovery.c's count of quick faults, then its complement. */
    DEFY_SCRATCH_FAULT = 0,
    DEFY_SCRATCH_FAULT_CHECK = 1,
    /* poweroff.c's marker of a wake by a key, then its complement. */
    DEFY_SCRATCH_KEY_WAKE = 2,
    DEFY_SCRATCH_KEY_WAKE_CHECK = 3,
    /* The boot ROM's magic. */
    DEFY_SCRATCH_BOOT_MAGIC = 4,
};

/*
 * The key matrix: lines on GPIO 8-12, driven low one at a time, and inputs on
 * GPIO 0-7, pulled up, which read low while a key on the driven line is down.
 */
#define DEFY_MATRIX_NODE DT_NODELABEL(kscan0)
#define DEFY_MATRIX_PIN(node, prop, idx) | BIT(DT_GPIO_PIN_BY_IDX(node, prop, idx))
#define DEFY_MATRIX_COLUMNS (0 DT_FOREACH_PROP_ELEM(DEFY_MATRIX_NODE, col_gpios, DEFY_MATRIX_PIN))
#define DEFY_MATRIX_ROWS (0 DT_FOREACH_PROP_ELEM(DEFY_MATRIX_NODE, row_gpios, DEFY_MATRIX_PIN))
#define DEFY_MATRIX_PIN_CHECK(node, prop, idx)                                                     \
    BUILD_ASSERT(DT_SAME_NODE(DT_GPIO_CTLR_BY_IDX(node, prop, idx), DT_NODELABEL(gpio0)));         \
    BUILD_ASSERT(DT_GPIO_FLAGS_BY_IDX(node, prop, idx) & GPIO_ACTIVE_LOW);

/* The interrupt bits below cover the inputs on GPIO 0-7 alone. */
BUILD_ASSERT(DEFY_MATRIX_COLUMNS == 0xff && DT_PROP_LEN(DEFY_MATRIX_NODE, col_gpios) == 8);
BUILD_ASSERT(DEFY_MATRIX_ROWS == 0x1f00 && DT_PROP_LEN(DEFY_MATRIX_NODE, row_gpios) == 5);
DT_FOREACH_PROP_ELEM(DEFY_MATRIX_NODE, col_gpios, DEFY_MATRIX_PIN_CHECK)
DT_FOREACH_PROP_ELEM(DEFY_MATRIX_NODE, row_gpios, DEFY_MATRIX_PIN_CHECK)

/* The inputs' bits in INTR0 and in its enables, four per GPIO: level low, then edge low. */
#define DEFY_MATRIX_LEVEL_LOW 0x11111111u
#define DEFY_MATRIX_EDGE_LOW 0x44444444u

/* DORMANT ends on these events of GPIO 0-7, in INTR0's layout, and on nothing else. */
static inline FUNC_NO_STACK_PROTECTOR void defy_rp2040_dormant_wake_on(uint32_t events) {
    for (unsigned int i = 0; i < ARRAY_SIZE(io_bank0_hw->dormant_wake_irq_ctrl.inte); i++) {
        io_bank0_hw->dormant_wake_irq_ctrl.inte[i] = i == 0 ? events : 0;
        io_bank0_hw->dormant_wake_irq_ctrl.intf[i] = 0;
    }
}

/* Every clock from the crystal, which must be running, and every other source off. */
static inline FUNC_NO_STACK_PROTECTOR void defy_rp2040_run_from_xosc(void) {
    /* clk_ref from the crystal. */
    clocks_hw->clk[clk_ref].ctrl =
        (clocks_hw->clk[clk_ref].ctrl & ~CLOCKS_CLK_REF_CTRL_SRC_BITS) |
        (CLOCKS_CLK_REF_CTRL_SRC_VALUE_XOSC_CLKSRC << CLOCKS_CLK_REF_CTRL_SRC_LSB);
    while (!(clocks_hw->clk[clk_ref].selected & BIT(CLOCKS_CLK_REF_CTRL_SRC_VALUE_XOSC_CLKSRC))) {
    }

    /* clk_sys from clk_ref, off the system PLL. */
    hw_clear_bits(&clocks_hw->clk[clk_sys].ctrl, CLOCKS_CLK_SYS_CTRL_SRC_BITS);
    while (!(clocks_hw->clk[clk_sys].selected & BIT(CLOCKS_CLK_SYS_CTRL_SRC_VALUE_CLK_REF))) {
    }

    hw_clear_bits(&clocks_hw->clk[clk_usb].ctrl, CLOCKS_CLK_USB_CTRL_ENABLE_BITS);
    hw_clear_bits(&clocks_hw->clk[clk_adc].ctrl, CLOCKS_CLK_ADC_CTRL_ENABLE_BITS);
    hw_clear_bits(&clocks_hw->clk[clk_rtc].ctrl, CLOCKS_CLK_RTC_CTRL_ENABLE_BITS);
    hw_clear_bits(&clocks_hw->clk[clk_peri].ctrl, CLOCKS_CLK_PERI_CTRL_ENABLE_BITS);

    pll_sys_hw->pwr = PLL_PWR_BITS;
    pll_usb_hw->pwr = PLL_PWR_BITS;

    /* The ring oscillator would keep running through DORMANT. */
    rosc_hw->ctrl = (rosc_hw->ctrl & ~ROSC_CTRL_ENABLE_BITS) |
                    (ROSC_CTRL_ENABLE_VALUE_DISABLE << ROSC_CTRL_ENABLE_LSB);
    while (rosc_hw->status & ROSC_STATUS_STABLE_BITS) {
    }
}

/* Stops the crystal and with it every clock, until a wake event. */
static inline FUNC_NO_STACK_PROTECTOR void defy_rp2040_dormant(void) {
    xosc_hw->dormant = XOSC_DORMANT_VALUE_DORMANT;
    while (!(xosc_hw->status & XOSC_STATUS_STABLE_BITS)) {
    }
}

/* Resets everything but the oscillators, like the Pico SDK's watchdog_reboot(). */
static inline FUNC_NO_STACK_PROTECTOR FUNC_NORETURN void defy_rp2040_watchdog_reset(void) {
    /*
     * The reset spares both oscillators but resets the clock block, whose
     * clk_ref then runs from the ring oscillator. It has to be running again,
     * or the chip would come out of reset without a clock.
     */
    rosc_hw->ctrl = (rosc_hw->ctrl & ~ROSC_CTRL_ENABLE_BITS) |
                    (ROSC_CTRL_ENABLE_VALUE_ENABLE << ROSC_CTRL_ENABLE_LSB);
    while (!(rosc_hw->status & ROSC_STATUS_STABLE_BITS)) {
    }

    hw_clear_bits(&watchdog_hw->ctrl, WATCHDOG_CTRL_ENABLE_BITS);
    psm_hw->wdsel = PSM_WDSEL_BITS & ~(PSM_WDSEL_ROSC_BITS | PSM_WDSEL_XOSC_BITS);
    hw_set_bits(&watchdog_hw->ctrl, WATCHDOG_CTRL_TRIGGER_BITS);

    for (;;) {
    }
}

#endif
