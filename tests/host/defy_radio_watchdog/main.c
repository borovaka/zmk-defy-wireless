/* SPDX-License-Identifier: MIT */

#include <assert.h>
#include <setjmp.h>
#include <string.h>

#include "../../../apps/defy_radio/src/watchdog.c"

NRF_WDT_Type watchdog_registers;
unsigned int reload_count[NRF_WDT_CHANNEL_NUMBER];

static struct k_timer *started_timer;
static unsigned int timer_start_count;
static uint32_t uptime;
static unsigned int reset_count;
static jmp_buf reboot_jump;

/* Like the real one, it does not return: it ends the tick that called it. */
void sys_reboot(int type) {
    assert(type == SYS_REBOOT_COLD);
    assert(!watchdog_lock.locked);
    reset_count++;
    longjmp(reboot_jump, 1);
}

k_spinlock_key_t k_spin_lock(struct k_spinlock *lock) {
    assert(!lock->locked);
    lock->locked = true;
    return 0;
}

void k_spin_unlock(struct k_spinlock *lock, k_spinlock_key_t key) {
    assert(lock->locked && key == 0);
    lock->locked = false;
}

uint32_t k_uptime_get_32(void) {
    assert(watchdog_lock.locked);
    return uptime;
}

void k_timer_start(struct k_timer *timer, k_timeout_t duration, k_timeout_t period) {
    ARG_UNUSED(duration);
    ARG_UNUSED(period);
    started_timer = timer;
    timer_start_count++;
}

static void reset(void) {
    memset(&watchdog_registers, 0, sizeof(watchdog_registers));
    memset(reload_count, 0, sizeof(reload_count));
    started_timer = NULL;
    timer_start_count = 0;
    uptime = 0;
    memset(&watchdog_lock, 0, sizeof(watchdog_lock));
    watchdog_last_progress = 0;
    watchdog_timeout = 0;
    watchdog_expired = false;
    reset_count = 0;
}

static void start(uint32_t now) {
    reset();
    uptime = now;
    watchdog_registers.RUNSTATUS = 1;
    watchdog_registers.RREN = 1;
    watchdog_registers.CRV = 0x8000;
    watchdog_registers.CONFIG = 9;
    assert(test_init_watchdog_timer_start() == 0);
    assert(started_timer != NULL);
    assert(reload_count[0] == 1);
}

static void tick(void) {
    if (setjmp(reboot_jump) == 0) {
        started_timer->expiry_fn(started_timer);
    }
}

static void test_all_channel_masks_and_configuration_preserved(void) {
    for (unsigned int mask = 0; mask < 256; mask++) {
        reset();
        watchdog_registers.RUNSTATUS = 1;
        watchdog_registers.RREN = mask;
        watchdog_registers.REQSTATUS = mask ^ 0xff;
        watchdog_registers.CRV = 0x8000;
        watchdog_registers.CONFIG = 9;
        NRF_WDT_Type expected = watchdog_registers;

        defy_radio_watchdog_feed();

        for (unsigned int channel = 0; channel < 8; channel++) {
            bool enabled = (mask & (1U << channel)) != 0;

            expected.RR[channel] = enabled ? NRF_WDT_RR_VALUE : 0;
            assert(reload_count[channel] == (enabled ? 1U : 0U));
        }
        assert(memcmp(&expected, &watchdog_registers, sizeof(expected)) == 0);
        assert(timer_start_count == 0);
    }
}

static void test_late_first_heartbeat_cannot_extend_startup(void) {
    start(17);
    uptime += STARTUP_GRACE_MS;
    defy_radio_watchdog_heartbeat();
    assert(watchdog_expired);
    assert(reset_count == 0);
    tick();
    assert(reload_count[0] == 1);
    assert(reset_count == 1);
}

static void test_uptime_wrap_preserves_deadlines(void) {
    start(UINT32_MAX - 2500U);
    uptime += STARTUP_GRACE_MS - 1;
    tick();
    assert(!watchdog_expired && reload_count[0] == 2);
    assert(reset_count == 0);
    uptime++;
    tick();
    assert(watchdog_expired && reload_count[0] == 2);
    assert(reset_count == 1);

    start(UINT32_MAX - 500U);
    defy_radio_watchdog_heartbeat();
    uptime += LINK_TIMEOUT_MS - 1;
    tick();
    assert(!watchdog_expired && reload_count[0] == 2);
    assert(reset_count == 0);
    uptime++;
    tick();
    assert(watchdog_expired && reload_count[0] == 2);
    assert(reset_count == 1);
    uptime++;
    defy_radio_watchdog_heartbeat();
    tick();
    assert(watchdog_expired && reload_count[0] == 2);
    assert(reset_count == 2);
}

int main(void) {
    test_all_channel_masks_and_configuration_preserved();
    test_late_first_heartbeat_cannot_extend_startup();
    test_uptime_wrap_preserves_deadlines();
    return 0;
}
