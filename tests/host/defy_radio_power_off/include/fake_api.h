/* SPDX-License-Identifier: MIT */

#pragma once

#include <assert.h>
#include <stdbool.h>
#include <stdint.h>

struct device { bool invert; };
struct gpio_dt_spec {
    const struct device *port;
    unsigned int pin;
    unsigned int dt_flags;
};

typedef struct { bool enabled; } NRF_SPIS_Type;
typedef enum {
    NRF_SPIS_SEMSTAT_FREE,
    NRF_SPIS_SEMSTAT_CPU,
    NRF_SPIS_SEMSTAT_SPIS,
    NRF_SPIS_SEMSTAT_CPUPENDING,
} nrf_spis_semstat_t;

extern struct device fake_gpio;
extern NRF_SPIS_Type fake_spis;

#define DT_REG_ADDR(node) ((uintptr_t)&fake_spis)
#define GPIO_ACTIVE_LOW 1U
#define GPIO_INPUT 2U
#define GPIO_PULL_UP 4U
#define GPIO_INT_LEVEL_ACTIVE 8U
#define NRF_SPIS_TASK_ACQUIRE 1U
#define NRF_SPIS_ALL_INTS_MASK 3U
#define SYS_REBOOT_COLD 0
#define CODE_UNREACHABLE __builtin_unreachable()

unsigned int irq_lock(void);
void irq_unlock(unsigned int key);
int gpio_pin_get_raw(const struct device *port, unsigned int pin);
int gpio_pin_get_dt(const struct gpio_dt_spec *pin);
int gpio_pin_configure_dt(const struct gpio_dt_spec *pin, unsigned int flags);
int gpio_pin_interrupt_configure_dt(const struct gpio_dt_spec *pin, unsigned int flags);
void nrf_spis_task_trigger(NRF_SPIS_Type *spis, unsigned int task);
nrf_spis_semstat_t nrf_spis_semaphore_status_get(NRF_SPIS_Type *spis);
void nrf_spis_int_disable(NRF_SPIS_Type *spis, unsigned int mask);
void nrf_spis_disable(NRF_SPIS_Type *spis);
_Noreturn void sys_reboot(int type);
_Noreturn void sys_poweroff(void);
