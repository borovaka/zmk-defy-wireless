/* SPDX-License-Identifier: MIT */

#include <setjmp.h>
#include <string.h>

#include "../../../apps/defy_radio/src/power_off.c"

struct device fake_gpio;
NRF_SPIS_Type fake_spis;
const struct gpio_dt_spec defy_radio_chip_select = {
    .port = &fake_gpio, .pin = 17, .dt_flags = GPIO_ACTIVE_LOW};

/* How defy_radio_power_off() left: longjmp() from sys_poweroff() or sys_reboot(). */
enum exit { OFF = 1, REBOOT };

static struct {
    bool irq_locked;
    bool interrupts_enabled;
    nrf_spis_semstat_t owner;
    int levels[2];
    size_t reads;
    int configure_result;
    int wake_result;
    bool wake_armed;
    jmp_buf exit;
} hardware;

unsigned int irq_lock(void) {
    assert(!hardware.irq_locked);
    hardware.irq_locked = true;
    return 42;
}

void irq_unlock(unsigned int key) {
    assert(key == 42 && hardware.irq_locked);
    hardware.irq_locked = false;
}

int gpio_pin_get_raw(const struct device *port, unsigned int pin) {
    assert(port == &fake_gpio && pin == 17);
    assert(hardware.irq_locked && hardware.reads < 2);
    return hardware.levels[hardware.reads++];
}

int gpio_pin_get_dt(const struct gpio_dt_spec *pin) {
    int level = gpio_pin_get_raw(pin->port, pin->pin);

    return level < 0 ? level : level ^ fake_gpio.invert;
}

int gpio_pin_configure_dt(const struct gpio_dt_spec *pin, unsigned int flags) {
    assert(pin->port == &fake_gpio && pin->pin == 17);
    assert(flags == (GPIO_INPUT | GPIO_PULL_UP));
    assert(hardware.irq_locked && !fake_spis.enabled && !hardware.interrupts_enabled);
    fake_gpio.invert = (pin->dt_flags & GPIO_ACTIVE_LOW) != 0;
    return hardware.configure_result;
}

int gpio_pin_interrupt_configure_dt(const struct gpio_dt_spec *pin, unsigned int flags) {
    assert(pin->port == &fake_gpio && pin->pin == 17);
    assert(flags == GPIO_INT_LEVEL_ACTIVE && fake_gpio.invert);
    assert(hardware.irq_locked && !fake_spis.enabled && !hardware.interrupts_enabled);
    hardware.wake_armed = hardware.wake_result == 0;
    return hardware.wake_result;
}

void nrf_spis_task_trigger(NRF_SPIS_Type *spis, unsigned int task) {
    assert(spis == &fake_spis && task == NRF_SPIS_TASK_ACQUIRE);
    assert(hardware.irq_locked && fake_spis.enabled);
    if (hardware.owner == NRF_SPIS_SEMSTAT_FREE) {
        hardware.owner = NRF_SPIS_SEMSTAT_CPU;
    } else if (hardware.owner == NRF_SPIS_SEMSTAT_SPIS) {
        hardware.owner = NRF_SPIS_SEMSTAT_CPUPENDING;
    }
}

nrf_spis_semstat_t nrf_spis_semaphore_status_get(NRF_SPIS_Type *spis) {
    assert(spis == &fake_spis && hardware.irq_locked);
    return hardware.owner;
}

void nrf_spis_int_disable(NRF_SPIS_Type *spis, unsigned int mask) {
    assert(spis == &fake_spis && mask == NRF_SPIS_ALL_INTS_MASK);
    assert(hardware.irq_locked && hardware.owner == NRF_SPIS_SEMSTAT_CPU);
    hardware.interrupts_enabled = false;
}

void nrf_spis_disable(NRF_SPIS_Type *spis) {
    assert(spis == &fake_spis && hardware.irq_locked && !hardware.interrupts_enabled);
    assert(hardware.owner == NRF_SPIS_SEMSTAT_CPU);
    fake_spis.enabled = false;
}

