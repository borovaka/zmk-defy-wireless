/*
 * The loader's drive mode, reached when start.c did not start ZMK. The
 * Neuron shows a USB drive that takes a UF2 file of ZMK (disk.c), and a
 * serial port, and its status LED (led.c) shows what a reset would start.
 * Setting the port to 1200 baud with DTR low, and keeping it so for 250 ms,
 * writes Dygma's boot request and resets into n2_bootloader, as ZMK's Studio
 * port does (docs/software-bootloader.md).
 *
 * ZMK can start when its vector table is valid and start.c did not stop it
 * for failing to confirm its starts. The loader leaves drive mode by a reset
 * into ZMK once a UF2 file has been written; when VBUS goes away while ZMK
 * can start; after a fatal error; and, for a drive shown on request, after
 * IDLE_TIMEOUT_MIN minutes without a UF2 block. Otherwise a VBUS loss powers
 * the Neuron down to System OFF, from which a USB connection wakes it into
 * the drive again. A failed setup, the disk's check included, leaves the
 * same way, except that while VBUS stays and ZMK cannot start, the loader
 * waits for VBUS to go: a reset would only fail again.
 *
 * SPDX-License-Identifier: MIT
 */

#include <errno.h>

#include <zephyr/device.h>
#include <zephyr/drivers/uart.h>
#include <zephyr/fatal.h>
#include <zephyr/kernel.h>
#include <zephyr/storage/flash_map.h>
#include <zephyr/sys/poweroff.h>
#include <zephyr/sys/reboot.h>
#include <zephyr/usb/usbd.h>
#include <hal/nrf_power.h>

#include <dygma/defy_boot_request.h>
#include <dygma/defy_loader.h>

#include "boot_mode.h"
#include "layout.h"
#include "loader.h"

/* The loader runs from loader_partition (app.overlay), and layout.h agrees with the
 * devicetree, which ZMK's board shares. */
BUILD_ASSERT(DT_SAME_NODE(DT_CHOSEN(zephyr_code_partition), DT_NODELABEL(loader_partition)));
BUILD_ASSERT(CONFIG_FLASH_LOAD_OFFSET == FIXED_PARTITION_OFFSET(loader_partition) &&
             CONFIG_FLASH_LOAD_SIZE == FIXED_PARTITION_SIZE(loader_partition));
BUILD_ASSERT(FIXED_PARTITION_OFFSET(loader_partition) + FIXED_PARTITION_SIZE(loader_partition) ==
             DEFY_LOADER_ZMK_START);
BUILD_ASSERT(FIXED_PARTITION_OFFSET(code_partition) == DEFY_LOADER_ZMK_START);
BUILD_ASSERT(FIXED_PARTITION_OFFSET(code_partition) + FIXED_PARTITION_SIZE(code_partition) ==
             DEFY_LOADER_ZMK_END);
BUILD_ASSERT(DT_REG_ADDR(DT_CHOSEN(zephyr_sram)) == DEFY_LOADER_RAM_START &&
             DT_REG_ADDR(DT_CHOSEN(zephyr_sram)) + DT_REG_SIZE(DT_CHOSEN(zephyr_sram)) ==
                 DEFY_LOADER_RAM_END);
BUILD_ASSERT(DT_PROP(DT_CHOSEN(zephyr_flash), erase_block_size) == DEFY_LOADER_PAGE_SIZE);

/* After a complete file, the host has this long to finish its own writes. */
#define COPIED_RESET_DELAY_MS 500
/* A drive shown on request, with a valid ZMK, goes back to ZMK after this long without
 * a UF2 block: after a stray press of the key, on a charger, or once the host has
 * ejected the drive. */
#define IDLE_TIMEOUT_MIN 5
/* bMaxPower, in 2 mA units, as in ZMK's build. */
#define MAX_POWER 50
/* How often a failed setup checks whether VBUS has gone. */
#define VBUS_POLL_MS 100

USBD_DEVICE_DEFINE(loader_usbd, DEVICE_DT_GET(DT_NODELABEL(zephyr_udc0)),
                   CONFIG_DEFY_LOADER_USB_VID, CONFIG_DEFY_LOADER_USB_PID);
USBD_DESC_LANG_DEFINE(loader_language);
USBD_DESC_MANUFACTURER_DEFINE(loader_manufacturer, CONFIG_DEFY_LOADER_USB_MANUFACTURER);
USBD_DESC_PRODUCT_DEFINE(loader_product, CONFIG_DEFY_LOADER_USB_PRODUCT);
/* ZMK's serial number: hwinfo's eight-byte device ID as 16 uppercase hex digits. */
USBD_DESC_SERIAL_NUMBER_DEFINE(loader_serial_number);
USBD_CONFIGURATION_DEFINE(loader_configuration, USB_SCD_SELF_POWERED, MAX_POWER, NULL);

static const struct device *const serial = DEVICE_DT_GET(DT_NODELABEL(loader_serial));

bool defy_loader_zmk_startable(void) {
    const volatile uint32_t *vectors = (const volatile uint32_t *)DEFY_LOADER_ZMK_START;
    return defy_loader_zmk_valid(vectors[0], vectors[1]) &&
           defy_loader_starts(nrf_power_gpregret_get(NRF_POWER, DEFY_LOADER_GPREGRET)) <
               DEFY_LOADER_START_LIMIT;
}

/* With GPREGRET cleared, start.c starts ZMK after the reset if it is valid, with a fresh
 * count of its starts. */
static FUNC_NORETURN void restart(void) {
    nrf_power_gpregret_set(NRF_POWER, DEFY_LOADER_GPREGRET, 0);
    sys_reboot(SYS_REBOOT_COLD);
}

