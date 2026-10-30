/*
 * The RP2040's HCI SPI driver installing the radio firmware through Dygma's
 * rf_bootloader as it starts the radio: over Dygma's radio application, at a
 * cold boot, after an interrupted install, and not at all when the radio
 * already holds it. Also its look for the bootloader at power-on, before
 * Bluetooth starts, the one session per start of the driver, and the start-up
 * stages it reports on the way.
 *
 * SPDX-License-Identifier: MIT
 */

#include <zephyr/drivers/bluetooth.h>
#include <zephyr/kernel.h>
#include <zephyr/ztest.h>

#include <dygma/defy_hci_spi.h>
#include <dygma/defy_radio_factory.h>
#include <dygma/defy_radio_image.h>
#include <dygma/defy_startup.h>

#include "fake_radio.h"

static const struct device *const hci = DEVICE_DT_GET(DT_NODELABEL(radio));

/* What the half's LEDs would show of the start-up, as the driver reports it. */
static struct {
    enum defy_startup_stage stages[16];
    size_t count;
    unsigned int progress_reports;
    size_t written, size;
    unsigned int sessions;
    int session_result;
    unsigned int install_reports;
    int install_result;
    bool install_attempted;
    struct defy_radio_factory factory;
} startup;

void defy_startup_report(enum defy_startup_stage stage) {
    zassert_true(startup.count < ARRAY_SIZE(startup.stages));
    startup.stages[startup.count++] = stage;
}

void defy_startup_progress(size_t written, size_t size) {
    zassert_true(written <= size);
    startup.progress_reports++;
    startup.written = written;
    startup.size = size;
}

void defy_startup_radio_answer(uint8_t first, uint8_t second) {
    ARG_UNUSED(first);
    ARG_UNUSED(second);
}

void defy_startup_radio_session(int err) {
    startup.sessions++;
    startup.session_result = err;
}

void defy_startup_radio_install(const struct defy_radio_factory *factory, int err, bool attempted) {
    startup.install_reports++;
    startup.install_result = err;
    startup.install_attempted = attempted;
    startup.factory = *factory;
}

/* The driver's FACTORY_SESSIONS: sessions that fail to begin before the look gives up. */
#define FACTORY_SESSIONS 3

#define assert_stages(...)                                                                         \
    do {                                                                                           \
        const enum defy_startup_stage expected[] = {__VA_ARGS__};                                  \
        zassert_equal(startup.count, ARRAY_SIZE(expected));                                        \
        zassert_mem_equal(startup.stages, expected, sizeof(expected));                             \
    } while (0)

static int recv(const struct device *dev, struct net_buf *buf) {
    ARG_UNUSED(dev);
    net_buf_unref(buf);
    return 0;
}

static void before(void *fixture) {
    ARG_UNUSED(fixture);
    defy_hci_spi_test_reset();
    memset(&startup, 0, sizeof(startup));
}

static void assert_installed_with_progress(void) {
    zassert_true(fake_radio_holds_image());
    zassert_true(startup.progress_reports > 1);
    zassert_equal(startup.written, startup.size);
    zassert_equal(fake_radio_counts().short_frames, 0);
    zassert_equal(fake_radio_counts().truncated_replies, 0);
    zassert_equal(startup.install_reports, 1);
    zassert_ok(startup.install_result);
    zassert_true(startup.install_attempted);
    zassert_true(startup.factory.finish_transmitted);
}

