/*
 * The RP2040's HCI SPI driver against a fake nRF52820 running the radio app's
 * link code. Each test starts the driver as a restarted RP2040 would, with
 * the radio in a given state. The defy_hci_spi_after_a_minute suite runs
 * last, after the first minute, when a radio that stops answering reboots the
 * RP2040.
 *
 * SPDX-License-Identifier: MIT
 */

#include <string.h>
#include <setjmp.h>

#include <zephyr/bluetooth/buf.h>
#include <zephyr/bluetooth/hci.h>
#include <zephyr/drivers/bluetooth.h>
#include <zephyr/kernel.h>
#include <zephyr/pm/device.h>
#include <zephyr/sys/reboot.h>
#include <zephyr/ztest.h>

#include <dygma/defy_hci_spi.h>
#include <dygma/defy_hci_diagnostics.h>

#include "fake_radio.h"

/* The driver's limits, from defy_hci_spi.c. */
#define START_ATTEMPTS 3
#define BOOT_TIMEOUT_MS 2000
#define ANSWER_TIMEOUT_MS 20
#define POWER_OFF_REPLY_TIMEOUT_MS DEFY_HCI_SPI_POWER_OFF_HOST_TIMEOUT_MS

#define MAX_RECEIVED 8

static const struct device *const hci = DEVICE_DT_GET(DT_NODELABEL(radio));

static struct {
    uint8_t data[MAX_RECEIVED][DEFY_HCI_SPI_MAX_MSG_LEN];
    size_t len[MAX_RECEIVED];
    enum bt_buf_type type[MAX_RECEIVED];
    int count;
} received;

static K_SEM_DEFINE(received_sem, 0, MAX_RECEIVED);
static jmp_buf reboot_env;
/* The thread that set reboot_env. */
static k_tid_t reboot_jumper;
static atomic_t reboot_expected;
static int reboot_kind;
static K_SEM_DEFINE(rebooted, 0, 1);

/*
 * A reboot on the thread that set reboot_env jumps back there. One on the RX
 * thread, which reboots when the radio stops answering, parks it until
 * before() aborts it.
 */
FUNC_NORETURN void __wrap_sys_reboot(int type) {
    zassert_true(atomic_get(&reboot_expected));
    reboot_kind = type;
    if (k_current_get() == reboot_jumper) {
        longjmp(reboot_env, 1);
    }
    k_sem_give(&rebooted);
    for (;;) {
        k_sleep(K_FOREVER);
    }
}

static int recv(const struct device *dev, struct net_buf *buf) {
    ARG_UNUSED(dev);

    if (received.count < MAX_RECEIVED) {
        memcpy(received.data[received.count], buf->data, buf->len);
        received.len[received.count] = buf->len;
        received.type[received.count] = bt_buf_get_type(buf);
        received.count++;
    }
    net_buf_unref(buf);
    k_sem_give(&received_sem);
    return 0;
}

static void wait_received(int count) {
    while (received.count < count) {
        zassert_ok(k_sem_take(&received_sem, K_MSEC(200)), "received %d of %d", received.count,
                   count);
    }
}

static int send_command(uint16_t opcode) {
    struct net_buf *buf = bt_hci_cmd_create(opcode, 0);
    int err;

    zassert_not_null(buf);
    err = bt_hci_send(hci, buf);
    if (err < 0) {
        net_buf_unref(buf);
    }
    return err;
}

static void before(void *fixture) {
    ARG_UNUSED(fixture);

    /*
     * The driver back to before its open, with the device's power state
     * active. The reset aborts an RX thread parked in __wrap_sys_reboot().
     */
    defy_hci_spi_test_reset();
    (void)pm_device_action_run(hci, PM_DEVICE_ACTION_RESUME);
    fake_radio_reset(FAKE_RADIO_RUNNING);
    memset(&received, 0, sizeof(received));
    k_sem_reset(&received_sem);
    atomic_set(&reboot_expected, false);
    reboot_jumper = NULL;
    k_sem_reset(&rebooted);
}

/* Both chips power up together: the radio is still booting when the RP2040 starts. */
ZTEST(defy_hci_spi, test_cold_boot) {
    fake_radio_reset(FAKE_RADIO_BOOTING);

    zassert_ok(bt_hci_open(hci, recv));
    zassert_equal(fake_radio_counts().reboot_commands, 0);
    zassert_equal(fake_radio_counts().boots, 1);
}

