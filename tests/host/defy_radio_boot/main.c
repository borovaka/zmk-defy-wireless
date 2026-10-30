/* SPDX-License-Identifier: MIT */

#include <assert.h>
#include <setjmp.h>
#include <stdint.h>
#include <string.h>

#include "boot.h"
#include "boot_registers.h"

#define TIMEOUT_CYCLES (DEFY_RADIO_BOOT_QUIESCE_TIMEOUT_US * DEFY_BOOT_CORE_CYCLES_PER_US)
#define CYCLES_PER_MS (1000u * DEFY_BOOT_CORE_CYCLES_PER_US)

static struct {
    uint32_t enabled;
    uint32_t pin;
    uint32_t input;
    uint32_t semaphore;
    uint32_t systick_ctrl;
    uint32_t systick_load;
    uint32_t counter_origin;
    uint32_t elapsed;
    uint32_t step;
    uint32_t freeze_at;
    uint32_t cs_release_at;
    uint32_t acquire_at;
    unsigned int reads;
    unsigned int counter_reads;
    unsigned int counter_writes;
    unsigned int clock_writes;
    unsigned int acquire_count;
    unsigned int disable_count;
    unsigned int irq_disable_count;
    unsigned int watchdog_feeds;
    unsigned int system_init_count;
    unsigned int reset_count;
    unsigned int startup_step;
    bool first_counter_read;
    bool timed_cs;
    bool grant_acquire;
    bool cs_after_acquire;
    bool disable_fails;
    bool systick_pending;
    bool startup_running;
} hardware;
static jmp_buf reset_return;

void soc_reset_hook(void);

void __disable_irq(void) {
    assert(hardware.startup_step == 0);
    hardware.startup_step++;
}

void defy_radio_watchdog_feed(void) {
    if (hardware.startup_running) {
        assert(hardware.startup_step == 1 || hardware.startup_step == 2);
        hardware.startup_step = 2;
    }
    assert(hardware.elapsed < TIMEOUT_CYCLES);
    hardware.watchdog_feeds++;
}

static void assert_clock_stopped(void) {
    assert(hardware.systick_ctrl == 0);
    assert(hardware.systick_load == 0);
    assert(hardware.first_counter_read);
    assert(!hardware.systick_pending);
}

void __DSB(void) {
    assert(hardware.startup_step == 2);
    assert(hardware.enabled == 0);
    assert_clock_stopped();
    hardware.startup_step++;
}

void __ISB(void) {
    assert(hardware.startup_step == 3);
    hardware.startup_step++;
}

void SystemInit(void) {
    assert(hardware.startup_step == 4);
    hardware.system_init_count++;
}

__attribute__((noreturn)) void NVIC_SystemReset(void) {
    assert(hardware.startup_step == 2);
    assert(hardware.system_init_count == 0);
    assert_clock_stopped();
    hardware.reset_count++;
    longjmp(reset_return, 1);
}

static uint32_t gpio_input(void) {
    if (hardware.timed_cs) {
        return hardware.elapsed >= hardware.cs_release_at ? 1u << hardware.pin : 0;
    }
    return hardware.input;
}

uint32_t defy_radio_boot_read(uintptr_t address) {
    assert(++hardware.reads < 250000);
    if (hardware.startup_running) {
        assert(hardware.startup_step == 2);
    }
    switch (address) {
    case DEFY_BOOT_SPIS_ENABLE:
        return hardware.enabled;
    case DEFY_BOOT_SPIS_CSN:
        return hardware.pin;
    case DEFY_BOOT_GPIO_IN:
        return gpio_input();
    case DEFY_BOOT_SPIS_SEMSTAT:
        if (hardware.cs_after_acquire) {
            hardware.input = 0;
            hardware.timed_cs = false;
        }
        return hardware.semaphore;
    case DEFY_BOOT_SYSTICK_VALUE:
        assert(hardware.systick_ctrl == (DEFY_BOOT_SYSTICK_ENABLE | DEFY_BOOT_SYSTICK_CORE_CLOCK));
        if (!hardware.first_counter_read && hardware.elapsed < hardware.freeze_at) {
            uint32_t step = hardware.freeze_at - hardware.elapsed;
            hardware.elapsed += step < hardware.step ? step : hardware.step;
        }
        hardware.first_counter_read = false;
        hardware.counter_reads++;
        return (hardware.counter_origin - hardware.elapsed) & DEFY_BOOT_SYSTICK_MASK;
    default:
        assert(false);
        return 0;
    }
}