/*
 * Into ZMK if it can start, otherwise to System OFF. Only a USB connection wakes the
 * Neuron from System OFF, and VBUS that is still there, or already back, would never
 * bring one, so then a reset shows the drive again at once.
 */
static FUNC_NORETURN void leave(void) {
    if (defy_loader_zmk_startable()) {
        restart();
    }
    (void)usbd_disable(&loader_usbd);
    if (nrf_power_usbregstatus_vbusdet_get(NRF_POWER)) {
        sys_reboot(SYS_REBOOT_COLD);
    }
    defy_loader_led_off();
    sys_poweroff();
}

/* The drive could not be set up. A reset would start the loader into the same
 * failure, so while ZMK cannot start, the LED shows red until VBUS goes and
 * leave() powers the Neuron down; the next USB connection tries again. */
static FUNC_NORETURN void setup_failed(void) {
    while (!defy_loader_zmk_startable() && nrf_power_usbregstatus_vbusdet_get(NRF_POWER)) {
        k_sleep(K_MSEC(VBUS_POLL_MS));
    }
    leave();
}

/* A fatal error restarts the Neuron, which starts ZMK if it is valid, where Zephyr's
 * default handler would halt it. */
void k_sys_fatal_error_handler(unsigned int reason, const struct arch_esf *esf) {
    ARG_UNUSED(reason);
    ARG_UNUSED(esf);
    restart();
}

static void copied(struct k_work *work) {
    ARG_UNUSED(work);
    restart();
}

static K_WORK_DELAYABLE_DEFINE(copied_work, copied);

void defy_loader_copied(void) { k_work_schedule(&copied_work, K_MSEC(COPIED_RESET_DELAY_MS)); }

static void idle(struct k_work *work) {
    ARG_UNUSED(work);
    if (!defy_loader_disk_written() && defy_loader_zmk_startable()) {
        restart();
    }
}

static K_WORK_DELAYABLE_DEFINE(idle_work, idle);

/* n2_bootloader starts the loader again afterwards, and GPREGRET cleared gives the ZMK
 * it then finds a fresh count of its starts. */
static void touch_check(struct k_work *work) {
    ARG_UNUSED(work);
    uint32_t baud;
    uint32_t dtr;
    if (!uart_line_ctrl_get(serial, UART_LINE_CTRL_BAUD_RATE, &baud) &&
        !uart_line_ctrl_get(serial, UART_LINE_CTRL_DTR, &dtr) && baud == DEFY_BOOT_TOUCH_BAUD &&
        dtr == 0 && !defy_boot_request_write()) {
        restart();
    }
}

static K_WORK_DELAYABLE_DEFINE(touch_work, touch_check);

/* The stack delivers these on the system work queue. */
static void usb_message(struct usbd_context *const context, const struct usbd_msg *const msg) {
    uint32_t baud;
    switch (msg->type) {
    case USBD_MSG_VBUS_READY:
        /* main() may have enabled the stack already. */
        (void)usbd_enable(context);
        break;
    case USBD_MSG_VBUS_REMOVED:
        leave();
    case USBD_MSG_CDC_ACM_LINE_CODING:
        if (msg->dev == serial && !uart_line_ctrl_get(serial, UART_LINE_CTRL_BAUD_RATE, &baud) &&
            baud == DEFY_BOOT_TOUCH_BAUD) {
            k_work_schedule(&touch_work, K_MSEC(DEFY_BOOT_TOUCH_DELAY_MS));
        }
        break;
    default:
        break;
    }
}

static int usb_init(void) {
    int err = usbd_add_descriptor(&loader_usbd, &loader_language);
    if (!err) {
        err = usbd_add_descriptor(&loader_usbd, &loader_manufacturer);
    }
    if (!err) {
        err = usbd_add_descriptor(&loader_usbd, &loader_product);
    }
    if (!err) {
        err = usbd_add_descriptor(&loader_usbd, &loader_serial_number);
    }
    if (!err) {
        err = usbd_add_configuration(&loader_usbd, USBD_SPEED_FS, &loader_configuration);
    }
    if (!err) {
        err = usbd_register_all_classes(&loader_usbd, USBD_SPEED_FS, 1, NULL);
    }
    if (!err) {
        /* CDC ACM has two interfaces under an interface association descriptor. */
        err = usbd_device_set_code_triple(&loader_usbd, USBD_SPEED_FS, USB_BCC_MISCELLANEOUS,
                                          0x02, 0x01);
    }
    if (!err) {
        usbd_self_powered(&loader_usbd, true);
        err = usbd_msg_register_cb(&loader_usbd, usb_message);
    }
    return err ? err : usbd_init(&loader_usbd);
}

int main(void) {
    /* A startable ZMK means the drive was requested: go back to ZMK after IDLE_TIMEOUT_MIN
     * without a UF2 block. */
    if (defy_loader_zmk_startable()) {
        k_work_schedule(&idle_work, K_MINUTES(IDLE_TIMEOUT_MIN));
    }
    if (!nrf_power_usbregstatus_vbusdet_get(NRF_POWER)) {
        leave();
    }
    defy_loader_led_init();
    if (defy_loader_disk_init() || usb_init()) {
        setup_failed();
    }
    /*
     * VBUS is present, so enable the stack now: USBD_MSG_VBUS_READY comes from the
     * USB-detected event, which an earlier stage may already have taken.
     */
    int err = usbd_enable(&loader_usbd);
    if (err && err != -EALREADY) {
        setup_failed();
    }
    return 0;
}
