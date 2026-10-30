/* SPDX-License-Identifier: MIT */

#include <errno.h>
#include <setjmp.h>
#include <string.h>

#include <zephyr/drivers/kscan.h>
#include <zephyr/irq.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/reboot.h>
#include <zephyr/ztest.h>

#include <dygma/defy_half_power.h>
#include <fake_registers.h>

#include "../../../../soc/rp2040/poweroff.c"

struct fake_io_bank0 fake_io_bank0;
struct fake_sio fake_sio;
struct fake_clocks fake_clocks;
struct fake_pll fake_pll_sys, fake_pll_usb;
struct fake_psm fake_psm;
struct fake_rosc fake_rosc;
struct fake_watchdog fake_watchdog;
struct fake_xosc fake_xosc;

struct fake_clocks *fake_clocks_get(void) { return &fake_clocks; }

struct fake_watchdog *fake_watchdog_get(void) { return &fake_watchdog; }

static jmp_buf exit_env;
static bool exit_armed;
static bool inject_transition_edge;
static int scan_error;
static unsigned int scan_calls;
static uint32_t rescanned_columns;
static int reboot_type;

void hw_clear_bits(volatile uint32_t *reg, uint32_t bits) {
    *reg &= ~bits;
    if (inject_transition_edge && reg == &fake_clocks.clk[clk_sys].ctrl) {
        fake_io_bank0.intr[0] |= BIT(14);
    }
}

void hw_set_bits(volatile uint32_t *reg, uint32_t bits) {
    *reg |= bits;
    if (reg == &fake_watchdog.ctrl && (bits & WATCHDOG_CTRL_TRIGGER_BITS)) {
        fake_watchdog.reason = WATCHDOG_REASON_FORCE_BITS;
        zassert_true(exit_armed);
        longjmp(exit_env, 3);
    }
}

struct fake_rosc *fake_rosc_get(void) {
    bool enabled = (fake_rosc.ctrl & ROSC_CTRL_ENABLE_BITS) ==
                   (ROSC_CTRL_ENABLE_VALUE_ENABLE << ROSC_CTRL_ENABLE_LSB);
    fake_rosc.status = enabled ? ROSC_STATUS_STABLE_BITS : 0;
    return &fake_rosc;
}

struct fake_xosc *fake_xosc_get(void) {
    if (fake_xosc.dormant == XOSC_DORMANT_VALUE_DORMANT) {
        zassert_true(exit_armed);
        if (!(fake_io_bank0.intr[0] & fake_io_bank0.dormant_wake_irq_ctrl.inte[0])) {
            longjmp(exit_env, 2);
        }
    }
    return &fake_xosc;
}

FUNC_NORETURN void __wrap_sys_reboot(int type) {
    zassert_true(exit_armed);
    reboot_type = type;
    longjmp(exit_env, 1);
}

static int enable_scan(const struct device *dev) {
    ARG_UNUSED(dev);
    scan_calls++;
    rescanned_columns = ~fake_sio.gpio_in & 0xff;
    if (scan_error != 0) {
        return scan_error;
    }
    fake_io_bank0.proc0_irq_ctrl.inte[0] = 0x11111111;
    return 0;
}

static DEVICE_API(kscan, scan_api) = {.enable_callback = enable_scan};
DEVICE_DT_DEFINE(DT_NODELABEL(kscan0), NULL, NULL, NULL, NULL, POST_KERNEL,
                 CONFIG_KERNEL_INIT_PRIORITY_DEVICE, &scan_api);

enum operation {PREPARE, CANCEL, ENTER, GENERIC_ENTER};
static enum operation requested;
static int result;
static int exit_path;
static K_SEM_DEFINE(work_done, 0, 1);
extern FUNC_NORETURN void z_sys_poweroff(void);

