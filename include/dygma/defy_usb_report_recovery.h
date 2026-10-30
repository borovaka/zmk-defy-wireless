/*
 * The halves' keys for the USB report recovery
 * (drivers/usb/defy_usb_report_recovery.c), which lets only a newly pressed
 * key wake a suspended USB host. The hybrid central passes each half's key
 * bitmap from the link it uses, and resets them when it stops or pauses.
 *
 * SPDX-License-Identifier: MIT
 */

#ifndef DYGMA_DEFY_USB_REPORT_RECOVERY_H
#define DYGMA_DEFY_USB_REPORT_RECOVERY_H

#include <zephyr/sys/util.h>
#include <dygma/defy_wired_protocol.h>

#if IS_ENABLED(CONFIG_DEFY_USB_REPORT_RECOVERY)
void defy_usb_input_snapshot(uint8_t side, const uint8_t bitmap[DEFY_WIRED_BITMAP_SIZE],
                             int64_t received);
void defy_usb_input_reset(void);
#else
static inline void defy_usb_input_snapshot(uint8_t side,
                                           const uint8_t bitmap[DEFY_WIRED_BITMAP_SIZE],
                                           int64_t received) {
    ARG_UNUSED(side);
    ARG_UNUSED(bitmap);
    ARG_UNUSED(received);
}
static inline void defy_usb_input_reset(void) {}
#endif

#endif
