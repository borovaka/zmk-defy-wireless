/* SPDX-License-Identifier: MIT */

#include <zephyr/ztest.h>
#include <zephyr/drivers/flash.h>
#include <zephyr/drivers/uart.h>
#include <zephyr/drivers/uart/cdc_acm.h>
#include <zephyr/init.h>
#include <zephyr/storage/flash_map.h>
#include <zephyr/sys/reboot.h>

#include <dygma/defy_updater.h>

static void test_reboot(int type);
static int test_page_info(const struct device *dev, off_t offset, struct flash_pages_info *info);
static int test_line_ctrl_get(const struct device *dev, uint32_t ctrl, uint32_t *value);

#define sys_reboot test_reboot
#define flash_get_page_info_by_offs test_page_info
#define uart_line_ctrl_get test_line_ctrl_get
#undef SYS_INIT
#define SYS_INIT(...)
#include "../../../../bootloader/defy_neuron_bootloader.c"
#undef sys_reboot
#undef flash_get_page_info_by_offs
#undef uart_line_ctrl_get

DEVICE_DT_DEFINE(DT_NODELABEL(boot_test_cdc), NULL, NULL, NULL, NULL, POST_KERNEL, 40, NULL);
DEVICE_DT_DEFINE(DT_NODELABEL(updater_test_cdc), NULL, NULL, NULL, NULL, POST_KERNEL, 40, NULL);

static const uint8_t expected_record[] = {
    1, 0, 0, 0, 16, 0, 0, 0, 0x5d, 0x50, 0x77, 0xf6, 1, 0, 0, 0,
};
static uint8_t flash_bytes[0x80000];
static struct flash_area boot_area;
static struct flash_area request_area;
static struct flash_pages_info page_info;
static cdc_dte_rate_callback_t rate_callback;
static uint32_t current_baud;
static uint32_t current_dtr;
static int open_error;
static int page_error;
static int boot_read_error;
static int request_read_failure;
static int erase_error;
static int write_error;
static int line_error;
static bool corrupt_readback;
static uint32_t alignment;
static unsigned int boot_reads;
static unsigned int request_reads;
static unsigned int erases;
static unsigned int writes;
static unsigned int reboots;
static unsigned int opens;
static unsigned int closes;
static struct defy_updater updater;
static unsigned int half_opens;
static unsigned int half_closes;
static int half_close_error;
static bool updater_during_request;
static int contending_open_status;

static int half_open(void) {
    half_opens++;
    return 0;
}

static int half_close(void) {
    half_closes++;
    return half_close_error;
}

static const struct defy_updater_ops half_ops = {
    .open = half_open,
    .close = half_close,
};

static int half_request(uint8_t op, uint32_t sequence) {
    uint8_t request[20] = {'D', 'U', 1, op};
    uint8_t response[DEFY_UPDATER_FRAME];
    sys_put_le32(1, request + 4);
    sys_put_le32(sequence, request + 8);
    sys_put_le32(crc32_ieee(request, 16), request + 16);
    zassert_equal(defy_updater_process(&updater, request, sizeof(request), response,
                                      k_uptime_get()), 20);
    return (int16_t)sys_get_le16(response + 14);
}

int flash_area_open(uint8_t id, const struct flash_area **area) {
    ++opens;
    if (updater_during_request && opens == 1) {
        contending_open_status = half_request(2, 1);
    }
    if ((int)opens == open_error) {
        return -ENODEV;
    }
    if (id == FIXED_PARTITION_ID(boot_partition)) {
        *area = &boot_area;
    } else {
        zassert_equal(id, FIXED_PARTITION_ID(boot_request_partition));
        *area = &request_area;
    }
    return 0;
}

void flash_area_close(const struct flash_area *area) {
    zassert_true(area == &boot_area || area == &request_area);
    ++closes;
}

uint32_t flash_area_align(const struct flash_area *area) {
    zassert_equal_ptr(area, &request_area);
    return alignment;
}

int flash_area_read(const struct flash_area *area, off_t offset, void *dst, size_t length) {
    zassert_true(offset >= 0);
    if (area == &boot_area) {
        ++boot_reads;
        zassert_true(offset + length <= DEFY_BOOT_LENGTH);
        if (boot_read_error) {
            return boot_read_error;
        }
    } else {
        zassert_equal_ptr(area, &request_area);
        zassert_equal(offset, 0);
        zassert_equal(length, sizeof(expected_record));
        ++request_reads;
        if ((int)request_reads == request_read_failure) {
            return -EIO;
        }
    }
    memcpy(dst, &flash_bytes[area->fa_off + offset], length);
    if (area == &request_area && request_reads == 2 && corrupt_readback) {
        ((uint8_t *)dst)[0] ^= 1;
    }
    return 0;
}