static void run_operation(struct k_work *work) {
    ARG_UNUSED(work);
    if (requested == PREPARE) {
        result = defy_half_power_prepare();
    } else if (requested == CANCEL) {
        result = defy_half_power_cancel();
    } else {
        unsigned int irq_key = irq_lock();
        irq_unlock(irq_key);
        exit_armed = true;
        exit_path = setjmp(exit_env);
        if (exit_path == 0) {
            if (requested == ENTER) {
                defy_half_power_enter();
            }
            z_sys_poweroff();
        }
        irq_unlock(irq_key);
        exit_armed = false;
    }
    k_sem_give(&work_done);
}

static K_WORK_DEFINE(operation_work, run_operation);

static int run(enum operation operation) {
    requested = operation;
    result = 0;
    exit_path = 0;
    k_work_submit(&operation_work);
    zassert_ok(k_sem_take(&work_done, K_MSEC(100)));
    return result;
}

static void before(void *fixture) {
    ARG_UNUSED(fixture);
    (void)run(CANCEL);
    memset(&fake_io_bank0, 0, sizeof(fake_io_bank0));
    memset(&fake_sio, 0, sizeof(fake_sio));
    memset(&fake_clocks, 0, sizeof(fake_clocks));
    memset(&fake_watchdog, 0, sizeof(fake_watchdog));
    zassert_ok(wake_reason_init());
    memset(&fake_xosc, 0, sizeof(fake_xosc));
    memset(&fake_rosc, 0, sizeof(fake_rosc));
    memset(&fake_psm, 0, sizeof(fake_psm));
    fake_io_bank0.proc0_irq_ctrl.inte[0] = 0x11111111;
    fake_io_bank0.proc0_irq_ctrl.inte[3] = BIT(3);
    fake_sio.gpio_in = 0xff;
    fake_sio.gpio_oe = 0x1f00;
    fake_clocks.clk[clk_ref].selected = BIT(CLOCKS_CLK_REF_CTRL_SRC_VALUE_XOSC_CLKSRC);
    fake_clocks.clk[clk_sys].selected = BIT(0);
    fake_xosc.status = XOSC_STATUS_STABLE_BITS;
    inject_transition_edge = false;
    scan_error = 0;
    scan_calls = 0;
    rescanned_columns = 0;
}

ZTEST(defy_half_power, test_prepare_masks_only_columns_without_clearing_edges) {
    fake_io_bank0.intr[0] = BIT(3);
    fake_io_bank0.intr[3] = BIT(3);
    zassert_ok(run(PREPARE));
    zassert_equal(fake_io_bank0.proc0_irq_ctrl.inte[0], 0);
    zassert_equal(fake_io_bank0.proc0_irq_ctrl.inte[3], BIT(3));
    zassert_equal(fake_io_bank0.intr[0], BIT(3));
    zassert_equal(fake_io_bank0.intr[3], BIT(3));
    zassert_equal(fake_sio.gpio_oe, 0x1f00);
    zassert_equal(fake_sio.gpio_out, 0);
    zassert_false(defy_half_power_pending());
    zassert_equal(run(PREPARE), -EALREADY);
}

ZTEST(defy_half_power, test_unarmed_scanner_vetoes_prepare) {
    fake_io_bank0.proc0_irq_ctrl.inte[0] &= ~BIT(4);
    uint32_t original = fake_io_bank0.proc0_irq_ctrl.inte[0];
    zassert_equal(run(PREPARE), -EBUSY);
    zassert_equal(fake_io_bank0.proc0_irq_ctrl.inte[0], original);
}

ZTEST(defy_half_power, test_rows_must_remain_active_outputs) {
    fake_sio.gpio_out = BIT(8);
    zassert_equal(run(PREPARE), -EBUSY);
    fake_sio.gpio_out = 0;
    fake_sio.gpio_oe &= ~BIT(12);
    zassert_equal(run(PREPARE), -EBUSY);
    zassert_equal(fake_io_bank0.proc0_irq_ctrl.inte[0], 0x11111111);
}

