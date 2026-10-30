/*
 * The RP2040 registers that the halves' power and fault code use, for tests
 * on the host and on native_sim, in place of the Pico SDK's hardware/structs/.
 * Each block keeps the fields that code touches, in the RP2040's order; the
 * bit values are the RP2040's.
 *
 * A test defines the fake_* blocks it uses, hw_set_bits(), hw_clear_bits() and
 * the four fake_*_get() functions. The code reaches clocks, rosc, watchdog and
 * xosc through those functions, so that a test can answer as the hardware
 * would, such as a clock that switches source.
 *
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include <stdint.h>

#include <hardware/address_mapped.h>

struct fake_resets {
    volatile uint32_t reset, reset_done;
};

struct fake_irq_ctrl {
    volatile uint32_t inte[4], intf[4], ints[4];
};

struct fake_io_bank0 {
    struct {
        volatile uint32_t ctrl;
    } io[30];
    volatile uint32_t intr[4];
    struct fake_irq_ctrl proc0_irq_ctrl, dormant_wake_irq_ctrl;
};

struct fake_pads_bank0 {
    volatile uint32_t io[30];
};

struct fake_sio {
    volatile uint32_t gpio_in, gpio_out, gpio_clr, gpio_oe, gpio_oe_set, gpio_oe_clr;
};

struct fake_clock {
    volatile uint32_t ctrl, selected;
};

struct fake_clocks {
    struct fake_clock clk[6];
};

struct fake_pll {
    volatile uint32_t pwr;
};

struct fake_psm {
    volatile uint32_t wdsel;
};

struct fake_rosc {
    volatile uint32_t ctrl, status;
};

/* The RP2040's layout in full: fatal recovery uses every register. */
struct fake_watchdog {
    volatile uint32_t ctrl, load, reason, scratch[8], tick;
};

struct fake_xosc {
    volatile uint32_t ctrl, status, dormant, startup;
};

extern struct fake_resets fake_resets;
extern struct fake_io_bank0 fake_io_bank0;
extern struct fake_pads_bank0 fake_pads_bank0;
extern struct fake_sio fake_sio;
extern struct fake_clocks fake_clocks;
extern struct fake_pll fake_pll_sys, fake_pll_usb;
extern struct fake_psm fake_psm;
extern struct fake_rosc fake_rosc;
extern struct fake_watchdog fake_watchdog;
extern struct fake_xosc fake_xosc;

struct fake_clocks *fake_clocks_get(void);
struct fake_rosc *fake_rosc_get(void);
struct fake_watchdog *fake_watchdog_get(void);
struct fake_xosc *fake_xosc_get(void);

#define resets_hw (&fake_resets)
#define io_bank0_hw (&fake_io_bank0)
#define pads_bank0_hw (&fake_pads_bank0)
#define sio_hw (&fake_sio)
#define clocks_hw fake_clocks_get()
#define pll_sys_hw (&fake_pll_sys)
#define pll_usb_hw (&fake_pll_usb)
#define psm_hw (&fake_psm)
#define rosc_hw fake_rosc_get()
#define watchdog_hw fake_watchdog_get()
#define xosc_hw fake_xosc_get()

enum { clk_ref, clk_sys, clk_usb, clk_adc, clk_rtc, clk_peri };

#define CLOCKS_CLK_REF_CTRL_SRC_BITS 3u
#define CLOCKS_CLK_REF_CTRL_SRC_VALUE_XOSC_CLKSRC 2u
#define CLOCKS_CLK_REF_CTRL_SRC_LSB 0u
#define CLOCKS_CLK_SYS_CTRL_SRC_BITS 1u
#define CLOCKS_CLK_SYS_CTRL_SRC_VALUE_CLK_REF 0u
#define CLOCKS_CLK_USB_CTRL_ENABLE_BITS 0x800u
#define CLOCKS_CLK_ADC_CTRL_ENABLE_BITS 0x800u
#define CLOCKS_CLK_RTC_CTRL_ENABLE_BITS 0x800u
#define CLOCKS_CLK_PERI_CTRL_ENABLE_BITS 0x800u
#define PADS_BANK0_GPIO0_IE_BITS 0x40u
#define PADS_BANK0_GPIO0_DRIVE_LSB 4u
#define PADS_BANK0_GPIO0_DRIVE_VALUE_4MA 1u
#define PADS_BANK0_GPIO0_PUE_BITS 0x8u
#define PADS_BANK0_GPIO0_SCHMITT_BITS 0x2u
#define PLL_PWR_BITS 0x2du
#define PSM_WDSEL_BITS 0x1ffffu
#define PSM_WDSEL_ROSC_BITS 0x1u
#define PSM_WDSEL_XOSC_BITS 0x2u
#define RESETS_RESET_IO_BANK0_BITS 0x20u
#define RESETS_RESET_PADS_BANK0_BITS 0x100u
#define ROSC_CTRL_ENABLE_BITS 0x00fff000u
#define ROSC_CTRL_ENABLE_VALUE_ENABLE 0xfabu
#define ROSC_CTRL_ENABLE_VALUE_DISABLE 0xd1eu
#define ROSC_CTRL_ENABLE_LSB 12
#define ROSC_STATUS_STABLE_BITS 0x80000000u
#define WATCHDOG_CTRL_ENABLE_BITS 0x40000000u
#define WATCHDOG_CTRL_TRIGGER_BITS 0x80000000u
#define WATCHDOG_CTRL_PAUSE_DBG1_BITS 0x04000000u
#define WATCHDOG_CTRL_PAUSE_DBG0_BITS 0x02000000u
#define WATCHDOG_CTRL_PAUSE_JTAG_BITS 0x01000000u
#define WATCHDOG_CTRL_TIME_BITS 0x00ffffffu
#define WATCHDOG_REASON_TIMER_BITS 1u
#define WATCHDOG_REASON_FORCE_BITS 2u
#define WATCHDOG_TICK_ENABLE_BITS 0x200u
#define XOSC_CTRL_ENABLE_VALUE_ENABLE 0xfabu
#define XOSC_CTRL_ENABLE_LSB 12
#define XOSC_CTRL_FREQ_RANGE_VALUE_1_15MHZ 0xaa0u
#define XOSC_STATUS_ENABLED_BITS 0x1000u
#define XOSC_STATUS_STABLE_BITS 0x80000000u
#define XOSC_DORMANT_VALUE_DORMANT 0x636f6d61u
