# SPDX-License-Identifier: MIT

if(CONFIG_DEFY_USB_REPORT_RECOVERY)
  target_sources(app PRIVATE defy_usb_report_recovery.c)
  zephyr_link_libraries(
    -Wl,--wrap=zmk_usb_hid_send_keyboard_report
    -Wl,--wrap=zmk_usb_hid_send_consumer_report
    -Wl,--wrap=usb_wakeup_request
    -Wl,--wrap=usb_enable)
  set(use "the USB report recovery's wraps in drivers/usb/defy_usb_report_recovery.c")
  defy_upstream_uses(${APPLICATION_SOURCE_DIR}/src/endpoints.c "${use}"
                     "zmk_usb_hid_send_keyboard_report(" "zmk_usb_hid_send_consumer_report(")
  defy_upstream_uses(${APPLICATION_SOURCE_DIR}/src/usb_hid.c "${use}" "usb_wakeup_request(")
  defy_upstream_uses(${APPLICATION_SOURCE_DIR}/src/usb.c "${use}" "usb_enable(")
  if(CONFIG_ZMK_POINTING)
    zephyr_link_libraries(-Wl,--wrap=zmk_usb_hid_send_mouse_report)
    defy_upstream_uses(${APPLICATION_SOURCE_DIR}/src/endpoints.c "${use}"
                       "zmk_usb_hid_send_mouse_report(")
  endif()
endif()