ZTEST(defy_half_power, test_held_or_pending_press_vetoes_prepare) {
    fake_sio.gpio_in &= ~BIT(4);
    zassert_equal(run(PREPARE), -EBUSY);
    fake_sio.gpio_in = 0xff;
    fake_io_bank0.intr[0] = BIT(18);
    zassert_equal(run(PREPARE), -EBUSY);
    zassert_equal(fake_io_bank0.intr[0], BIT(18));
    fake_io_bank0.intr[0] = BIT(16);
    zassert_equal(run(PREPARE), -EBUSY);
}

ZTEST(defy_half_power, test_short_tap_survives_shutdown_gap) {
    zassert_ok(run(PREPARE));
    fake_io_bank0.intr[0] = BIT(22) | BIT(23);
    zassert_true(defy_half_power_pending());
    zassert_ok(run(ENTER));
    zassert_equal(exit_path, 1);
    zassert_equal(reboot_type, SYS_REBOOT_COLD);
    zassert_equal(fake_io_bank0.intr[0], BIT(22) | BIT(23));
    zassert_equal(fake_xosc.dormant, 0);
}

ZTEST(defy_half_power, test_held_key_during_shutdown_reboots) {
    zassert_ok(run(PREPARE));
    fake_sio.gpio_in &= ~BIT(2);
    zassert_true(defy_half_power_pending());
    zassert_ok(run(ENTER));
    zassert_equal(exit_path, 1);
}

ZTEST(defy_half_power, test_cancel_rescans_pending_key) {
    zassert_ok(run(PREPARE));
    fake_sio.gpio_in &= ~BIT(1);
    zassert_ok(run(CANCEL));
    zassert_equal(scan_calls, 1);
    zassert_equal(rescanned_columns, BIT(1));
    zassert_equal(fake_io_bank0.proc0_irq_ctrl.inte[0], 0x11111111);
    zassert_true(defy_half_power_pending());
}

ZTEST(defy_half_power, test_cancel_propagates_scanner_error) {
    zassert_ok(run(PREPARE));
    scan_error = -EIO;
    zassert_equal(run(CANCEL), -EIO);
    zassert_true(defy_half_power_pending());
    zassert_equal(fake_io_bank0.proc0_irq_ctrl.inte[0], 0);
}

ZTEST(defy_half_power, test_unprepared_entry_reboots) {
    zassert_ok(run(ENTER));
    zassert_equal(exit_path, 1);
    zassert_ok(run(GENERIC_ENTER));
    zassert_equal(exit_path, 1);
    zassert_equal(fake_xosc.dormant, 0);
}

ZTEST(defy_half_power, test_entry_arms_explicit_low_and_falling_masks) {
    zassert_ok(run(PREPARE));
    for (int i = 0; i < 4; i++) {
        fake_io_bank0.dormant_wake_irq_ctrl.inte[i] = 0xffffffff;
        fake_io_bank0.dormant_wake_irq_ctrl.intf[i] = 0xffffffff;
    }
    zassert_ok(run(ENTER));
    zassert_equal(exit_path, 2);
    zassert_equal(fake_io_bank0.dormant_wake_irq_ctrl.inte[0], 0x55555555);
    for (int i = 0; i < 4; i++) {
        zassert_equal(fake_io_bank0.dormant_wake_irq_ctrl.intf[i], 0);
        if (i != 0) {
            zassert_equal(fake_io_bank0.dormant_wake_irq_ctrl.inte[i], 0);
        }
    }
    zassert_equal(fake_xosc.dormant, XOSC_DORMANT_VALUE_DORMANT);
    zassert_equal(fake_pll_sys.pwr, PLL_PWR_BITS);
    zassert_equal(fake_pll_usb.pwr, PLL_PWR_BITS);
}