int flash_area_erase(const struct flash_area *area, off_t offset, size_t length) {
    zassert_equal_ptr(area, &request_area);
    zassert_equal(area->fa_off, DEFY_BOOT_REQUEST_ADDRESS);
    zassert_equal(offset, 0);
    zassert_equal(length, DEFY_BOOT_REQUEST_PAGE_SIZE);
    ++erases;
    if (erase_error) {
        return erase_error;
    }
    memset(&flash_bytes[DEFY_BOOT_REQUEST_ADDRESS], 0xff, length);
    return 0;
}

int flash_area_write(const struct flash_area *area, off_t offset, const void *src, size_t length) {
    zassert_equal_ptr(area, &request_area);
    zassert_equal(area->fa_off, DEFY_BOOT_REQUEST_ADDRESS);
    zassert_equal(offset, 0);
    zassert_equal(length, sizeof(expected_record));
    zassert_mem_equal(src, expected_record, length);
    zassert_equal(erases, 1);
    ++writes;
    memcpy(&flash_bytes[DEFY_BOOT_REQUEST_ADDRESS], src, length);
    return write_error;
}

static int test_page_info(const struct device *dev, off_t offset, struct flash_pages_info *info) {
    zassert_equal_ptr(dev, request_area.fa_dev);
    zassert_equal(offset, DEFY_BOOT_REQUEST_ADDRESS);
    *info = page_info;
    return page_error;
}

static int test_line_ctrl_get(const struct device *dev, uint32_t ctrl, uint32_t *value) {
    zassert_equal_ptr(dev, DEVICE_DT_GET(DT_NODELABEL(boot_test_cdc)));
    zassert_true(ctrl == UART_LINE_CTRL_BAUD_RATE || ctrl == UART_LINE_CTRL_DTR);
    *value = ctrl == UART_LINE_CTRL_BAUD_RATE ? current_baud : current_dtr;
    return line_error;
}

int cdc_acm_dte_rate_callback_set(const struct device *dev, cdc_dte_rate_callback_t callback) {
    zassert_equal_ptr(dev, DEVICE_DT_GET(DT_NODELABEL(boot_test_cdc)));
    rate_callback = callback;
    return 0;
}

static void test_reboot(int type) {
    zassert_equal(type, SYS_REBOOT_COLD);
    zassert_mem_equal(&flash_bytes[DEFY_BOOT_REQUEST_ADDRESS], expected_record,
                      sizeof(expected_record));
    zassert_true(boot_reads > 0);
    zassert_equal(closes, 2);
    ++reboots;
}

static void touch(void) {
    current_baud = DEFY_BOOT_TOUCH_BAUD;
    current_dtr = 0;
    rate_callback(DEVICE_DT_GET(DT_NODELABEL(boot_test_cdc)), current_baud);
}

static void wait_for_touch(void) {
    int64_t deadline = k_uptime_get() + 2000;
    while (atomic_get(&pending) && reboots == 0 && k_uptime_get() < deadline) {
        k_sleep(K_MSEC(1));
    }
    zassert_true(reboots || !atomic_get(&pending), "touch work did not finish");
}

static void before(void *fixture) {
    ARG_UNUSED(fixture);
    struct k_work_sync sync;
    k_work_cancel_delayable_sync(&bootloader_work, &sync);
    atomic_clear(&pending);
    defy_update_release(DEFY_UPDATE_NEURON);
    defy_update_release(DEFY_UPDATE_HALVES);
    defy_updater_init(&updater, &half_ops);
    half_opens = half_closes = 0;
    half_close_error = 0;
    updater_during_request = false;
    contending_open_status = 0;
    memset(flash_bytes, 0xa5, sizeof(flash_bytes));
    memset(&flash_bytes[DEFY_BOOT_ADDRESS], 0, DEFY_BOOT_LENGTH);
    memcpy(&flash_bytes[DEFY_BOOT_ADDRESS + DEFY_BOOT_LENGTH - 4], "\x38\xcf\xb3\x26", 4);
    boot_area = (struct flash_area){
        .fa_off = DEFY_BOOT_ADDRESS,
        .fa_size = DEFY_BOOT_REQUEST_ADDRESS - DEFY_BOOT_ADDRESS,
        .fa_dev = DEVICE_DT_GET(DT_NODELABEL(boot_test_cdc)),
    };
    request_area = (struct flash_area){
        .fa_off = DEFY_BOOT_REQUEST_ADDRESS,
        .fa_size = DEFY_BOOT_REQUEST_PAGE_SIZE,
        .fa_dev = boot_area.fa_dev,
    };
    page_info = (struct flash_pages_info){
        .start_offset = DEFY_BOOT_REQUEST_ADDRESS,
        .size = DEFY_BOOT_REQUEST_PAGE_SIZE,
    };
    open_error = page_error = boot_read_error = request_read_failure = 0;
    erase_error = write_error = line_error = 0;
    corrupt_readback = false;
    alignment = 4;
    boot_reads = request_reads = erases = writes = reboots = opens = closes = 0;
    current_baud = 115200;
    current_dtr = 0;
    rate_callback = NULL;
    zassert_ok(bootloader_init());
    zassert_not_null(rate_callback);
}