/*
 * The radio booted first, so data ready was high before the driver watched it:
 * no edge. A message after the ready event keeps it high, with no new edge.
 */
ZTEST(defy_hci_spi, test_radio_booted_first_with_message) {
    static const uint8_t event[] = {BT_HCI_H4_EVT, BT_HCI_EVT_VENDOR, 1, 0x42};

    fake_radio_reset(FAKE_RADIO_BOOTED);
    fake_radio_send(event, sizeof(event));

    zassert_ok(bt_hci_open(hci, recv));
    zassert_equal(fake_radio_counts().reboot_commands, 0);
    wait_received(1);
    zassert_mem_equal(received.data[0], &event[1], sizeof(event) - 1);
}

/*
 * The RP2040 restarted alone, as after &sys_reset or waking from deep sleep.
 * Messages from before the restart are read out and dropped.
 */
ZTEST(defy_hci_spi, test_rp2040_restart_with_messages) {
    static const uint8_t event[] = {BT_HCI_H4_EVT, BT_HCI_EVT_VENDOR, 1, 0x42};

    for (int i = 0; i < 3; i++) {
        fake_radio_send(event, sizeof(event));
    }

    zassert_ok(bt_hci_open(hci, recv));
    zassert_equal(fake_radio_counts().reboot_commands, 1);
    k_sleep(K_MSEC(50));
    zassert_equal(received.count, 0);
}

/* The RP2040 restarted between a header and the message after it. */
ZTEST(defy_hci_spi, test_rp2040_restart_mid_read) {
    static const uint8_t event[] = {BT_HCI_H4_EVT, BT_HCI_EVT_VENDOR, 1, 0x42};

    fake_radio_send(event, sizeof(event));
    fake_radio_start_read();

    zassert_ok(bt_hci_open(hci, recv));
    zassert_equal(fake_radio_counts().reboot_commands, 1);
}

/* The radio is in System OFF, after deep sleep. */
ZTEST(defy_hci_spi, test_radio_asleep) {
    fake_radio_reset(FAKE_RADIO_OFF);

    zassert_ok(bt_hci_open(hci, recv));
    zassert_true(fake_radio_counts().wakes >= 1);
    zassert_equal(fake_radio_counts().reboot_commands, 0);
}

/* The RP2040 restarted while the radio was powering off. */
ZTEST(defy_hci_spi, test_radio_powering_off) {
    fake_radio_power_off_pending();

    zassert_ok(bt_hci_open(hci, recv));
    zassert_true(fake_radio_counts().wakes >= 1);
    zassert_equal(fake_radio_state(), FAKE_RADIO_BOOTED);
}

/* A slow boot, as with the 32.768 kHz crystal starting. */
ZTEST(defy_hci_spi, test_slow_boot) {
    fake_radio_reset(FAKE_RADIO_BOOTING);
    fake_radio_set_boot_ms(BOOT_TIMEOUT_MS / 2);

    zassert_ok(bt_hci_open(hci, recv));
}

/* No radio: the start gives up, so ZMK can start without Bluetooth. */
ZTEST(defy_hci_spi, test_dead_radio) {
    int64_t start;
    int64_t took;

    fake_radio_reset(FAKE_RADIO_SILENT);

    start = k_uptime_get();
    zassert_equal(bt_hci_open(hci, recv), -EIO);
    took = k_uptime_get() - start;

    zassert_true(took <= START_ATTEMPTS * (BOOT_TIMEOUT_MS + 100), "took %lld ms", took);
    zassert_equal(send_command(BT_HCI_OP_RESET), -EIO);
    /* A failed open is not retried. */
    zassert_equal(bt_hci_open(hci, recv), -EALREADY);
}

ZTEST(defy_hci_spi, test_radio_ignores_reboot) {
    fake_radio_ignore_reboot(true);

    zassert_equal(bt_hci_open(hci, recv), -EIO);
    zassert_equal(fake_radio_counts().reboot_commands, START_ATTEMPTS);
}