_Noreturn void sys_reboot(int type) {
    assert(type == SYS_REBOOT_COLD && hardware.irq_locked);
    assert(!fake_spis.enabled && !hardware.interrupts_enabled);
    longjmp(hardware.exit, REBOOT);
}

_Noreturn void sys_poweroff(void) {
    assert(hardware.irq_locked && !fake_spis.enabled && !hardware.interrupts_enabled);
    /* Without a wake source the radio would stay off. */
    assert(hardware.wake_armed);
    longjmp(hardware.exit, OFF);
}

static void reset(void) {
    memset(&hardware, 0, sizeof(hardware));
    fake_gpio.invert = false;
    fake_spis.enabled = true;
    hardware.interrupts_enabled = true;
    hardware.owner = NRF_SPIS_SEMSTAT_FREE;
    hardware.levels[0] = 1;
    hardware.levels[1] = 1;
}

static void test_active_or_unreadable_cs(void) {
    const int levels[] = {0, -EIO};

    for (size_t i = 0; i < sizeof(levels) / sizeof(levels[0]); i++) {
        reset();
        hardware.levels[0] = levels[i];
        assert(defy_radio_power_off() == (levels[i] < 0 ? levels[i] : -EBUSY));
        assert(fake_spis.enabled && hardware.interrupts_enabled && !hardware.irq_locked);
        assert(hardware.owner == NRF_SPIS_SEMSTAT_FREE);
    }
}

static void test_dma_ownership_is_not_disabled(void) {
    const nrf_spis_semstat_t owners[] = {NRF_SPIS_SEMSTAT_SPIS, NRF_SPIS_SEMSTAT_CPUPENDING};

    for (size_t i = 0; i < sizeof(owners) / sizeof(owners[0]); i++) {
        reset();
        hardware.owner = owners[i];
        assert(defy_radio_power_off() == -EBUSY);
        assert(fake_spis.enabled && hardware.interrupts_enabled && !hardware.irq_locked);
        assert(hardware.owner == NRF_SPIS_SEMSTAT_CPUPENDING);
    }
}

static void test_cs_changes_while_acquiring(void) {
    const int levels[] = {0, -EIO};

    for (size_t i = 0; i < sizeof(levels) / sizeof(levels[0]); i++) {
        reset();
        hardware.levels[1] = levels[i];
        assert(defy_radio_power_off() == (levels[i] < 0 ? levels[i] : -EBUSY));
        assert(fake_spis.enabled && hardware.interrupts_enabled && !hardware.irq_locked);
        assert(hardware.owner == NRF_SPIS_SEMSTAT_CPU && hardware.reads == 2);
    }
}

static void test_idle_cs_without_prior_gpio_configuration(void) {
    const nrf_spis_semstat_t owners[] = {NRF_SPIS_SEMSTAT_FREE, NRF_SPIS_SEMSTAT_CPU};

    for (size_t i = 0; i < sizeof(owners) / sizeof(owners[0]); i++) {
        reset();
        hardware.owner = owners[i];
        assert(!fake_gpio.invert);
        int exit = setjmp(hardware.exit);

        if (exit == 0) {
            (void)defy_radio_power_off();
            assert(false);
        }
        assert(exit == OFF && hardware.reads == 2);
    }
}

static void test_configuration_errors_reboot_after_quiescence(void) {
    for (int stage = 0; stage < 2; stage++) {
        reset();
        hardware.configure_result = stage == 0 ? -EIO : 0;
        hardware.wake_result = stage == 1 ? -EIO : 0;
        int exit = setjmp(hardware.exit);

        if (exit == 0) {
            (void)defy_radio_power_off();
            assert(false);
        }
        assert(exit == REBOOT && !hardware.wake_armed);
    }
}

int main(void) {
    test_active_or_unreadable_cs();
    test_dma_ownership_is_not_disabled();
    test_cs_changes_while_acquiring();
    test_idle_cs_without_prior_gpio_configuration();
    test_configuration_errors_reboot_after_quiescence();
    return 0;
}
