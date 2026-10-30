/*
 * Restarts a half after a fatal error and holds it after repeated quick
 * faults; see CONFIG_DEFY_FATAL_RECOVERY.
 *
 * SPDX-License-Identifier: MIT
 */

#include <zephyr/devicetree.h>
#include <zephyr/dt-bindings/gpio/gpio.h>
#include <zephyr/dt-bindings/pinctrl/rpi-pico-rp2040-pinctrl.h>
#include <zephyr/fatal.h>
#include <zephyr/init.h>
#include <zephyr/kernel.h>
#include <cmsis_core.h>
#include <zephyr/irq.h>
#include <zephyr/sys/util.h>

#include <hardware/address_mapped.h>
#include <hardware/structs/io_bank0.h>
#include <hardware/structs/pads_bank0.h>
#include <hardware/structs/psm.h>
#include <hardware/structs/resets.h>
#include <hardware/structs/rosc.h>
#include <hardware/structs/sio.h>
#include <hardware/structs/watchdog.h>
#include <hardware/structs/xosc.h>

#include <dygma/defy_fatal_leds.h>
#include <dygma/defy_fatal_recovery.h>
#include <dygma/defy_rp2040_dormant.h>

#define PIN_SDB DT_GPIO_PIN(DT_NODELABEL(key_leds), sdb_gpios)
#define PIN_UNDERGLOW_POWER DT_GPIO_PIN(DT_NODELABEL(defy_leds), underglow_power_gpios)
#define SPIN_LIMIT 100000U
/* Erratum RP2040-E1: the watchdog counts down twice per 1 us tick, so these
 * are 2 s and 250 ms. */
#define WATCHDOG_TIMEOUT_TICKS 4000000U
#define INDICATION_TICKS 500000U
/* clk_ref, the crystal: 1 us watchdog ticks. */
#define REF_MHZ 12
#define WATCHDOG_PAUSE_BITS                                                                        \
    (WATCHDOG_CTRL_PAUSE_DBG0_BITS | WATCHDOG_CTRL_PAUSE_DBG1_BITS | WATCHDOG_CTRL_PAUSE_JTAG_BITS)
#define MATRIX_PINS (DEFY_MATRIX_COLUMNS | DEFY_MATRIX_ROWS)
#define COLUMN_PAD                                                                                 \
    (PADS_BANK0_GPIO0_IE_BITS | PADS_BANK0_GPIO0_PUE_BITS | PADS_BANK0_GPIO0_SCHMITT_BITS)
#define ROW_PAD (PADS_BANK0_GPIO0_DRIVE_VALUE_4MA << PADS_BANK0_GPIO0_DRIVE_LSB)
/* 64 times the Pico SDK's delay for a 12 MHz crystal, about 64 ms. */
#define XOSC_STARTUP_DELAY 3008U
#define XOSC_SPIN_LIMIT 10000000U
#define FAULT_MAGIC 0x44460000U
#define FAULT_MAGIC_MASK 0xffff0000U
#define FAULT_PENDING BIT(15)
#define FAULT_COUNT_MASK 0xffU
#define FAULT_LIMIT 3U
#define STABLE_UPTIME K_SECONDS(60)

BUILD_ASSERT(DT_SAME_NODE(DT_GPIO_CTLR(DT_NODELABEL(key_leds), sdb_gpios),
                          DT_NODELABEL(gpio0)));
BUILD_ASSERT(DT_SAME_NODE(DT_GPIO_CTLR(DT_NODELABEL(defy_leds), underglow_power_gpios),
                          DT_NODELABEL(gpio0)));
BUILD_ASSERT(DT_GPIO_FLAGS(DT_NODELABEL(key_leds), sdb_gpios) == GPIO_ACTIVE_HIGH);
BUILD_ASSERT(DT_GPIO_FLAGS(DT_NODELABEL(defy_leds), underglow_power_gpios) == GPIO_ACTIVE_HIGH);

static FUNC_NO_STACK_PROTECTOR uint32_t fault_state(void) {
    uint32_t state = watchdog_hw->scratch[DEFY_SCRATCH_FAULT];

    return (state & FAULT_MAGIC_MASK) == FAULT_MAGIC &&
                   watchdog_hw->scratch[DEFY_SCRATCH_FAULT_CHECK] == ~state
               ? state
               : 0;
}