ZTEST(defy_radio_install, test_install_failure_keeps_the_command_and_stops_retries) {
    const uint8_t commands[] = {'S', 'E', 'W', 'V'};

    for (size_t i = 0; i < ARRAY_SIZE(commands); i++) {
        before(NULL);
        fake_radio_reset_stock();
        fake_radio_reject_command(commands[i]);
        fake_radio_power_on();
        defy_hci_spi_catch_bootloader();

        zassert_equal(startup.install_reports, 1);
        zassert_equal(startup.install_result, -EPROTO);
        zassert_true(startup.install_attempted);
        zassert_equal(startup.factory.trace.command, commands[i]);
        zassert_equal(startup.factory.trace.phase, DEFY_RADIO_PHASE_SEND_STATUS);
        zassert_equal(startup.factory.trace.rx[0], 2);
        zassert_equal(startup.factory.trace.rx[1], 0x81);
        zassert_false(startup.factory.finish_transmitted);
        struct fake_radio_counts counts = fake_radio_counts();

        zassert_equal(counts.rejected_commands, 1);
        /* The session had begun: no second one, before or after the erases. */
        zassert_equal(startup.sessions, 1);
        zassert_equal(bt_hci_open(hci, recv), -EIO);
        zassert_equal(startup.sessions, 1);
        zassert_equal(fake_radio_counts().transfers, counts.transfers);
        zassert_equal(startup.install_reports, 1);
        zassert_equal(startup.stages[startup.count - 1], DEFY_STARTUP_RADIO_FAILED);
        if (commands[i] == 'S' || commands[i] == 'E') {
            zassert_equal(counts.erases + counts.writes, 0);
            zassert_equal(startup.progress_reports, 0);
        } else {
            zassert_true(counts.erases > 0);
            zassert_true(startup.progress_reports > 0);
            if (commands[i] == 'V') {
                zassert_equal(startup.written, startup.size);
                zassert_true(startup.written > 0);
            } else {
                zassert_equal(startup.written, 0);
            }
        }
    }
}

ZTEST(defy_radio_install, test_failure_during_hci_open_is_not_retried) {
    fake_radio_reset_stock();
    fake_radio_reject_command('W');
    fake_radio_power_on();

    zassert_equal(bt_hci_open(hci, recv), -EIO);
    zassert_equal(startup.install_reports, 1);
    zassert_equal(fake_radio_counts().rejected_commands, 1);
    zassert_equal(startup.factory.trace.command, 'W');
    zassert_equal(startup.install_result, -EPROTO);
}

ZTEST(defy_radio_install, test_radio_remaining_in_bootloader_is_not_rewritten) {
    fake_radio_reset_stock();
    fake_radio_reject_command('F');
    fake_radio_power_on();
    defy_hci_spi_catch_bootloader();
    struct fake_radio_counts counts = fake_radio_counts();

    zassert_true(counts.writes > 0);
    zassert_equal(counts.rejected_commands, 1);
    zassert_equal(bt_hci_open(hci, recv), -EIO);
    zassert_equal(fake_radio_counts().erases, counts.erases);
    zassert_equal(fake_radio_counts().writes, counts.writes);
    zassert_equal(fake_radio_counts().begins, counts.begins);
    zassert_equal(startup.install_reports, 1);
    zassert_ok(startup.install_result);
    zassert_equal(startup.factory.trace.command, 'F');
    zassert_true(startup.factory.finish_transmitted);
    zassert_equal(startup.stages[startup.count - 1], DEFY_STARTUP_RADIO_FAILED);
}

ZTEST(defy_radio_install, test_pending_erase_reply_is_requested_again) {
    fake_radio_reset_stock();
    fake_radio_drop_receive_request('E');
    fake_radio_power_on();
    defy_hci_spi_catch_bootloader();

    zassert_ok(startup.install_result);
    assert_installed_with_progress();
    struct fake_radio_counts counts = fake_radio_counts();

    zassert_equal(counts.dropped_receive_requests, 1);
    zassert_equal(counts.begins, 1);
    zassert_equal(counts.erases, DIV_ROUND_UP(defy_radio_image_size + 4, 4096));
    zassert_equal(counts.finishes, 1);
    zassert_ok(bt_hci_open(hci, recv));
}

/*
 * Nothing resets Dygma's application into its bootloader over SPI, so its
 * sessions fail to begin and the start fails. Each begin times out, and each
 * failure reopens the window, so all three sessions run.
 */
ZTEST(defy_radio_install, test_stock_application_fails_the_start) {
    fake_radio_reset_stock();
    int64_t started = k_uptime_get();

    zassert_equal(bt_hci_open(hci, recv), -EIO);
    struct fake_radio_counts counts = fake_radio_counts();

    zassert_equal(startup.sessions, FACTORY_SESSIONS);
    zassert_equal(startup.session_result, -ETIMEDOUT);
    zassert_equal(counts.erases + counts.writes + counts.seals, 0);
    zassert_equal(startup.progress_reports, 0);
    zassert_equal(fake_radio_state(), FAKE_RADIO_STOCK);
    assert_stages(DEFY_STARTUP_RADIO_FAILED);
    /* The sessions' own timeouts, about a second each, with none in the later start attempts. */
    zassert_true(k_uptime_get() - started < FACTORY_SESSIONS * 1000 + 500);
}