void defy_radio_boot_write(uintptr_t address, uint32_t value) {
    switch (address) {
    case DEFY_BOOT_SPIS_ACQUIRE:
        assert(value == 1);
        assert(gpio_input() & (1u << hardware.pin));
        hardware.acquire_count++;
        if (hardware.grant_acquire || hardware.elapsed >= hardware.acquire_at) {
            hardware.semaphore = DEFY_BOOT_SPIS_CPU_OWNER;
        }
        break;
    case DEFY_BOOT_SPIS_INTENCLR:
        assert(value == UINT32_MAX);
        assert(hardware.semaphore == DEFY_BOOT_SPIS_CPU_OWNER);
        assert(gpio_input() & (1u << hardware.pin));
        assert(hardware.elapsed < TIMEOUT_CYCLES);
        hardware.irq_disable_count++;
        break;
    case DEFY_BOOT_SPIS_ENABLE:
        assert(value == 0);
        assert(hardware.semaphore == DEFY_BOOT_SPIS_CPU_OWNER);
        assert(gpio_input() & (1u << hardware.pin));
        assert(hardware.irq_disable_count == 1);
        if (!hardware.disable_fails) {
            hardware.enabled = value;
        }
        hardware.disable_count++;
        break;
    case DEFY_BOOT_SYSTICK_CTRL:
        assert(value == 0 || value == (DEFY_BOOT_SYSTICK_ENABLE | DEFY_BOOT_SYSTICK_CORE_CLOCK));
        if (value != 0) {
            assert(hardware.systick_load == DEFY_BOOT_SYSTICK_MASK);
        }
        hardware.systick_ctrl = value;
        hardware.clock_writes++;
        break;
    case DEFY_BOOT_SYSTICK_LOAD:
        assert(hardware.systick_ctrl == 0);
        assert(value == 0 || value == DEFY_BOOT_SYSTICK_MASK);
        hardware.systick_load = value;
        hardware.clock_writes++;
        break;
    case DEFY_BOOT_SYSTICK_VALUE:
        assert(hardware.systick_ctrl == 0 && value == 0);
        hardware.first_counter_read = true;
        hardware.counter_writes++;
        hardware.clock_writes++;
        break;
    case DEFY_BOOT_ICSR:
        assert(value == DEFY_BOOT_CLEAR_PENDING_SYSTICK);
        assert(hardware.systick_ctrl == 0);
        hardware.systick_pending = false;
        hardware.clock_writes++;
        break;
    default:
        assert(false);
    }
}

static void reset(void) {
    memset(&hardware, 0, sizeof(hardware));
    hardware.enabled = DEFY_BOOT_SPIS_ENABLED;
    hardware.pin = 11;
    hardware.input = 1u << hardware.pin;
    hardware.step = DEFY_BOOT_CORE_CYCLES_PER_US;
    hardware.freeze_at = UINT32_MAX;
    hardware.cs_release_at = UINT32_MAX;
    hardware.acquire_at = UINT32_MAX;
    hardware.systick_pending = true;
}

static void assert_left_enabled(void) {
    assert(hardware.enabled == DEFY_BOOT_SPIS_ENABLED);
    assert(hardware.disable_count == 0);
    assert(hardware.irq_disable_count == 0);
}

static bool quiesce(void) {
    return defy_radio_boot_quiesce_spis(defy_radio_watchdog_feed);
}

static void test_disabled_or_invalid_peripheral(void) {
    reset();
    hardware.enabled = 0;
    assert(quiesce());
    assert(hardware.reads == 1);
    assert(hardware.acquire_count == 0 && hardware.disable_count == 0);
    assert(hardware.clock_writes == 0 && hardware.watchdog_feeds == 0);

    reset();
    hardware.enabled = 7;
    assert(!quiesce());
    assert(hardware.acquire_count == 0 && hardware.disable_count == 0);
    assert(hardware.clock_writes == 0);

    reset();
    hardware.pin = UINT32_MAX;
    assert(!quiesce());
    assert_left_enabled();
    assert(hardware.clock_writes == 0);
}

static void test_timeout_and_ownership(void) {
    reset();
    hardware.input = 0;
    assert(!quiesce());
    assert(hardware.elapsed == TIMEOUT_CYCLES);
    assert(hardware.acquire_count == 0);
    assert(hardware.watchdog_feeds == DEFY_RADIO_BOOT_QUIESCE_TIMEOUT_US / 1000u - 1u);
    assert_left_enabled();
    assert_clock_stopped();

    for (uint32_t owner = 0; owner <= 3; owner++) {
        if (owner == DEFY_BOOT_SPIS_CPU_OWNER) {
            continue;
        }
        reset();
        hardware.semaphore = owner;
        assert(!quiesce());
        assert(hardware.acquire_count > 0);
        assert(hardware.elapsed == TIMEOUT_CYCLES);
        assert_left_enabled();
        assert_clock_stopped();
    }

    reset();
    hardware.grant_acquire = true;
    hardware.cs_after_acquire = true;
    assert(!quiesce());
    assert(hardware.acquire_count == 1);
    assert(hardware.elapsed == TIMEOUT_CYCLES);
    assert_left_enabled();
    assert_clock_stopped();
}

