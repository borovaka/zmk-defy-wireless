/* SPDX-License-Identifier: MIT */

#include <assert.h>
#include <setjmp.h>
#include <stdbool.h>
#include <string.h>

#include "../../../diagnostics/defy_fatal_recovery.c"

struct fake_systick fake_systick;
struct fake_nvic fake_nvic;
struct fake_scb fake_scb;
struct fake_resets fake_resets;
struct fake_io_bank0 fake_io_bank0;
struct fake_pads_bank0 fake_pads_bank0;
struct fake_sio fake_sio;
struct fake_clocks fake_clocks;
struct fake_pll fake_pll_sys, fake_pll_usb;
struct fake_rosc fake_rosc;
struct fake_psm fake_psm;
struct fake_watchdog fake_watchdog;
struct fake_xosc fake_xosc;
static jmp_buf reset_jump;
static bool locked, early_reset, containment, xosc_starts;
static unsigned int lock_calls, shutdown_calls, show_calls, idle_calls, schedule_calls;
static unsigned int dormant_calls, trigger_calls;
static uint32_t tick_step;
static uint8_t shown[2][4];

struct fake_watchdog *fake_watchdog_get(void) {
    if (fake_watchdog.ctrl & WATCHDOG_CTRL_ENABLE_BITS) {
        uint32_t remaining = fake_watchdog.ctrl & WATCHDOG_CTRL_TIME_BITS;
        assert(remaining > tick_step);
        fake_watchdog.ctrl -= tick_step;
    }
    return &fake_watchdog;
}

struct fake_clocks *fake_clocks_get(void) {
    fake_clocks.clk[clk_ref].selected = BIT(fake_clocks.clk[clk_ref].ctrl & 3u);
    fake_clocks.clk[clk_sys].selected = BIT(fake_clocks.clk[clk_sys].ctrl & 1u);
    return &fake_clocks;
}

struct fake_rosc *fake_rosc_get(void) {
    if (containment) {
        bool enabled = (fake_rosc.ctrl & ROSC_CTRL_ENABLE_BITS) ==
                       (ROSC_CTRL_ENABLE_VALUE_ENABLE << ROSC_CTRL_ENABLE_LSB);
        fake_rosc.status = enabled ? ROSC_STATUS_STABLE_BITS : 0;
    }
    return &fake_rosc;
}

/* Everything DORMANT needs must be in place when the crystal stops. Reading
 * the crystal again stands for the key press that wakes it. */
struct fake_xosc *fake_xosc_get(void) {
    if (fake_xosc.dormant == XOSC_DORMANT_VALUE_DORMANT) {
        assert(containment && locked && shutdown_calls == 1);
        assert(!(fake_watchdog.ctrl & WATCHDOG_CTRL_ENABLE_BITS));
        for (unsigned int pin = 0; pin < 13; pin++) {
            assert(fake_io_bank0.io[pin].ctrl == 5);
            assert(fake_pads_bank0.io[pin] == (pin < 8 ? 0x4au : 0x10u));
        }
        assert(fake_pads_bank0.io[13] == 0x56u && fake_io_bank0.io[13].ctrl == 0xffffffffu);
        assert(fake_sio.gpio_oe_clr == 0xffu && fake_sio.gpio_oe_set == 0x1f00u);
        assert(fake_sio.gpio_clr == 0x1f00u);
        for (unsigned int i = 0; i < 4; i++) {
            assert(fake_io_bank0.intr[i] == UINT32_MAX);
            assert(fake_io_bank0.dormant_wake_irq_ctrl.inte[i] == (i == 0 ? 0x44444444u : 0));
            assert(fake_io_bank0.dormant_wake_irq_ctrl.intf[i] == 0);
        }
        assert(fake_clocks.clk[clk_ref].ctrl == 0x12345802u);
        assert(fake_clocks.clk[clk_sys].ctrl == 0x12345600u);
        for (unsigned int i = clk_usb; i <= clk_peri; i++) {
            assert(fake_clocks.clk[i].ctrl == 0x12345000u);
        }
        assert(fake_pll_sys.pwr == PLL_PWR_BITS && fake_pll_usb.pwr == PLL_PWR_BITS);
        assert((fake_rosc.ctrl & ROSC_CTRL_ENABLE_BITS) ==
               (ROSC_CTRL_ENABLE_VALUE_DISABLE << ROSC_CTRL_ENABLE_LSB));
        assert(fake_xosc.status & XOSC_STATUS_STABLE_BITS);
        dormant_calls++;
        fake_xosc.dormant = 0;
    }
    return &fake_xosc;
}

unsigned int irq_lock(void) {
    unsigned int previous = locked;
    locked = true;
    lock_calls++;
    return previous;
}

void irq_unlock(unsigned int key) { locked = key != 0; }