/* Messages queued together keep data ready high, with one edge for all of them. */
ZTEST(defy_hci_spi, test_events_reach_host) {
    uint8_t event[] = {BT_HCI_H4_EVT, BT_HCI_EVT_VENDOR, 1, 0};

    zassert_ok(bt_hci_open(hci, recv));

    for (int i = 0; i < 3; i++) {
        event[3] = i;
        fake_radio_send(event, sizeof(event));
    }

    wait_received(3);
    for (int i = 0; i < 3; i++) {
        zassert_equal(received.type[i], BT_BUF_EVT);
        zassert_equal(received.len[i], 3);
        zassert_equal(received.data[i][0], BT_HCI_EVT_VENDOR);
        zassert_equal(received.data[i][2], i);
    }
}

ZTEST(defy_hci_spi, test_acl_reaches_host) {
    static const uint8_t acl[] = {BT_HCI_H4_ACL, 0x01, 0x00, 3, 0, 0xaa, 0xbb, 0xcc};

    zassert_ok(bt_hci_open(hci, recv));
    fake_radio_send(acl, sizeof(acl));

    wait_received(1);
    zassert_equal(received.type[0], BT_BUF_ACL_IN);
    zassert_mem_equal(received.data[0], &acl[1], sizeof(acl) - 1);
}

ZTEST(defy_hci_spi, test_command) {
    zassert_ok(bt_hci_open(hci, recv));
    zassert_ok(send_command(BT_HCI_OP_RESET));

    wait_received(1);
    zassert_equal(fake_radio_counts().last_opcode, BT_HCI_OP_RESET);
    zassert_equal(received.data[0][0], BT_HCI_EVT_CMD_COMPLETE);
    zassert_equal(sys_get_le16(&received.data[0][3]), BT_HCI_OP_RESET);
    struct defy_hci_diagnostics diagnostic;
    defy_hci_diagnostics_get(&diagnostic);
    zassert_equal(diagnostic.tx_opcode, BT_HCI_OP_RESET);
    zassert_equal(diagnostic.rx_opcode, BT_HCI_OP_RESET);
}

/* The radio's reply never reaches the host, which did not send the command. */
ZTEST(defy_hci_spi, test_battery_refresh_reply_is_dropped) {
    static const uint8_t vendor[] = {BT_HCI_H4_EVT, BT_HCI_EVT_VENDOR, 1, 0x42};

    zassert_ok(bt_hci_open(hci, recv));
    zassert_ok(defy_hci_spi_battery_refresh());
    zassert_equal(fake_radio_counts().refresh_commands, 1);
    zassert_equal(fake_radio_counts().commands, 0);
    fake_radio_send(vendor, sizeof(vendor));
    wait_received(1);
    zassert_equal(received.data[0][0], BT_HCI_EVT_VENDOR);
    struct defy_hci_diagnostics diagnostic;
    defy_hci_diagnostics_get(&diagnostic);
    zassert_equal(diagnostic.tx_opcode, DEFY_HCI_SPI_OP_BATTERY_REFRESH);
    zassert_equal(diagnostic.rx_opcode, DEFY_HCI_SPI_OP_BATTERY_REFRESH);
}

/* A radio from before the command answers with Command Status. */
ZTEST(defy_hci_spi, test_battery_refresh_status_from_old_radio_is_dropped) {
    static const uint8_t status[] = {BT_HCI_H4_EVT, BT_HCI_EVT_CMD_STATUS, 4,
                                     BT_HCI_ERR_UNKNOWN_CMD, 1,
                                     DEFY_HCI_SPI_OP_BATTERY_REFRESH & 0xff,
                                     DEFY_HCI_SPI_OP_BATTERY_REFRESH >> 8};
    static const uint8_t vendor[] = {BT_HCI_H4_EVT, BT_HCI_EVT_VENDOR, 1, 0x42};

    zassert_ok(bt_hci_open(hci, recv));
    fake_radio_send(status, sizeof(status));
    fake_radio_send(vendor, sizeof(vendor));
    wait_received(1);
    k_sleep(K_MSEC(50));
    zassert_equal(received.count, 1);
    zassert_equal(received.data[0][0], BT_HCI_EVT_VENDOR);
}

