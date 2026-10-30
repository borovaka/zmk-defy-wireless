/*
 * Power off to System OFF, once the RP2040 has let go of the SPI link. Chip
 * select going low wakes the radio again.
 *
 * SPDX-License-Identifier: MIT
 */

#include <errno.h>

#include <zephyr/drivers/gpio.h>
#include <zephyr/irq.h>
#include <zephyr/sys/poweroff.h>
#include <zephyr/sys/reboot.h>

#include <hal/nrf_spis.h>

#include "bridge.h"
#include "power_off.h"

/* The pin belongs to the SPIS, so gpio_pin_get_dt() would not apply its active-low flag. */
static int chip_select_active(const struct gpio_dt_spec *pin) {
    int level = gpio_pin_get_raw(pin->port, pin->pin);

    if (level < 0) {
        return level;
    }
    return (level != 0) ^ ((pin->dt_flags & GPIO_ACTIVE_LOW) != 0);
}

int defy_radio_power_off(void) {
    NRF_SPIS_Type *spis = (NRF_SPIS_Type *)DT_REG_ADDR(DEFY_RADIO_SPI_NODE);
    unsigned int key = irq_lock();
    int active = chip_select_active(&defy_radio_chip_select);

    if (active != 0) {
        irq_unlock(key);
        return active < 0 ? active : -EBUSY;
    }

    nrf_spis_task_trigger(spis, NRF_SPIS_TASK_ACQUIRE);
    if (nrf_spis_semaphore_status_get(spis) != NRF_SPIS_SEMSTAT_CPU) {
        irq_unlock(key);
        return -EBUSY;
    }

    active = chip_select_active(&defy_radio_chip_select);
    if (active != 0) {
        irq_unlock(key);
        return active < 0 ? active : -EBUSY;
    }

    nrf_spis_int_disable(spis, NRF_SPIS_ALL_INTS_MASK);
    nrf_spis_disable(spis);
    if (gpio_pin_configure_dt(&defy_radio_chip_select, GPIO_INPUT | GPIO_PULL_UP) < 0 ||
        gpio_pin_interrupt_configure_dt(&defy_radio_chip_select, GPIO_INT_LEVEL_ACTIVE) < 0) {
        sys_reboot(SYS_REBOOT_COLD);
    }
    sys_poweroff();
    CODE_UNREACHABLE;
}