int k_work_schedule(struct k_work_delayable *work, int delay) {
    assert(work == &stable_boot_work && delay == 60000);
    schedule_calls++;
    return 1;
}

void hw_clear_bits(volatile uint32_t *address, uint32_t mask) {
    assert(locked);
    if (address == &fake_resets.reset) {
        assert(mask == (BIT(5) | BIT(8)));
        shutdown_calls++;
    } else if (address == &fake_watchdog.ctrl) {
        assert(containment && dormant_calls == 1 && mask == WATCHDOG_CTRL_ENABLE_BITS);
    } else {
        assert(containment && dormant_calls == 0);
        assert(address == &fake_clocks.clk[clk_sys].ctrl ? mask == CLOCKS_CLK_SYS_CTRL_SRC_BITS
                                                         : mask == 0x800u);
    }
    *address &= ~mask;
}

void hw_set_bits(volatile uint32_t *address, uint32_t mask) {
    assert(locked);
    if (address == &fake_xosc.ctrl) {
        assert(containment && fake_xosc.ctrl == XOSC_CTRL_FREQ_RANGE_VALUE_1_15MHZ);
        assert(fake_xosc.startup == 3008u && mask == 0xfab000u);
        *address |= mask;
        fake_xosc.status = XOSC_STATUS_ENABLED_BITS | (xosc_starts ? XOSC_STATUS_STABLE_BITS : 0);
        return;
    }
    if (mask == WATCHDOG_CTRL_TRIGGER_BITS) {
        assert(containment && address == &fake_watchdog.ctrl && dormant_calls == 1);
        assert((fake_rosc.ctrl & ROSC_CTRL_ENABLE_BITS) == 0xfab000u);
        assert(fake_rosc.status & ROSC_STATUS_STABLE_BITS);
        assert(fake_psm.wdsel == 0x1fffcu && fake_watchdog.scratch[4] == 0);
        trigger_calls++;
        longjmp(reset_jump, 3);
    }
    assert(address == &fake_watchdog.ctrl && mask == WATCHDOG_CTRL_ENABLE_BITS);
    assert(show_calls == 0 && shutdown_calls == 0);
    assert(fake_watchdog.ctrl == 0x07000000u);
    assert(fake_watchdog.load == 4000000u);
    assert(fake_watchdog.tick == (WATCHDOG_TICK_ENABLE_BITS | 12u));
    assert(fake_psm.wdsel == 0x1fffcu);
    assert(fake_watchdog.scratch[4] == 0);
    assert((fake_rosc.ctrl & ROSC_CTRL_ENABLE_BITS) == 0xfab000u);
    *address = mask | fake_watchdog.load;
}

void test_dsb(void) {
    assert(fake_systick.CTRL == 0 && fake_nvic.ICER[0] == UINT32_MAX &&
           fake_nvic.ICPR[0] == UINT32_MAX);
    assert(fake_scb.ICSR == (SCB_ICSR_PENDSTCLR_Msk | SCB_ICSR_PENDSVCLR_Msk));
}

void test_wfi(void) {
    test_dsb();
    assert(locked);
    idle_calls++;
    if (containment) {
        assert(!(fake_watchdog.ctrl & WATCHDOG_CTRL_ENABLE_BITS));
        assert(shutdown_calls == 1 && show_calls == 0 && dormant_calls == 0);
        assert(fake_io_bank0.dormant_wake_irq_ctrl.inte[0] == 0x44444444u);
    } else {
        assert(fake_watchdog.ctrl & WATCHDOG_CTRL_ENABLE_BITS);
        assert(show_calls == 2 && shutdown_calls == 2);
        assert((fake_watchdog.ctrl & WATCHDOG_CTRL_TIME_BITS) <=
               WATCHDOG_TIMEOUT_TICKS - 2 * INDICATION_TICKS);
    }
    longjmp(reset_jump, 1);
}

void defy_fatal_leds_show(uint8_t c0, uint8_t c1, uint8_t c2, uint8_t c3) {
    assert(locked && shutdown_calls == 1 && show_calls < 2);
    assert(fake_watchdog.ctrl & WATCHDOG_CTRL_ENABLE_BITS);
    assert((fault_state() & FAULT_PENDING) != 0);
    assert(fake_sio.gpio_clr == (BIT(14) | BIT(15)));
    assert(fake_sio.gpio_oe_set == (BIT(14) | BIT(15)));
    assert(fake_io_bank0.io[14].ctrl == 5 && fake_io_bank0.io[15].ctrl == 5);
    if (early_reset) {
        longjmp(reset_jump, 2);
    }
    shown[show_calls][0] = c0;
    shown[show_calls][1] = c1;
    shown[show_calls][2] = c2;
    shown[show_calls][3] = c3;
    show_calls++;
}