ZTEST(defy_hci_spi, test_battery_refresh_needs_open_link) {
    zassert_equal(defy_hci_spi_battery_refresh(), -EAGAIN);
    zassert_ok(bt_hci_open(hci, recv));
    zassert_ok(pm_device_action_run(hci, PM_DEVICE_ACTION_SUSPEND));
    zassert_equal(defy_hci_spi_battery_refresh(), -EAGAIN);
    zassert_equal(fake_radio_counts().refresh_commands, 0);
}

/* A radio slow to arm makes the driver retry, not fail. */
ZTEST(defy_hci_spi, test_unarmed_retries) {
    zassert_ok(bt_hci_open(hci, recv));
    fake_radio_miss_transfers(3);

    zassert_ok(send_command(BT_HCI_OP_RESET));
    wait_received(1);
}

/* The host opens the link once: a second open is refused, whatever became of the first. */
ZTEST(defy_hci_spi, test_open_is_one_shot) {
    static const uint8_t event[] = {BT_HCI_H4_EVT, BT_HCI_EVT_VENDOR, 1, 0x42};

    zassert_ok(bt_hci_open(hci, recv));
    zassert_equal(bt_hci_open(hci, recv), -EALREADY);
    zassert_equal(fake_radio_counts().reboot_commands, 1);
    fake_radio_send(event, sizeof(event));
    wait_received(1);
}

/* Deep sleep: ZMK suspends devices, and the radio powers off. */
ZTEST(defy_hci_spi, test_suspend) {
    zassert_ok(bt_hci_open(hci, recv));

    zassert_ok(pm_device_action_run(hci, PM_DEVICE_ACTION_SUSPEND));
    zassert_equal(fake_radio_counts().power_off_commands, 1);
    zassert_equal(fake_radio_state(), FAKE_RADIO_OFF);
    zassert_equal(received.count, 0);
    zassert_ok(hci->pm_base->action_cb(hci, PM_DEVICE_ACTION_SUSPEND));
}

ZTEST(defy_hci_spi, test_suspend_stops_reading_before_data_ready_falls) {
    zassert_ok(bt_hci_open(hci, recv));
    fake_radio_set_power_off_ms(30);

    zassert_ok(pm_device_action_run(hci, PM_DEVICE_ACTION_SUSPEND));
    k_sleep(K_MSEC(50));

    zassert_equal(fake_radio_counts().power_off_commands, 1);
    zassert_equal(fake_radio_counts().transfers_after_power_off_reply, 0);
    zassert_equal(fake_radio_counts().wakes, 0);
    zassert_equal(fake_radio_state(), FAKE_RADIO_OFF);
    zassert_equal(send_command(BT_HCI_OP_RESET), -EIO);
}

ZTEST(defy_hci_spi, test_suspend_without_answer) {
    int64_t start;
    int64_t took;

    zassert_ok(bt_hci_open(hci, recv));
    fake_radio_kill();

    start = k_uptime_get();
    zassert_equal(pm_device_action_run(hci, PM_DEVICE_ACTION_SUSPEND), -ETIMEDOUT);
    took = k_uptime_get() - start;
    zassert_true(took <= ANSWER_TIMEOUT_MS + POWER_OFF_REPLY_TIMEOUT_MS + 10, "took %lld ms",
                 took);

    zassert_equal(pm_device_action_run(hci, PM_DEVICE_ACTION_RESUME), -EALREADY);
}

static void assert_suspend_failed(int error) {
    zassert_equal(pm_device_action_run(hci, PM_DEVICE_ACTION_SUSPEND), error);
    /* The link stays failed to power off. */
    zassert_equal(hci->pm_base->action_cb(hci, PM_DEVICE_ACTION_SUSPEND), -EIO);
    zassert_equal(send_command(BT_HCI_OP_RESET), -EIO);
}

ZTEST(defy_hci_spi, test_suspend_propagates_transfer_error) {
    zassert_ok(bt_hci_open(hci, recv));
    fake_radio_fail_next_transfer();
    assert_suspend_failed(-EIO);
    zassert_equal(fake_radio_counts().power_off_commands, 0);
}

ZTEST(defy_hci_spi, test_suspend_requires_success_ack) {
    zassert_ok(bt_hci_open(hci, recv));
    fake_radio_set_power_off_reply(BT_HCI_ERR_CMD_DISALLOWED);
    assert_suspend_failed(-EIO);
    zassert_equal(fake_radio_counts().power_off_commands, 1);
    zassert_not_equal(fake_radio_state(), FAKE_RADIO_OFF);
}

