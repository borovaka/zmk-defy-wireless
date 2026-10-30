# SPDX-License-Identifier: MIT

if(CONFIG_DEFY_STUDIO_USB_PRIORITY)
  target_sources(app PRIVATE defy_studio_transport.c)
  # Limit the selection override to Studio; HID keeps its selected output.
  set(rpc ${APPLICATION_SOURCE_DIR}/src/studio/rpc.c)
  set_property(SOURCE ${rpc} TARGET_DIRECTORY app APPEND PROPERTY COMPILE_DEFINITIONS
               zmk_endpoint_get_selected=defy_studio_get_selected_endpoint)
  # defy_studio_transport.c also calls the listener with an endpoint change.
  defy_upstream_uses(${rpc} "the Studio rename and listener call in studio/"
                     "zmk_endpoint_get_selected(" "ZMK_SUBSCRIPTION(studio_rpc, zmk_endpoint_changed)")
  if(CONFIG_ZMK_STUDIO_TRANSPORT_BLE)
    set(gatt ${APPLICATION_SOURCE_DIR}/src/studio/gatt_rpc_transport.c)
    set_property(SOURCE ${gatt} TARGET_DIRECTORY app APPEND PROPERTY COMPILE_DEFINITIONS
                 zmk_studio_core_lock=defy_studio_ble_disconnected)
    defy_upstream_uses(${gatt} "the Studio rename in studio/CMakeLists.txt" "zmk_studio_core_lock(")
  endif()
endif()
