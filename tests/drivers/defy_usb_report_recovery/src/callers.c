/* SPDX-License-Identifier: MIT */

#include <zephyr/usb/usb_device.h>
#include <zmk/usb_hid.h>

int test_send_keyboard(void) { return zmk_usb_hid_send_keyboard_report(); }
int test_send_consumer(void) { return zmk_usb_hid_send_consumer_report(); }
int test_send_mouse(void) { return zmk_usb_hid_send_mouse_report(); }
int test_usb_enable(usb_dc_status_callback callback) { return usb_enable(callback); }

int fake_wakeup_request(void);
int usb_wakeup_request(void) { return fake_wakeup_request(); }