ZTEST(defy_hci_spi, test_suspend_accepts_ack_near_deadline) {
    zassert_ok(bt_hci_open(hci, recv));
    fake_radio_set_power_off_timing(POWER_OFF_REPLY_TIMEOUT_MS - 5, 100, 0);
    int64_t started = k_uptime_get();

    zassert_ok(pm_device_action_run(hci, PM_DEVICE_ACTION_SUSPEND));
    int64_t elapsed = k_uptime_get() - started;
    zassert_true(elapsed >= POWER_OFF_REPLY_TIMEOUT_MS - 5);
    zassert_true(elapsed <= POWER_OFF_REPLY_TIMEOUT_MS);
    zassert_equal(fake_radio_state(), FAKE_RADIO_OFF);
    zassert_equal(fake_radio_counts().wakes, 0);
}

static void assert_suspend_stops_before_radio_fallback(int fallback_ms) {
    zassert_ok(bt_hci_open(hci, recv));
    fake_radio_set_power_off_reply(-1);
    fake_radio_set_power_off_timing(0, fallback_ms, 0);
    int64_t started = k_uptime_get();

    assert_suspend_failed(-ETIMEDOUT);
    int64_t elapsed = k_uptime_get() - started;
    zassert_true(elapsed >= POWER_OFF_REPLY_TIMEOUT_MS);
    zassert_true(elapsed <= POWER_OFF_REPLY_TIMEOUT_MS + 2);
    zassert_not_equal(fake_radio_state(), FAKE_RADIO_OFF);
    unsigned int transfers = fake_radio_counts().transfers;
    k_sleep(K_MSEC(fallback_ms + 10));
    zassert_equal(fake_radio_counts().transfers, transfers);
    zassert_equal(fake_radio_counts().wakes, 0);
    zassert_equal(fake_radio_state(), FAKE_RADIO_OFF);
}

ZTEST(defy_hci_spi, test_suspend_stops_before_legacy_radio_fallback) {
    assert_suspend_stops_before_radio_fallback(100);
}

ZTEST(defy_hci_spi, test_suspend_does_not_start_data_phase_after_deadline) {
    zassert_ok(bt_hci_open(hci, recv));
    fake_radio_set_power_off_timing(0, 100, POWER_OFF_REPLY_TIMEOUT_MS + 5);
    unsigned int transfers = fake_radio_counts().transfers;
    int64_t started = k_uptime_get();

    assert_suspend_failed(-ETIMEDOUT);
    int64_t elapsed = k_uptime_get() - started;
    zassert_true(elapsed >= POWER_OFF_REPLY_TIMEOUT_MS + 5);
    zassert_true(elapsed <= POWER_OFF_REPLY_TIMEOUT_MS + 7);
    zassert_equal(fake_radio_counts().transfers, transfers + 3);
    k_sleep(K_MSEC(110));
    zassert_equal(fake_radio_counts().transfers, transfers + 3);
    zassert_equal(fake_radio_counts().wakes, 0);
    zassert_equal(fake_radio_state(), FAKE_RADIO_OFF);
}

ZTEST(defy_hci_spi, test_suspend_rejects_truncated_ack) {
    zassert_ok(bt_hci_open(hci, recv));
    fake_radio_set_power_off_reply(-2);
    assert_suspend_failed(-ETIMEDOUT);
}

static K_THREAD_STACK_DEFINE(blocked_sender_stack, 2048);
static struct k_thread blocked_sender;
static int blocked_sender_result;

static void send_blocked(void *a, void *b, void *c) {
    ARG_UNUSED(a);
    ARG_UNUSED(b);
    ARG_UNUSED(c);
    blocked_sender_result = send_command(BT_HCI_OP_RESET);
}