static void test_delayed_cs_and_acquisition(void) {
    reset();
    hardware.timed_cs = true;
    hardware.cs_release_at = 2u * CYCLES_PER_MS;
    hardware.grant_acquire = true;
    assert(quiesce());
    assert(hardware.elapsed > hardware.cs_release_at);
    assert(hardware.acquire_count == 1 && hardware.disable_count == 1);
    assert(hardware.watchdog_feeds == 2);
    assert_clock_stopped();

    reset();
    hardware.acquire_at = 7u * CYCLES_PER_MS;
    assert(quiesce());
    assert(hardware.elapsed > hardware.acquire_at);
    assert(hardware.acquire_count > 1 && hardware.disable_count == 1);
    assert(hardware.watchdog_feeds == 7);
    assert_clock_stopped();
}

static void test_deadline_boundary(void) {
    for (unsigned int delay = 0; delay < 4; delay++) {
        reset();
        hardware.timed_cs = true;
        hardware.grant_acquire = true;
        hardware.cs_release_at = TIMEOUT_CYCLES - 2u * hardware.step + delay * hardware.step;
        assert(quiesce() == (delay == 0));
        if (delay == 0) {
            assert(hardware.elapsed == TIMEOUT_CYCLES - hardware.step);
            assert(hardware.disable_count == 1);
        } else {
            assert(hardware.elapsed == TIMEOUT_CYCLES);
            assert_left_enabled();
        }
        assert_clock_stopped();
    }

    reset();
    hardware.acquire_at = TIMEOUT_CYCLES - hardware.step;
    assert(!quiesce());
    assert(hardware.semaphore == DEFY_BOOT_SPIS_CPU_OWNER);
    assert(hardware.elapsed == TIMEOUT_CYCLES);
    assert_left_enabled();
    assert_clock_stopped();
}

static void test_counter_wrap_and_stall(void) {
    reset();
    hardware.counter_origin = 17;
    hardware.timed_cs = true;
    hardware.cs_release_at = 2u * CYCLES_PER_MS;
    hardware.grant_acquire = true;
    assert(quiesce());
    assert(hardware.elapsed > hardware.counter_origin);
    assert(hardware.elapsed > hardware.cs_release_at);
    assert_clock_stopped();

    const uint32_t freezes[] = {0, 3u * CYCLES_PER_MS};
    for (unsigned int i = 0; i < sizeof(freezes) / sizeof(freezes[0]); i++) {
        reset();
        hardware.input = 0;
        hardware.freeze_at = freezes[i];
        assert(!quiesce());
        assert(hardware.elapsed == freezes[i]);
        assert(hardware.counter_reads < freezes[i] / hardware.step + 16u);
        assert_left_enabled();
        assert_clock_stopped();
    }
}

static void test_immediate_handoff_and_disable_failure(void) {
    reset();
    hardware.systick_ctrl = 7;
    hardware.systick_load = 1234;
    hardware.grant_acquire = true;
    assert(defy_radio_boot_quiesce_spis(NULL));
    assert(hardware.acquire_count == 1 && hardware.disable_count == 1);
    assert(hardware.enabled == 0);
    assert(hardware.watchdog_feeds == 0);
    assert_clock_stopped();

    reset();
    hardware.semaphore = DEFY_BOOT_SPIS_CPU_OWNER;
    assert(quiesce());
    assert(hardware.disable_count == 1);
    assert_clock_stopped();

    reset();
    hardware.grant_acquire = true;
    hardware.disable_fails = true;
    assert(!quiesce());
    assert(hardware.disable_count == 1 && hardware.enabled == DEFY_BOOT_SPIS_ENABLED);
    assert_clock_stopped();
}

static void test_startup_order(void) {
    reset();
    hardware.timed_cs = true;
    hardware.cs_release_at = 2u * CYCLES_PER_MS;
    hardware.grant_acquire = true;
    hardware.startup_running = true;
    soc_reset_hook();
    assert(hardware.system_init_count == 1);
    assert(hardware.reset_count == 0);
    assert(hardware.disable_count == 1);
    assert(hardware.watchdog_feeds == 3);

    reset();
    hardware.startup_running = true;
    hardware.input = 0;
    if (setjmp(reset_return) == 0) {
        soc_reset_hook();
        assert(false);
    }
    assert(hardware.reset_count == 1);
    assert(hardware.system_init_count == 0);
    assert(hardware.elapsed == TIMEOUT_CYCLES);
    assert_left_enabled();
}

int main(void) {
    test_disabled_or_invalid_peripheral();
    test_timeout_and_ownership();
    test_delayed_cs_and_acquisition();
    test_deadline_boundary();
    test_counter_wrap_and_stall();
    test_immediate_handoff_and_disable_failure();
    test_startup_order();
    return 0;
}