static FUNC_NO_STACK_PROTECTOR void store_fault(uint32_t state) {
    watchdog_hw->scratch[DEFY_SCRATCH_FAULT_CHECK] = 0;
    watchdog_hw->scratch[DEFY_SCRATCH_FAULT] = state;
    watchdog_hw->scratch[DEFY_SCRATCH_FAULT_CHECK] = ~state;
}

static FUNC_NO_STACK_PROTECTOR void arm_watchdog(void) {
    rosc_hw->ctrl = (rosc_hw->ctrl & ~ROSC_CTRL_ENABLE_BITS) |
                    (ROSC_CTRL_ENABLE_VALUE_ENABLE << ROSC_CTRL_ENABLE_LSB);
    watchdog_hw->ctrl = WATCHDOG_PAUSE_BITS;
    watchdog_hw->scratch[DEFY_SCRATCH_BOOT_MAGIC] = 0;
    psm_hw->wdsel = PSM_WDSEL_BITS & ~(PSM_WDSEL_ROSC_BITS | PSM_WDSEL_XOSC_BITS);
    watchdog_hw->tick = WATCHDOG_TICK_ENABLE_BITS | REF_MHZ;
    watchdog_hw->load = WATCHDOG_TIMEOUT_TICKS;
    hw_set_bits(&watchdog_hw->ctrl, WATCHDOG_CTRL_ENABLE_BITS);
}

static FUNC_NO_STACK_PROTECTOR void wait_until(uint32_t remaining) {
    while ((watchdog_hw->ctrl & WATCHDOG_CTRL_TIME_BITS) > remaining) {
    }
}

static FUNC_NO_STACK_PROTECTOR void lights_off(void) {
    const uint32_t blocks = RESETS_RESET_IO_BANK0_BITS | RESETS_RESET_PADS_BANK0_BITS;
    const uint32_t pins = BIT(PIN_SDB) | BIT(PIN_UNDERGLOW_POWER);

    hw_clear_bits(&resets_hw->reset, blocks);
    for (unsigned int i = 0; i < SPIN_LIMIT && (~resets_hw->reset_done & blocks); i++) {
    }
    sio_hw->gpio_clr = pins;
    sio_hw->gpio_oe_set = pins;
    io_bank0_hw->io[PIN_SDB].ctrl = RP2_PINCTRL_GPIO_FUNC_SIO;
    io_bank0_hw->io[PIN_UNDERGLOW_POWER].ctrl = RP2_PINCTRL_GPIO_FUNC_SIO;
}

static FUNC_NO_STACK_PROTECTOR FUNC_NORETURN void idle(void) {
    SysTick->CTRL = 0;
    NVIC->ICER[0] = UINT32_MAX;
    NVIC->ICPR[0] = UINT32_MAX;
    SCB->ICSR = SCB_ICSR_PENDSTCLR_Msk | SCB_ICSR_PENDSVCLR_Msk;
    __DSB();
    for (;;) {
        __WFI();
    }
}

/* Lines driven low and inputs pulled up, so any key pulls its input low. At
 * the reset hook nothing has set the pins up yet. Only a new press wakes the
 * half: a key held down, perhaps the one that caused the faults, does not. */
static FUNC_NO_STACK_PROTECTOR void arm_key_wake(void) {
    sio_hw->gpio_oe_clr = DEFY_MATRIX_COLUMNS;
    sio_hw->gpio_clr = DEFY_MATRIX_ROWS;
    sio_hw->gpio_oe_set = DEFY_MATRIX_ROWS;
    for (unsigned int pin = 0; pin < ARRAY_SIZE(pads_bank0_hw->io); pin++) {
        if (MATRIX_PINS & BIT(pin)) {
            pads_bank0_hw->io[pin] = (DEFY_MATRIX_COLUMNS & BIT(pin)) ? COLUMN_PAD : ROW_PAD;
            io_bank0_hw->io[pin].ctrl = RP2_PINCTRL_GPIO_FUNC_SIO;
        }
    }
    for (unsigned int i = 0;
         i < SPIN_LIMIT && (sio_hw->gpio_in & DEFY_MATRIX_COLUMNS) != DEFY_MATRIX_COLUMNS; i++) {
    }
    for (unsigned int i = 0; i < ARRAY_SIZE(io_bank0_hw->intr); i++) {
        io_bank0_hw->intr[i] = UINT32_MAX;
    }
    defy_rp2040_dormant_wake_on(DEFY_MATRIX_EDGE_LOW);
}