ZTEST(defy_hci_spi, test_suspend_lock_timeout) {
    zassert_ok(bt_hci_open(hci, recv));
    fake_radio_block_next_transfer();
    k_thread_create(&blocked_sender, blocked_sender_stack, K_THREAD_STACK_SIZEOF(blocked_sender_stack),
                    send_blocked, NULL, NULL, NULL, 5, 0, K_NO_WAIT);
    zassert_ok(fake_radio_wait_blocked());
    int64_t started = k_uptime_get();
    int err = pm_device_action_run(hci, PM_DEVICE_ACTION_SUSPEND);
    int64_t elapsed = k_uptime_get() - started;

    fake_radio_unblock_transfer();
    zassert_ok(k_thread_join(&blocked_sender, K_MSEC(100)));
    zassert_equal(err, -ETIMEDOUT);
    zassert_true(elapsed >= 100 && elapsed <= 110, "elapsed %lld", elapsed);
    zassert_ok(blocked_sender_result);
    zassert_equal(fake_radio_counts().power_off_commands, 0);
    zassert_equal(send_command(BT_HCI_OP_RESET), -EIO);
}

/* A link held by a stalled transfer makes the refresh give up, not wait. */
ZTEST(defy_hci_spi, test_battery_refresh_gives_up_on_busy_link) {
    zassert_ok(bt_hci_open(hci, recv));
    fake_radio_block_next_transfer();
    k_thread_create(&blocked_sender, blocked_sender_stack, K_THREAD_STACK_SIZEOF(blocked_sender_stack),
                    send_blocked, NULL, NULL, NULL, 5, 0, K_NO_WAIT);
    zassert_ok(fake_radio_wait_blocked());
    int64_t started = k_uptime_get();
    int err = defy_hci_spi_battery_refresh();
    int64_t elapsed = k_uptime_get() - started;

    fake_radio_unblock_transfer();
    zassert_ok(k_thread_join(&blocked_sender, K_MSEC(100)));
    zassert_equal(err, -EBUSY);
    zassert_true(elapsed >= 100 && elapsed <= 110, "elapsed %lld", elapsed);
    zassert_ok(blocked_sender_result);
    zassert_equal(fake_radio_counts().refresh_commands, 0);
    zassert_ok(defy_hci_spi_battery_refresh());
    zassert_equal(fake_radio_counts().refresh_commands, 1);
}

ZTEST(defy_hci_spi, test_resume_after_ack_reboots) {
    zassert_ok(bt_hci_open(hci, recv));
    zassert_ok(pm_device_action_run(hci, PM_DEVICE_ACTION_SUSPEND));
    atomic_set(&reboot_expected, true);
    reboot_jumper = k_current_get();
    if (setjmp(reboot_env) == 0) {
        pm_device_action_run(hci, PM_DEVICE_ACTION_RESUME);
        zassert_unreachable();
    }
    atomic_set(&reboot_expected, false);
    zassert_equal(reboot_kind, SYS_REBOOT_COLD);
}

ZTEST(defy_hci_spi, test_resume_callback_after_failed_shutdown_reboots) {
    zassert_ok(bt_hci_open(hci, recv));
    fake_radio_set_power_off_reply(-1);
    assert_suspend_failed(-ETIMEDOUT);
    atomic_set(&reboot_expected, true);
    reboot_jumper = k_current_get();
    if (setjmp(reboot_env) == 0) {
        hci->pm_base->action_cb(hci, PM_DEVICE_ACTION_RESUME);
        zassert_unreachable();
    }
    atomic_set(&reboot_expected, false);
    zassert_equal(reboot_kind, SYS_REBOOT_COLD);
}

/* A ready event while running: the radio restarted. Within a minute of start, Bluetooth stops. */
ZTEST(defy_hci_spi, test_radio_restarts) {
    static const uint8_t ready[] = DEFY_HCI_SPI_READY_EVENT;
    uint8_t msg[1 + sizeof(ready)] = {BT_HCI_H4_EVT};

    zassert_true(k_uptime_get() < 60000, "would reboot");
    zassert_ok(bt_hci_open(hci, recv));

    memcpy(&msg[1], ready, sizeof(ready));
    fake_radio_send(msg, sizeof(msg));
    k_sleep(K_MSEC(50));

    zassert_equal(received.count, 0);
    zassert_equal(send_command(BT_HCI_OP_RESET), -EIO);
    zassert_equal(pm_device_action_run(hci, PM_DEVICE_ACTION_SUSPEND), -ENOTCONN);
    zassert_equal(bt_hci_open(hci, recv), -EALREADY);
}

/*
 * A vendor event reaches the host. The link only delivers while it is up, and
 * a failed link stays failed, so it has been up since the open.
 */