/*
 * A bootloader not Dygma's is left alone after one session, met by the look or
 * by the open: unlike other failures to begin, -ENOTSUP is not tried again.
 */
ZTEST(defy_radio_install, test_foreign_bootloader_fails_the_start) {
    for (int look = 0; look < 2; look++) {
        before(NULL);
        fake_radio_reset_stock();
        fake_radio_foreign_bootloader(true);
        fake_radio_power_on();
        if (look) {
            defy_hci_spi_catch_bootloader();
            zassert_equal(startup.sessions, 1);
            zassert_equal(startup.count, 0);
        }

        zassert_equal(bt_hci_open(hci, recv), -EIO);
        struct fake_radio_counts counts = fake_radio_counts();

        zassert_equal(startup.sessions, 1);
        zassert_equal(startup.session_result, -ENOTSUP);
        zassert_equal(counts.infos, 1);
        zassert_equal(counts.seals + counts.erases + counts.writes + counts.finishes, 0);
        zassert_equal(startup.install_reports, 0);
        assert_stages(DEFY_STARTUP_RADIO_FAILED);
    }
}

/* A session that failed to begin wrote nothing: the next one installs, at the look or the open. */
ZTEST(defy_radio_install, test_session_that_fails_to_begin_is_tried_again) {
    for (int look = 0; look < 2; look++) {
        before(NULL);
        fake_radio_reset_stock();
        fake_radio_reject_hellos(1);
        fake_radio_power_on();
        if (look) {
            defy_hci_spi_catch_bootloader();
        }

        zassert_ok(bt_hci_open(hci, recv));
        zassert_equal(fake_radio_counts().rejected_hellos, 1);
        zassert_equal(startup.sessions, 2);
        zassert_equal(fake_radio_counts().begins, 1);
        zassert_equal(fake_radio_counts().seals, 1);
        assert_installed_with_progress();
        assert_stages(DEFY_STARTUP_RADIO_BOOTLOADER, DEFY_STARTUP_RADIO_READY);
    }
}

/* A begin that fails only after the look's window has passed still gets another session. */
ZTEST(defy_radio_install, test_slow_failure_to_begin_is_tried_again) {
    fake_radio_reset_stock();
    fake_radio_ignore_until_next_begin('I');
    fake_radio_power_on();

    defy_hci_spi_catch_bootloader();
    zassert_ok(bt_hci_open(hci, recv));
    zassert_true(fake_radio_counts().ignored_receive_requests > 0);
    zassert_equal(startup.sessions, 2);
    assert_installed_with_progress();
    assert_stages(DEFY_STARTUP_RADIO_BOOTLOADER, DEFY_STARTUP_RADIO_READY);
}

/* Three sessions that fail to begin end the look, and the start fails without another. */
ZTEST(defy_radio_install, test_three_sessions_that_fail_to_begin_give_up) {
    fake_radio_reset_stock();
    fake_radio_reject_hellos(100);
    fake_radio_power_on();

    defy_hci_spi_catch_bootloader();
    zassert_equal(startup.sessions, 3);
    zassert_equal(startup.session_result, -EPROTO);
    zassert_equal(startup.count, 0);

    zassert_equal(bt_hci_open(hci, recv), -EIO);
    struct fake_radio_counts counts = fake_radio_counts();

    zassert_equal(startup.sessions, 3);
    zassert_equal(counts.rejected_hellos, 3);
    zassert_equal(counts.begins + counts.seals + counts.erases + counts.writes, 0);
    zassert_equal(counts.finishes, 0);
    assert_stages(DEFY_STARTUP_RADIO_FAILED);
}

/* Both chips power up together: the bootloader is still listening. */
ZTEST(defy_radio_install, test_cold_boot_installs_in_the_listening_bootloader) {
    fake_radio_reset_stock();
    fake_radio_power_on();

    zassert_ok(bt_hci_open(hci, recv));
    struct fake_radio_counts counts = fake_radio_counts();
    zassert_equal(startup.sessions, 1);
    zassert_equal(counts.windows_expired, 0);
    zassert_equal(counts.seals, 1);
    assert_installed_with_progress();
    assert_stages(DEFY_STARTUP_RADIO_BOOTLOADER, DEFY_STARTUP_RADIO_READY);
}

