/*
 * sys_poweroff() for the RP2040 halves: DORMANT with key wake, then a full
 * reset.
 *
 * Zephyr has no power-off support for the RP2040. This fills the gap for
 * CONFIG_DEFY_HALF_SLEEP the same way the stock Defy firmware's true sleep
 * works: stop every clock, wake on a key, reboot. The sequence itself is in
 * dygma/defy_rp2040_dormant.h. ZMK's own deep sleep and soft off are not
 * supported.
 *
 * SPDX-License-Identifier: MIT
 */

#ifndef CONFIG_DEFY_HALF_SLEEP
#error "sys_poweroff() on the halves serves CONFIG_DEFY_HALF_SLEEP alone"
#endif

#include <errno.h>

#include <zephyr/drivers/kscan.h>
#include <zephyr/init.h>
#include <zephyr/irq.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/poweroff.h>
#include <zephyr/sys/reboot.h>
#include <zephyr/sys/util.h>

#include <hardware/structs/io_bank0.h>
#include <hardware/structs/sio.h>
#include <hardware/structs/watchdog.h>

#include <dygma/defy_half_power.h>
#include <dygma/defy_rp2040_dormant.h>

#define MATRIX_IRQ_MASK 0xffffffffu

#define KEY_WAKE_MAGIC 0x44594b57u

static bool matrix_prepared;
static bool woke_from_key;

static int wake_reason_init(void) {
    woke_from_key = (watchdog_hw->reason & WATCHDOG_REASON_FORCE_BITS) != 0 &&
                    watchdog_hw->scratch[DEFY_SCRATCH_KEY_WAKE] == KEY_WAKE_MAGIC &&
                    watchdog_hw->scratch[DEFY_SCRATCH_KEY_WAKE_CHECK] == ~KEY_WAKE_MAGIC;
    watchdog_hw->scratch[DEFY_SCRATCH_KEY_WAKE] = 0;
    watchdog_hw->scratch[DEFY_SCRATCH_KEY_WAKE_CHECK] = 0;
    return 0;
}

SYS_INIT(wake_reason_init, PRE_KERNEL_1, 0);

bool defy_half_power_woke_from_key(void) { return woke_from_key; }

static bool matrix_pending(void) {
    return (io_bank0_hw->intr[0] & (DEFY_MATRIX_LEVEL_LOW | DEFY_MATRIX_EDGE_LOW)) != 0 ||
           (sio_hw->gpio_in & DEFY_MATRIX_COLUMNS) != DEFY_MATRIX_COLUMNS;
}

int defy_half_power_prepare(void) {
    __ASSERT(k_current_get() == &k_sys_work_q.thread, "system work queue only");

    unsigned int key = irq_lock();
    int err = 0;

    if (matrix_prepared) {
        err = -EALREADY;
    } else if (io_bank0_hw->proc0_irq_ctrl.inte[0] != DEFY_MATRIX_LEVEL_LOW ||
               (sio_hw->gpio_oe & DEFY_MATRIX_ROWS) != DEFY_MATRIX_ROWS ||
               (sio_hw->gpio_out & DEFY_MATRIX_ROWS) != 0 || matrix_pending()) {
        err = -EBUSY;
    } else {
        hw_clear_bits(&io_bank0_hw->proc0_irq_ctrl.inte[0], MATRIX_IRQ_MASK);
        matrix_prepared = true;
    }
    irq_unlock(key);
    return err;
}

bool defy_half_power_pending(void) {
    unsigned int key = irq_lock();
    bool pending = !matrix_prepared || matrix_pending();

    irq_unlock(key);
    return pending;
}

int defy_half_power_cancel(void) {
    __ASSERT(k_current_get() == &k_sys_work_q.thread, "system work queue only");

    unsigned int key = irq_lock();
    bool prepared = matrix_prepared;

    matrix_prepared = false;
    irq_unlock(key);
    return prepared ? kscan_enable_callback(DEVICE_DT_GET(DEFY_MATRIX_NODE)) : -EALREADY;
}

FUNC_NORETURN void defy_half_power_enter(void) {
    (void)irq_lock();
    if (!matrix_prepared || matrix_pending()) {
        sys_reboot(SYS_REBOOT_COLD);
    }

    defy_rp2040_dormant_wake_on(DEFY_MATRIX_LEVEL_LOW | DEFY_MATRIX_EDGE_LOW);
    defy_rp2040_run_from_xosc();
    defy_rp2040_dormant();
    watchdog_hw->scratch[DEFY_SCRATCH_KEY_WAKE] = KEY_WAKE_MAGIC;
    watchdog_hw->scratch[DEFY_SCRATCH_KEY_WAKE_CHECK] = ~KEY_WAKE_MAGIC;
    defy_rp2040_watchdog_reset();
}

FUNC_NORETURN void z_sys_poweroff(void) { defy_half_power_enter(); }
