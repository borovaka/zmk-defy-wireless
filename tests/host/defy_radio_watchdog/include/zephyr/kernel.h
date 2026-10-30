/* SPDX-License-Identifier: MIT */

#pragma once

#include <stddef.h>
#include <stdbool.h>
#include <stdint.h>

typedef unsigned int k_timeout_t;
typedef unsigned int k_spinlock_key_t;

struct k_spinlock {
    bool locked;
};

struct k_timer {
    void (*expiry_fn)(struct k_timer *timer);
    void (*stop_fn)(struct k_timer *timer);
};

#define ARG_UNUSED(value) ((void)(value))
#define K_MSEC(value) (value)
#define K_TIMER_DEFINE(name, expiry, stop) struct k_timer name = {expiry, stop}

void k_timer_start(struct k_timer *timer, k_timeout_t duration, k_timeout_t period);
k_spinlock_key_t k_spin_lock(struct k_spinlock *lock);
void k_spin_unlock(struct k_spinlock *lock, k_spinlock_key_t key);
uint32_t k_uptime_get_32(void);