/* Every start reboots the radio through the bootloader, which then only lets it go. */
ZTEST(defy_radio_install, test_installed_firmware_is_left_alone) {
    fake_radio_reset_installed();

    zassert_ok(bt_hci_open(hci, recv));
    struct fake_radio_counts counts = fake_radio_counts();
    zassert_equal(counts.reboot_commands, 1);
    zassert_equal(counts.bootloader_entries, 1);
    zassert_equal(counts.finishes, 1);
    zassert_equal(counts.erases + counts.writes + counts.seals, 0);
    zassert_equal(counts.windows_expired, 0);
    zassert_true(fake_radio_holds_image());
    zassert_equal(startup.progress_reports, 0);
    assert_stages(DEFY_STARTUP_RADIO_BOOTLOADER, DEFY_STARTUP_RADIO_READY);
}

/* Without S113 the slot is not where the bootloader says: nothing is written. */
ZTEST(defy_radio_install, test_radio_without_s113_is_not_written) {
    fake_radio_reset_stock();
    fake_radio_remove_s113();
    fake_radio_power_on();

    zassert_equal(bt_hci_open(hci, recv), -EIO);
    struct fake_radio_counts counts = fake_radio_counts();
    zassert_equal(counts.erases + counts.writes + counts.seals, 0);
    zassert_equal(counts.finishes, 1);
    zassert_false(fake_radio_holds_image());
    zassert_equal(startup.stages[startup.count - 1], DEFY_STARTUP_RADIO_FAILED);
}

/* At power-on the look installs before Bluetooth starts, which then only meets the new firmware. */
ZTEST(defy_radio_install, test_power_on_look_installs_before_bluetooth) {
    fake_radio_reset_stock();
    fake_radio_power_on();

    defy_hci_spi_catch_bootloader();
    struct fake_radio_counts counts = fake_radio_counts();
    zassert_equal(counts.seals, 1);
    zassert_equal(counts.windows_expired, 0);
    assert_installed_with_progress();

    zassert_ok(bt_hci_open(hci, recv));
    counts = fake_radio_counts();
    zassert_equal(counts.bootloader_entries + counts.reboot_commands, 0);
    assert_stages(DEFY_STARTUP_RADIO_BOOTLOADER, DEFY_STARTUP_RADIO_READY);
}

/* The look gives Dygma's application its sessions, and the open does not try again. */
ZTEST(defy_radio_install, test_power_on_look_meets_the_stock_application_once) {
    fake_radio_reset_stock();

    defy_hci_spi_catch_bootloader();
    zassert_equal(startup.sessions, FACTORY_SESSIONS);
    zassert_equal(startup.count, 0);

    zassert_equal(bt_hci_open(hci, recv), -EIO);
    struct fake_radio_counts counts = fake_radio_counts();

    zassert_equal(startup.sessions, FACTORY_SESSIONS);
    zassert_equal(counts.seals + counts.erases + counts.writes, 0);
    zassert_equal(startup.progress_reports, 0);
    zassert_equal(fake_radio_state(), FAKE_RADIO_STOCK);
    assert_stages(DEFY_STARTUP_RADIO_FAILED);
}

ZTEST(defy_radio_install, test_power_on_look_checks_a_running_controller_once) {
    fake_radio_reset_installed();

    defy_hci_spi_catch_bootloader();
    defy_hci_spi_catch_bootloader();
    zassert_ok(bt_hci_open(hci, recv));
    struct fake_radio_counts counts = fake_radio_counts();

    zassert_equal(counts.bootloader_entries, 1);
    zassert_equal(counts.reboot_commands, 1);
    zassert_equal(counts.finishes, 1);
    zassert_equal(counts.infos, 1);
    zassert_equal(counts.erases + counts.writes + counts.seals, 0);
    assert_stages(DEFY_STARTUP_RADIO_BOOTLOADER, DEFY_STARTUP_RADIO_READY);
}

