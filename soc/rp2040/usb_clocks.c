/*
 * The halves have no use for the RP2040's USB PLL or the USB, ADC and RTC
 * clocks it drives: they build no USB stack, ADC or RTC. Zephyr's clock driver
 * starts them with every other clock, and this turns them off again once it
 * has run. Dormant sleep stops them anyway, so this saves only while a half is
 * awake. The next start runs the clock driver again, which starts the PLL.
 *
 * SPDX-License-Identifier: MIT
 */

#include <errno.h>

#include <zephyr/device.h>
#include <zephyr/drivers/clock_control.h>
#include <zephyr/dt-bindings/clock/rpi_pico_rp2040_clock.h>
#include <zephyr/init.h>
#include <zephyr/sys/util.h>
#include <zephyr/toolchain.h>

#define FROM_PLL_USB(clk) DT_SAME_NODE(DT_CLOCKS_CTLR(DT_NODELABEL(clk)), DT_NODELABEL(pll_usb))

BUILD_ASSERT(!FROM_PLL_USB(clk_ref) && !FROM_PLL_USB(clk_sys) && !FROM_PLL_USB(clk_peri),
             "the clocks that stay on must not run from the USB PLL");

static int usb_clocks_off(void) {
    const struct device *clocks = DEVICE_DT_GET(DT_NODELABEL(clocks));
    /* The clocks first, then the PLL that drives them. */
    static const uint32_t ids[] = {RPI_PICO_CLKID_CLK_USB, RPI_PICO_CLKID_CLK_ADC,
                                   RPI_PICO_CLKID_CLK_RTC, RPI_PICO_CLKID_PLL_USB};

    if (!device_is_ready(clocks)) {
        return -ENODEV;
    }
    for (size_t i = 0; i < ARRAY_SIZE(ids); i++) {
        int err = clock_control_off(clocks, (clock_control_subsys_t)ids[i]);

        if (err < 0) {
            return err;
        }
    }
    return 0;
}

SYS_INIT(usb_clocks_off, PRE_KERNEL_2, 0);
