# SPDX-License-Identifier: MIT
# Test-only: lets Studio answer on native_sim. See native_sim_usb.c.
if(CONFIG_ZMK_STUDIO_TRANSPORT_UART AND CONFIG_ARCH_POSIX)
  target_sources(app PRIVATE native_sim_usb.c
                             ${APPLICATION_SOURCE_DIR}/src/events/usb_conn_state_changed.c)
  zephyr_link_libraries(-Wl,--wrap=zmk_endpoint_get_selected)
endif()