static void send_vendor_event(void) {
    static const uint8_t event[] = {BT_HCI_H4_EVT, BT_HCI_EVT_VENDOR, 1, 0x42};
    int count = received.count;
    fake_radio_send(event, sizeof(event));
    wait_received(count + 1);
}

ZTEST(defy_hci_spi, test_rx_failure_is_bounded_before_reboot_guard) {
    static const enum fake_radio_rx_fault faults[] = {
        FAKE_RX_SILENT, FAKE_RX_EMPTY, FAKE_RX_INVALID, FAKE_RX_ERROR,
    };

    for (size_t i = 0; i < ARRAY_SIZE(faults); i++) {
        zassert_true(k_uptime_get() < 60000);
        zassert_ok(bt_hci_open(hci, recv));
        fake_radio_rx_fault(faults[i]);
        k_sleep(K_MSEC(1200));
        zassert_equal(pm_device_action_run(hci, PM_DEVICE_ACTION_SUSPEND), -ENOTCONN,
                      "fault %d", faults[i]);
        zassert_equal(send_command(BT_HCI_OP_RESET), -EIO);
        unsigned int transfers = fake_radio_counts().transfers;
        k_sleep(K_MSEC(50));
        zassert_equal(fake_radio_counts().transfers, transfers);
        before(NULL);
    }
}

ZTEST(defy_hci_spi, test_valid_rx_frame_resets_failure_budget) {
    zassert_ok(bt_hci_open(hci, recv));
    fake_radio_rx_fault(FAKE_RX_SILENT);
    k_sleep(K_MSEC(600));
    send_vendor_event();
    fake_radio_rx_fault(FAKE_RX_INVALID);
    k_sleep(K_MSEC(600));
    fake_radio_rx_fault(FAKE_RX_NONE);
    send_vendor_event();
}

ZTEST(defy_hci_spi, test_idle_irq_resets_failure_budget) {
    zassert_ok(bt_hci_open(hci, recv));
    fake_radio_rx_fault(FAKE_RX_EMPTY);
    k_sleep(K_MSEC(600));
    fake_radio_rx_fault(FAKE_RX_NONE);
    k_sleep(K_MSEC(50));
    fake_radio_rx_fault(FAKE_RX_EMPTY);
    k_sleep(K_MSEC(600));
    fake_radio_rx_fault(FAKE_RX_NONE);
    send_vendor_event();
}

ZTEST(defy_hci_spi, test_idle_controller_is_not_polled_or_failed) {
    zassert_ok(bt_hci_open(hci, recv));
    k_sleep(K_MSEC(50));
    unsigned int transfers = fake_radio_counts().transfers;
    k_sleep(K_MSEC(1200));
    zassert_equal(fake_radio_counts().transfers, transfers);
    send_vendor_event();
}

ZTEST(defy_hci_spi, test_short_hci_frames_are_not_delivered) {
    static const uint8_t frames[][6] = {
        {BT_HCI_H4_EVT},
        {BT_HCI_H4_EVT, BT_HCI_EVT_VENDOR},
        {BT_HCI_H4_EVT, BT_HCI_EVT_VENDOR, 1},
        {BT_HCI_H4_ACL, 1, 0, 0},
        {BT_HCI_H4_ACL, 1, 0, 2, 0, 0x42},
        {0xff},
    };
    static const size_t lengths[] = {1, 2, 3, 4, 6, 1};
    zassert_ok(bt_hci_open(hci, recv));
    for (size_t i = 0; i < ARRAY_SIZE(frames); i++) {
        fake_radio_send(frames[i], lengths[i]);
    }
    send_vendor_event();
    zassert_equal(received.count, 1);
    zassert_equal(received.data[0][0], BT_HCI_EVT_VENDOR);
}

ZTEST(defy_hci_spi, test_truncated_ready_event_does_not_trigger_recovery) {
    const uint8_t event[] = {BT_HCI_H4_EVT, BT_HCI_EVT_VENDOR, 3,
                             DEFY_HCI_SPI_EVT_READY & 0xff, DEFY_HCI_SPI_EVT_READY >> 8};
    zassert_ok(bt_hci_open(hci, recv));
    fake_radio_send(event, sizeof(event));
    send_vendor_event();
    zassert_equal(received.count, 1);
}

