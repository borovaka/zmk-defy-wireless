/*
 * Studio replies on native_sim.
 *
 * Studio sends its replies through the RPC transport of the endpoint it sees
 * selected. ZMK's UART transport, which reads the Studio port from boot, is
 * the USB one, and native_sim has no USB. With DEFY_STUDIO_USB_PRIORITY, which
 * native_sim.conf sets, Studio sees USB while a USB host is ready, so this
 * reports one. Keys go to Bluetooth profile 3 meanwhile: Studio stays on USB
 * whichever output carries them.
 *
 * SPDX-License-Identifier: MIT
 */

#include <zmk/endpoints.h>
#include <zmk/usb.h>

bool zmk_usb_is_hid_ready(void) { return true; }

struct zmk_endpoint_instance __wrap_zmk_endpoint_get_selected(void) {
    return (struct zmk_endpoint_instance){
        .transport = ZMK_TRANSPORT_BLE,
        .ble.profile_index = 3,
    };
}