ZTEST(bootloader, test_verified_request_only_changes_dedicated_page) {
    uint32_t outside_crc = crc32_ieee(flash_bytes, DEFY_BOOT_REQUEST_ADDRESS);
    zassert_ok(defy_boot_request_write());
    zassert_equal(crc32_ieee(flash_bytes, DEFY_BOOT_REQUEST_ADDRESS), outside_crc);
    zassert_mem_equal(&flash_bytes[DEFY_BOOT_REQUEST_ADDRESS], expected_record,
                      sizeof(expected_record));
    for (size_t i = sizeof(expected_record); i < DEFY_BOOT_REQUEST_PAGE_SIZE; ++i) {
        zassert_equal(flash_bytes[DEFY_BOOT_REQUEST_ADDRESS + i], 0xff);
    }
    zassert_equal(boot_reads, DIV_ROUND_UP(DEFY_BOOT_LENGTH, 128));
    zassert_equal(erases, 1);
    zassert_equal(writes, 1);
    zassert_equal(request_reads, 2);
    zassert_equal(closes, 2);
}

ZTEST(bootloader, test_matching_request_does_not_erase_or_write_again) {
    memcpy(&flash_bytes[DEFY_BOOT_REQUEST_ADDRESS], expected_record, sizeof(expected_record));
    touch();
    wait_for_touch();
    zassert_equal(reboots, 1);
    zassert_equal(erases, 0);
    zassert_equal(writes, 0);
}

ZTEST(bootloader, test_unknown_bootloader_never_changes_flash) {
    flash_bytes[DEFY_BOOT_ADDRESS + 16] ^= 1;
    zassert_equal(defy_boot_request_write(), -ENOTSUP);
    zassert_equal(erases, 0);
    zassert_equal(writes, 0);
    zassert_equal(request_reads, 0);
}

ZTEST(bootloader, test_request_area_open_failure_closes_boot_area) {
    open_error = 2;
    zassert_equal(defy_boot_request_write(), -ENODEV);
    zassert_equal(closes, 1);
    zassert_equal(erases, 0);
}

ZTEST(bootloader, test_wrong_alignment_is_rejected) {
    alignment = 1;
    zassert_equal(defy_boot_request_write(), -EINVAL);
    zassert_equal(boot_reads, 0);
    zassert_equal(erases, 0);
}

ZTEST(bootloader, test_wrong_page_geometry_is_rejected) {
    page_info.start_offset -= DEFY_BOOT_REQUEST_PAGE_SIZE;
    zassert_equal(defy_boot_request_write(), -EINVAL);
    page_info.start_offset = DEFY_BOOT_REQUEST_ADDRESS;
    page_info.size *= 2;
    zassert_equal(defy_boot_request_write(), -EINVAL);
    zassert_equal(erases, 0);
}

ZTEST(bootloader, test_failures_before_the_erase_return_their_error) {
    /* The boot area's open, the page query, the bootloader's read and the request's read. */
    const struct {
        int *error;
        int value;
        int expected;
    } failures[] = {
        {&open_error, 1, -ENODEV},
        {&page_error, -EIO, -EIO},
        {&boot_read_error, -EIO, -EIO},
        {&request_read_failure, 1, -EIO},
    };
    for (size_t i = 0; i < ARRAY_SIZE(failures); i++) {
        before(NULL);
        *failures[i].error = failures[i].value;
        zassert_equal(defy_boot_request_write(), failures[i].expected);
        zassert_equal(erases, 0);
    }
}

ZTEST(bootloader, test_erase_failure_never_writes_or_resets) {
    erase_error = -EIO;
    touch();
    wait_for_touch();
    zassert_equal(erases, 1);
    zassert_equal(writes, 0);
    zassert_equal(reboots, 0);
}

ZTEST(bootloader, test_write_failure_never_resets) {
    write_error = -EIO;
    touch();
    wait_for_touch();
    zassert_equal(writes, 1);
    zassert_equal(reboots, 0);
    zassert_equal(request_reads, 1);
}

ZTEST(bootloader, test_readback_error_never_resets) {
    request_read_failure = 2;
    touch();
    wait_for_touch();
    zassert_equal(writes, 1);
    zassert_equal(reboots, 0);
}

