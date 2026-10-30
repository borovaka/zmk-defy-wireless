/*
 * A 1200-baud touch on a CDC ACM port other than the half updater's restarts
 * the Neuron into n2_bootloader; docs/software-bootloader.md.
 *
 * SPDX-License-Identifier: MIT
 */

#include <errno.h>

#include <zephyr/device.h>
#include <zephyr/drivers/uart.h>
#include <zephyr/drivers/uart/cdc_acm.h>
#include <zephyr/init.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/atomic.h>
#include <zephyr/sys/reboot.h>
#include <zephyr/sys/util.h>
#include <dygma/defy_boot_request.h>
#include <dygma/defy_loader.h>
#include <dygma/defy_update_guard.h>

LOG_MODULE_REGISTER(defy_bootloader, CONFIG_ZMK_LOG_LEVEL);

static atomic_t pending;
static const struct device *touch_uart;

static void enter_bootloader(struct k_work *work) {
    ARG_UNUSED(work);
    uint32_t baud;
    uint32_t dtr;
    int err = uart_line_ctrl_get(touch_uart, UART_LINE_CTRL_BAUD_RATE, &baud);
    if (!err) {
        err = uart_line_ctrl_get(touch_uart, UART_LINE_CTRL_DTR, &dtr);
    }
    if (err || baud != DEFY_BOOT_TOUCH_BAUD || dtr != 0 ||
        !defy_update_claim(DEFY_UPDATE_NEURON)) {
        atomic_clear(&pending);
        return;
    }
    err = defy_boot_request_write();
    if (!err) {
        /* A restart on request is not a failed start for the UF2 loader's count. */
        if (IS_ENABLED(CONFIG_DEFY_LOADER_CONFIRM)) {
            defy_loader_confirm();
        }
        sys_reboot(SYS_REBOOT_COLD);
        return;
    }
    LOG_ERR("Bootloader request failed: %d", err);
    defy_update_release(DEFY_UPDATE_NEURON);
    atomic_clear(&pending);
}

static K_WORK_DELAYABLE_DEFINE(bootloader_work, enter_bootloader);

static void baud_changed(const struct device *dev, uint32_t baud) {
    if (baud != DEFY_BOOT_TOUCH_BAUD || !atomic_cas(&pending, 0, 1)) {
        return;
    }
    touch_uart = dev;
    if (k_work_reschedule(&bootloader_work, K_MSEC(DEFY_BOOT_TOUCH_DELAY_MS)) < 0) {
        atomic_clear(&pending);
    }
}

#define CDC_ACM_PORT(node) DEVICE_DT_GET(node),
static const struct device *const cdc_acm_ports[] = {
    DT_FOREACH_STATUS_OKAY(zephyr_cdc_acm_uart, CDC_ACM_PORT)};

static int bootloader_init(void) {
    const struct device *updater =
        COND_CODE_1(DT_HAS_CHOSEN(dygma_half_updater_uart),
                    (DEVICE_DT_GET(DT_CHOSEN(dygma_half_updater_uart))), (NULL));

    ARRAY_FOR_EACH(cdc_acm_ports, i) {
        if (cdc_acm_ports[i] == updater) {
            continue;
        }
        if (!device_is_ready(cdc_acm_ports[i])) {
            return -ENODEV;
        }
        int err = cdc_acm_dte_rate_callback_set(cdc_acm_ports[i], baud_changed);
        if (err) {
            return err;
        }
    }
    return 0;
}

SYS_INIT(bootloader_init, APPLICATION, CONFIG_APPLICATION_INIT_PRIORITY);