ZTEST(defy_hci_spi, test_spi_ownership_wait_does_not_count_as_rx_failure) {
    zassert_ok(bt_hci_open(hci, recv));
    fake_radio_block_next_transfer();
    k_thread_create(&blocked_sender, blocked_sender_stack, K_THREAD_STACK_SIZEOF(blocked_sender_stack),
                    send_blocked, NULL, NULL, NULL, 5, 0, K_NO_WAIT);
    zassert_ok(fake_radio_wait_blocked());
    fake_radio_rx_fault(FAKE_RX_SILENT);
    k_sleep(K_MSEC(1200));
    fake_radio_rx_fault(FAKE_RX_NONE);
    fake_radio_unblock_transfer();
    zassert_ok(k_thread_join(&blocked_sender, K_MSEC(100)));
    zassert_ok(blocked_sender_result);
    wait_received(1);
}

ZTEST(defy_hci_spi, test_poweroff_owns_timeout_near_rx_failure_limit) {
    zassert_ok(bt_hci_open(hci, recv));
    fake_radio_rx_fault(FAKE_RX_EMPTY);
    k_sleep(K_MSEC(950));
    /* Only a link still up tries to power off and times out. */
    assert_suspend_failed(-ETIMEDOUT);
    unsigned int transfers = fake_radio_counts().transfers;
    k_sleep(K_MSEC(100));
    zassert_equal(fake_radio_counts().transfers, transfers);
}

ZTEST(defy_hci_spi, test_single_slow_header_keeps_healthy_link) {
    static const uint8_t event[] = {BT_HCI_H4_EVT, BT_HCI_EVT_VENDOR, 1, 0x42};
    zassert_ok(bt_hci_open(hci, recv));
    fake_radio_block_next_transfer();
    fake_radio_send(event, sizeof(event));
    zassert_ok(fake_radio_wait_blocked());
    k_sleep(K_MSEC(1200));
    fake_radio_unblock_transfer();
    wait_received(1);
    zassert_mem_equal(received.data[0], &event[1], sizeof(event) - 1);
}

/* A minute or more after the start, a radio that stops answering reboots the RP2040. */
ZTEST(defy_hci_spi_after_a_minute, test_silent_radio_reboots_after_startup_guard) {
    zassert_ok(bt_hci_open(hci, recv));
    int64_t remaining = 60000 - k_uptime_get();
    if (remaining > 0) {
        k_sleep(K_MSEC(remaining));
    }
    atomic_set(&reboot_expected, true);
    fake_radio_rx_fault(FAKE_RX_SILENT);
    zassert_ok(k_sem_take(&rebooted, K_MSEC(1500)));
    zassert_equal(reboot_kind, SYS_REBOOT_COLD);
    zassert_equal(pm_device_action_run(hci, PM_DEVICE_ACTION_SUSPEND), -ENOTCONN);
    zassert_equal(bt_hci_open(hci, recv), -EALREADY);
    unsigned int transfers = fake_radio_counts().transfers;
    k_sleep(K_MSEC(100));
    zassert_equal(fake_radio_counts().transfers, transfers);
    zassert_equal(received.count, 0);
}

/*
 * test_main() runs the suites in two passes, so that defy_hci_spi's tests of
 * the first minute after boot all run before the minute is waited out,
 * whatever order ztest gives the suites.
 */
enum pass {
    WITHIN_A_MINUTE,
    AFTER_A_MINUTE,
};

static bool within_a_minute(const void *state) {
    return *(const enum pass *)state == WITHIN_A_MINUTE;
}

static bool after_a_minute(const void *state) {
    return *(const enum pass *)state == AFTER_A_MINUTE;
}

void test_main(void) {
    static const enum pass passes[] = {WITHIN_A_MINUTE, AFTER_A_MINUTE};

    for (size_t i = 0; i < ARRAY_SIZE(passes); i++) {
        ztest_run_all(&passes[i], false, 1, 1);
    }
    ztest_verify_all_test_suites_ran();
}

ZTEST_SUITE(defy_hci_spi, within_a_minute, NULL, before, NULL, NULL);
ZTEST_SUITE(defy_hci_spi_after_a_minute, after_a_minute, NULL, before, NULL, NULL);