static void setup_test(void) {
    fake_systick.CTRL = UINT32_MAX;
    memset(&fake_nvic, 0, sizeof(fake_nvic));
    fake_scb.ICSR = 0;
    memset(&fake_resets, 0, sizeof(fake_resets));
    memset(&fake_io_bank0, 0xff, sizeof(fake_io_bank0));
    for (unsigned int i = 0; i < 30; i++) {
        fake_pads_bank0.io[i] = 0x56u;
    }
    memset(&fake_sio, 0, sizeof(fake_sio));
    fake_sio.gpio_in = 0xffffffffu;
    for (unsigned int i = 0; i < 6; i++) {
        fake_clocks.clk[i].ctrl = i == clk_sys ? 0x12345601u : 0x12345800u;
    }
    fake_pll_sys.pwr = fake_pll_usb.pwr = 0;
    memset(&fake_xosc, 0, sizeof(fake_xosc));
    fake_xosc.status = XOSC_STATUS_ENABLED_BITS | XOSC_STATUS_STABLE_BITS;
    memset(&fake_rosc, 0, sizeof(fake_rosc));
    memset(&fake_psm, 0, sizeof(fake_psm));
    memset(&fake_watchdog, 0, sizeof(fake_watchdog));
    fake_resets.reset = 0x12345678u;
    fake_resets.reset_done = 0xffffffffu;
    fake_rosc.ctrl = 0x12d1e345u;
    fake_rosc.status = ROSC_STATUS_STABLE_BITS;
    for (unsigned int i = 0; i < 8; i++) {
        fake_watchdog.scratch[i] = 0x12345600u + i;
    }
    locked = early_reset = containment = false;
    xosc_starts = true;
    lock_calls = shutdown_calls = show_calls = idle_calls = schedule_calls = 0;
    dormant_calls = trigger_calls = 0;
    tick_step = 1000;
    memset(shown, 0, sizeof(shown));
}

static void run_fault(unsigned int reason) {
    if (setjmp(reset_jump) == 0) {
        defy_fatal_recover(reason);
    }
}

static void boot_after_fault(void) {
    fake_watchdog.ctrl = 0;
    fake_watchdog.reason = WATCHDOG_REASON_TIMER_BITS;
    defy_fatal_recovery_boot();
}

static void test_recovery(unsigned int reason, unsigned int channel, bool irq_locked,
                          bool clocks_ready, uint32_t step) {
    setup_test();
    locked = irq_locked;
    tick_step = step;
    fake_resets.reset_done = clocks_ready ? 0xffffffffu : 0;
    fake_rosc.status = clocks_ready ? ROSC_STATUS_STABLE_BITS : 0;
    run_fault(reason);
    assert(lock_calls == 1 && shutdown_calls == 2 && show_calls == 2 && idle_calls == 1);
    const uint8_t first[] = {DEFY_FATAL_LEDS_LEVEL, 0, 0, 0};
    uint8_t second[] = {0, 0, 0, 0};
    second[channel] = DEFY_FATAL_LEDS_LEVEL;
    assert(memcmp(shown[0], first, sizeof(first)) == 0);
    assert(memcmp(shown[1], second, sizeof(second)) == 0);
    assert(fake_resets.reset == (0x12345678u & ~(BIT(5) | BIT(8))));
    assert((fake_rosc.ctrl & ~ROSC_CTRL_ENABLE_BITS) == (0x12d1e345u & ~ROSC_CTRL_ENABLE_BITS));
    assert(fault_state() == (FAULT_MAGIC | FAULT_PENDING | 1));
    for (unsigned int i = 2; i < 8; i++) {
        assert(fake_watchdog.scratch[i] == (i == 4 ? 0 : 0x12345600u + i));
    }
    for (unsigned int i = 0; i < 30; i++) {
        assert(fake_io_bank0.io[i].ctrl == (i == 14 || i == 15 ? 5 : 0xffffffffu));
    }
}

static void test_watchdog_can_reset_during_led_operation(void) {
    setup_test();
    early_reset = true;
    run_fault(K_ERR_CPU_EXCEPTION);
    assert(idle_calls == 0 && shutdown_calls == 1 && show_calls == 0);
    assert((fault_state() & FAULT_COUNT_MASK) == 1);
    boot_after_fault();
    assert((fault_state() & FAULT_PENDING) == 0);
    assert((fault_state() & FAULT_COUNT_MASK) == 1);
}

static void reach_fault_limit(void) {
    for (unsigned int count = 1; count <= FAULT_LIMIT; count++) {
        shutdown_calls = show_calls = 0;
        run_fault(K_ERR_STACK_CHK_FAIL);
        assert((fault_state() & FAULT_COUNT_MASK) == count);
        if (count < FAULT_LIMIT) {
            boot_after_fault();
            assert((fault_state() & FAULT_COUNT_MASK) == count);
            assert((fault_state() & FAULT_PENDING) == 0);
        }
    }
    fake_watchdog.ctrl = 0;
    fake_watchdog.reason = WATCHDOG_REASON_TIMER_BITS;
    containment = true;
    locked = false;
    shutdown_calls = show_calls = idle_calls = 0;
}