ZTEST(defy_radio_install, test_pending_ready_from_older_radio_is_updated_early) {
    fake_radio_reset_custom(defy_radio_image_version ^ 1u, true);

    defy_hci_spi_catch_bootloader();
    assert_installed_with_progress();
    zassert_ok(bt_hci_open(hci, recv));
    zassert_equal(fake_radio_counts().reboot_commands, 1);
    zassert_equal(fake_radio_counts().infos, 1);
    assert_stages(DEFY_STARTUP_RADIO_BOOTLOADER, DEFY_STARTUP_RADIO_READY);
}

ZTEST(defy_radio_install, test_pending_ready_from_older_radio_is_updated_at_open) {
    fake_radio_reset_custom(defy_radio_image_version ^ 1u, true);

    zassert_ok(bt_hci_open(hci, recv));
    assert_installed_with_progress();
    zassert_equal(fake_radio_counts().reboot_commands, 1);
    zassert_equal(fake_radio_counts().infos, 1);
    assert_stages(DEFY_STARTUP_RADIO_BOOTLOADER, DEFY_STARTUP_RADIO_READY);
}

ZTEST(defy_radio_install, test_pending_ready_matching_radio_is_checked_without_writes) {
    fake_radio_reset_custom(defy_radio_image_version, true);

    defy_hci_spi_catch_bootloader();
    zassert_ok(bt_hci_open(hci, recv));
    struct fake_radio_counts counts = fake_radio_counts();

    zassert_equal(counts.reboot_commands, 1);
    zassert_equal(counts.infos, 1);
    zassert_equal(counts.finishes, 1);
    zassert_equal(counts.erases + counts.writes + counts.seals, 0);
    zassert_false(startup.install_attempted);
    zassert_ok(startup.install_result);
}

ZTEST(defy_radio_install, test_ignored_early_reboot_cannot_accept_stale_ready) {
    fake_radio_reset_custom(defy_radio_image_version ^ 1u, true);
    fake_radio_ignore_reboot(true);
    int64_t started = k_uptime_get();

    defy_hci_spi_catch_bootloader();
    zassert_true(k_uptime_get() - started < 500);
    zassert_equal(bt_hci_open(hci, recv), -EIO);
    zassert_equal(fake_radio_counts().reboot_commands, 1);
    zassert_equal(fake_radio_counts().infos, 0);
    zassert_equal(fake_radio_counts().erases + fake_radio_counts().writes, 0);
    assert_stages(DEFY_STARTUP_RADIO_FAILED);
}

ZTEST(defy_radio_install, test_ignored_open_reboot_cannot_accept_stale_ready) {
    fake_radio_reset_custom(defy_radio_image_version ^ 1u, true);
    fake_radio_ignore_reboot(true);

    zassert_equal(bt_hci_open(hci, recv), -EIO);
    zassert_equal(fake_radio_counts().reboot_commands, 1);
    zassert_equal(fake_radio_counts().infos, 0);
    assert_stages(DEFY_STARTUP_RADIO_FAILED);
}

ZTEST(defy_radio_install, test_reboot_without_factory_window_cannot_accept_new_ready) {
    fake_radio_reset_custom(defy_radio_image_version ^ 1u, true);
    fake_radio_skip_bootloader(true);

    defy_hci_spi_catch_bootloader();
    zassert_equal(bt_hci_open(hci, recv), -EIO);
    zassert_equal(fake_radio_counts().reboot_commands, 1);
    zassert_equal(fake_radio_counts().infos, 0);
    assert_stages(DEFY_STARTUP_RADIO_FAILED);
}

/* A radio asleep in S113, its install cut off: the look gives up in time, and only power-on helps. */
ZTEST(defy_radio_install, test_power_on_look_gives_up_on_a_silent_radio) {
    fake_radio_reset_stock();
    fake_radio_erase_seal();
    fake_radio_power_on();
    k_sleep(K_MSEC(400));
    zassert_equal(fake_radio_state(), FAKE_RADIO_SILENT);

    int64_t started = k_uptime_get();

    defy_hci_spi_catch_bootloader();
    zassert_true(k_uptime_get() - started < 250);
    zassert_equal(fake_radio_counts().finishes, 0);
    zassert_equal(startup.count, 0);

    fake_radio_power_on();
    defy_hci_spi_catch_bootloader();
    zassert_equal(fake_radio_counts().seals, 1);
    zassert_true(fake_radio_holds_image());
}

ZTEST_SUITE(defy_radio_install, NULL, NULL, before, NULL, NULL);