/* Dygma's bootloader may leave the crystal off. */
static FUNC_NO_STACK_PROTECTOR bool start_xosc(void) {
    if (!(xosc_hw->status & XOSC_STATUS_ENABLED_BITS)) {
        xosc_hw->ctrl = XOSC_CTRL_FREQ_RANGE_VALUE_1_15MHZ;
        xosc_hw->startup = XOSC_STARTUP_DELAY;
        hw_set_bits(&xosc_hw->ctrl, XOSC_CTRL_ENABLE_VALUE_ENABLE << XOSC_CTRL_ENABLE_LSB);
    }
    for (unsigned int i = 0; i < XOSC_SPIN_LIMIT; i++) {
        if (xosc_hw->status & XOSC_STATUS_STABLE_BITS) {
            return true;
        }
    }
    return false;
}

/* DORMANT until a key edge, then a watchdog reset that starts the half from
 * Dygma's bootloader, so DEFY_SCRATCH_BOOT_MAGIC must not hold the boot ROM's magic. */
static FUNC_NO_STACK_PROTECTOR FUNC_NORETURN void sleep_until_key(void) {
    arm_key_wake();
    if (!start_xosc()) {
        idle();
    }
    defy_rp2040_run_from_xosc();
    defy_rp2040_dormant();
    watchdog_hw->scratch[DEFY_SCRATCH_BOOT_MAGIC] = 0;
    defy_rp2040_watchdog_reset();
}

FUNC_NO_STACK_PROTECTOR void defy_fatal_recovery_boot(void) {
    uint32_t state = fault_state();

    if (!(watchdog_hw->reason & (WATCHDOG_REASON_TIMER_BITS | WATCHDOG_REASON_FORCE_BITS)) ||
        !(state & FAULT_PENDING)) {
        store_fault(0);
        return;
    }
    store_fault(state & ~FAULT_PENDING);
    if ((state & FAULT_COUNT_MASK) >= FAULT_LIMIT) {
        /* The key wake boots without the pending flag, which clears the
         * count, as a power cycle does. */
        (void)irq_lock();
        lights_off();
        sleep_until_key();
    }
}

static void stable_boot(struct k_work *work) {
    ARG_UNUSED(work);
    unsigned int key = irq_lock();
    uint32_t state = fault_state();

    if (state != 0 && !(state & FAULT_PENDING)) {
        store_fault(state & ~FAULT_COUNT_MASK);
    }
    irq_unlock(key);
}

static K_WORK_DELAYABLE_DEFINE(stable_boot_work, stable_boot);

static int start_stable_boot_timer(void) {
    if ((fault_state() & FAULT_COUNT_MASK) != 0) {
        k_work_schedule(&stable_boot_work, STABLE_UPTIME);
    }
    return 0;
}

SYS_INIT(start_stable_boot_timer, APPLICATION, 99);

FUNC_NO_STACK_PROTECTOR FUNC_NORETURN void defy_fatal_recover(unsigned int reason) {
    (void)irq_lock();
    arm_watchdog();
    uint32_t state = fault_state();

    /* A fault inside this handler enters it again; count the first only. */
    if (!(state & FAULT_PENDING)) {
        uint32_t count = MIN((state & FAULT_COUNT_MASK) + 1, FAULT_LIMIT);

        store_fault(FAULT_MAGIC | FAULT_PENDING | count);
    }
    lights_off();
    defy_fatal_leds_show(DEFY_FATAL_LEDS_LEVEL, 0, 0, 0);
    wait_until(WATCHDOG_TIMEOUT_TICKS - INDICATION_TICKS);
    defy_fatal_leds_show_reason(reason);
    wait_until(WATCHDOG_TIMEOUT_TICKS - 2 * INDICATION_TICKS);
    lights_off();
    idle();
}