static void test_three_faults_sleep_until_key(bool xosc_running, uint32_t columns) {
    setup_test();
    reach_fault_limit();
    if (!xosc_running) {
        fake_xosc.status = 0;
        fake_xosc.ctrl = 0x00d1eaa0u;
    }
    fake_sio.gpio_in = columns;
    if (setjmp(reset_jump) == 0) {
        defy_fatal_recovery_boot();
        assert(false);
    }
    assert(dormant_calls == 1 && trigger_calls == 1 && idle_calls == 0);
    assert(fake_xosc.ctrl == (xosc_running ? 0 : 0x00fabaa0u));
    assert(fault_state() == (FAULT_MAGIC | FAULT_LIMIT));
    fake_watchdog.ctrl = 0;
    fake_watchdog.reason = WATCHDOG_REASON_FORCE_BITS;
    defy_fatal_recovery_boot();
    assert(fault_state() == 0);
}

static void test_dead_crystal_still_contains(void) {
    setup_test();
    reach_fault_limit();
    fake_xosc.status = 0;
    xosc_starts = false;
    if (setjmp(reset_jump) == 0) {
        defy_fatal_recovery_boot();
        assert(false);
    }
    assert(idle_calls == 1 && dormant_calls == 0 && trigger_calls == 0);
    assert(fake_pll_sys.pwr == 0 && fake_clocks.clk[clk_sys].ctrl == 0x12345601u);
}

static void test_fault_inside_handler_counts_once(void) {
    setup_test();
    run_fault(K_ERR_STACK_CHK_FAIL);
    shutdown_calls = show_calls = 0;
    run_fault(K_ERR_CPU_EXCEPTION);
    assert(fault_state() == (FAULT_MAGIC | FAULT_PENDING | 1));
}

static void test_normal_resets_and_invalid_markers_do_not_count(void) {
    const uint32_t states[] = {0, FAULT_MAGIC | FAULT_PENDING | 2,
                               FAULT_MAGIC | 2, 0x12345678u};
    for (unsigned int i = 0; i < sizeof(states) / sizeof(states[0]); i++) {
        for (unsigned int reason = 0; reason <= 3; reason++) {
            for (unsigned int valid = 0; valid < 2; valid++) {
                setup_test();
                fake_watchdog.scratch[0] = states[i];
                fake_watchdog.scratch[1] = valid ? ~states[i] : 123;
                fake_watchdog.reason = reason;
                defy_fatal_recovery_boot();
                bool accepted = i == 1 && valid && reason;
                assert((fault_state() & FAULT_COUNT_MASK) == (accepted ? 2 : 0));
                assert((fault_state() & FAULT_PENDING) == 0);
            }
        }
    }
}

static void test_stable_uptime_clears_only_consecutive_count(void) {
    setup_test();
    assert(init_level == APPLICATION && init_priority == 99);
    assert(start_stable_boot_timer() == 0 && schedule_calls == 0);
    run_fault(K_ERR_STACK_CHK_FAIL);
    boot_after_fault();
    assert(start_stable_boot_timer() == 0 && schedule_calls == 1);
    uint32_t state = fault_state();
    locked = false;
    stable_boot_work.handler(NULL);
    assert(!locked && fault_state() == (state & ~FAULT_COUNT_MASK));
    shutdown_calls = show_calls = 0;
    run_fault(K_ERR_CPU_EXCEPTION);
    assert((fault_state() & FAULT_COUNT_MASK) == 1);
}

static void test_stale_work_does_not_clear_pending_fatal(void) {
    setup_test();
    run_fault(K_ERR_CPU_EXCEPTION);
    uint32_t state = fault_state();
    stable_boot_work.handler(NULL);
    assert(fault_state() == state);
}

int main(void) {
    test_recovery(K_ERR_CPU_EXCEPTION, 2, false, true, 1000);
    test_recovery(K_ERR_STACK_CHK_FAIL, 3, true, true, 5000);
    test_recovery(4, 1, true, true, 50);
    test_recovery(999, 1, true, false, 1000);
    test_watchdog_can_reset_during_led_operation();
    test_three_faults_sleep_until_key(true, 0xffffffffu);
    test_three_faults_sleep_until_key(false, 0xffffffffu);
    test_three_faults_sleep_until_key(true, 0xfffffffeu);
    test_dead_crystal_still_contains();
    test_fault_inside_handler_counts_once();
    test_normal_resets_and_invalid_markers_do_not_count();
    test_stable_uptime_clears_only_consecutive_count();
    test_stale_work_does_not_clear_pending_fatal();
    return 0;
}