ZTEST(defy_half_power, test_edge_during_clock_transition_wakes_and_resets) {
    zassert_ok(run(PREPARE));
    inject_transition_edge = true;
    zassert_ok(run(ENTER));
    zassert_equal(exit_path, 3);
    zassert_equal(fake_io_bank0.intr[0], BIT(14));
    zassert_equal(fake_io_bank0.dormant_wake_irq_ctrl.inte[0], 0x55555555);
    zassert_true(fake_watchdog.ctrl & WATCHDOG_CTRL_TRIGGER_BITS);
    zassert_equal(fake_psm.wdsel, PSM_WDSEL_BITS & ~(PSM_WDSEL_ROSC_BITS | PSM_WDSEL_XOSC_BITS));
}

ZTEST(defy_half_power, test_dormant_wake_marker_is_consumed_once) {
    for (size_t i = 0; i < ARRAY_SIZE(fake_watchdog.scratch); i++) {
        fake_watchdog.scratch[i] = 0x12345600u + i;
    }
    zassert_ok(run(PREPARE));
    inject_transition_edge = true;
    zassert_ok(run(ENTER));
    zassert_equal(exit_path, 3);
    zassert_ok(wake_reason_init());
    zassert_true(defy_half_power_woke_from_key());
    zassert_true(defy_half_power_woke_from_key());
    for (size_t i = 0; i < ARRAY_SIZE(fake_watchdog.scratch); i++) {
        zassert_equal(fake_watchdog.scratch[i], i == 2 || i == 3 ? 0 : 0x12345600u + i);
    }
    zassert_ok(wake_reason_init());
    zassert_false(defy_half_power_woke_from_key());
}

ZTEST(defy_half_power, test_only_a_forced_reset_with_both_marker_words_is_a_key_wake) {
    /* Reset reason, scratch 2 and scratch 3: each row fails one term. */
    const uint32_t resets[][3] = {
        {WATCHDOG_REASON_FORCE_BITS, 0, 0},
        {0, 0x44594b57u, ~0x44594b57u},
        {WATCHDOG_REASON_TIMER_BITS, 0x44594b57u, ~0x44594b57u},
        {WATCHDOG_REASON_FORCE_BITS, 0x44594b57u, 0},
        {WATCHDOG_REASON_FORCE_BITS, 0, ~0x44594b57u},
        {WATCHDOG_REASON_FORCE_BITS, 0x44594b56u, ~0x44594b56u},
        {WATCHDOG_REASON_FORCE_BITS, 0x44594b57u, ~0x44594b56u},
    };
    for (size_t i = 0; i < ARRAY_SIZE(resets); i++) {
        fake_watchdog.reason = resets[i][0];
        fake_watchdog.scratch[2] = resets[i][1];
        fake_watchdog.scratch[3] = resets[i][2];
        zassert_ok(wake_reason_init());
        zassert_false(defy_half_power_woke_from_key());
        zassert_equal(fake_watchdog.scratch[2], 0);
        zassert_equal(fake_watchdog.scratch[3], 0);
    }
}

ZTEST(defy_half_power, test_entry_abort_does_not_create_wake_marker) {
    zassert_ok(run(ENTER));
    zassert_equal(exit_path, 1);
    fake_watchdog.reason = WATCHDOG_REASON_FORCE_BITS;
    zassert_ok(wake_reason_init());
    zassert_false(defy_half_power_woke_from_key());

    zassert_ok(run(PREPARE));
    fake_io_bank0.intr[0] = BIT(22);
    zassert_ok(run(ENTER));
    zassert_equal(exit_path, 1);
    zassert_ok(wake_reason_init());
    zassert_false(defy_half_power_woke_from_key());
}

ZTEST(defy_half_power, test_waiting_in_dormant_does_not_set_marker) {
    zassert_ok(run(PREPARE));
    zassert_ok(run(ENTER));
    zassert_equal(exit_path, 2);
    zassert_equal(fake_watchdog.scratch[2], 0);
    zassert_equal(fake_watchdog.scratch[3], 0);
    zassert_false(defy_half_power_woke_from_key());
}

ZTEST_SUITE(defy_half_power, NULL, NULL, before, NULL, NULL);