ZTEST(bootloader, test_readback_mismatch_never_resets) {
    corrupt_readback = true;
    touch();
    wait_for_touch();
    zassert_equal(writes, 1);
    zassert_equal(reboots, 0);
}

ZTEST(bootloader, test_stable_closed_touch_is_deferred_and_resets) {
    touch();
    zassert_equal(erases, 0);
    zassert_equal(reboots, 0);
    k_sleep(K_MSEC(DEFY_BOOT_TOUCH_DELAY_MS / 2));
    zassert_equal(erases, 0);
    wait_for_touch();
    zassert_equal(erases, 1);
    zassert_equal(reboots, 1);
}

ZTEST(bootloader, test_duplicate_touch_is_coalesced) {
    touch();
    touch();
    touch();
    wait_for_touch();
    k_sleep(K_MSEC(DEFY_BOOT_TOUCH_DELAY_MS + 10));
    zassert_equal(erases, 1);
    zassert_equal(reboots, 1);
}

ZTEST(bootloader, test_ordinary_baud_rates_do_nothing) {
    rate_callback(DEVICE_DT_GET(DT_NODELABEL(boot_test_cdc)), 115200);
    rate_callback(DEVICE_DT_GET(DT_NODELABEL(boot_test_cdc)), 9600);
    zassert_false(atomic_get(&pending));
    zassert_equal(opens, 0);
}

ZTEST(bootloader, test_transient_touch_then_studio_baud_cancels) {
    touch();
    current_baud = 115200;
    rate_callback(DEVICE_DT_GET(DT_NODELABEL(boot_test_cdc)), current_baud);
    wait_for_touch();
    zassert_equal(opens, 0);
    zassert_equal(reboots, 0);
}

ZTEST(bootloader, test_open_1200_port_does_not_enter_bootloader) {
    touch();
    current_dtr = 1;
    wait_for_touch();
    zassert_equal(opens, 0);
    zassert_equal(reboots, 0);
}

ZTEST(bootloader, test_line_query_error_does_not_touch_flash) {
    line_error = -EIO;
    touch();
    wait_for_touch();
    zassert_equal(opens, 0);
    zassert_equal(reboots, 0);
}

ZTEST(bootloader, test_failed_attempt_allows_fresh_touch_retry) {
    erase_error = -EIO;
    touch();
    wait_for_touch();
    zassert_equal(reboots, 0);
    erase_error = 0;
    erases = opens = closes = 0;
    current_baud = 115200;
    rate_callback(DEVICE_DT_GET(DT_NODELABEL(boot_test_cdc)), current_baud);
    touch();
    wait_for_touch();
    zassert_equal(reboots, 1);
}

ZTEST(bootloader, test_active_half_update_rejects_bootloader_entry) {
    zassert_ok(half_request(2, 1));
    touch();
    wait_for_touch();
    zassert_equal(opens + erases + reboots, 0);
    zassert_true(updater.active);
    zassert_ok(half_request(7, 2));
    touch();
    wait_for_touch();
    zassert_equal(reboots, 1);
}

ZTEST(bootloader, test_half_open_during_request_cannot_acquire_maintenance) {
    updater_during_request = true;
    touch();
    wait_for_touch();
    zassert_equal(contending_open_status, -EBUSY);
    zassert_equal(half_opens + half_closes, 0);
    zassert_equal(reboots, 1);
    zassert_false(defy_update_claim(DEFY_UPDATE_HALVES));
}

ZTEST(bootloader, test_failed_request_releases_claim_for_half_update) {
    updater_during_request = true;
    write_error = -EIO;
    touch();
    wait_for_touch();
    zassert_equal(contending_open_status, -EBUSY);
    zassert_equal(half_opens + half_closes, 0);
    zassert_equal(reboots, 0);
    zassert_ok(half_request(2, 1));
    zassert_equal(half_opens, 1);
}

ZTEST(bootloader, test_pending_transport_recovery_blocks_bootloader_entry) {
    zassert_ok(half_request(2, 1));
    half_close_error = -EBUSY;
    zassert_equal(half_request(7, 2), -EBUSY);
    zassert_false(updater.active);
    zassert_true(updater.closing);
    touch();
    wait_for_touch();
    zassert_equal(opens + reboots, 0);
    half_close_error = 0;
    defy_updater_expire(&updater, k_uptime_get(), true);
    defy_updater_expire(&updater, updater.retry_at, true);
    zassert_false(updater.closing);
    touch();
    wait_for_touch();
    zassert_equal(reboots, 1);
}

ZTEST_SUITE(bootloader, NULL, NULL, before, NULL, NULL);
