# SPDX-License-Identifier: MIT

if(CONFIG_DEFY_HYBRID AND CONFIG_ZMK_SPLIT_ROLE_CENTRAL)
  target_sources(app PRIVATE behavior_defy_lighting.c behavior_defy_profiles.c)
endif()

if(CONFIG_DEFY_LIGHTING)
  if(CONFIG_ZMK_SPLIT_ROLE_CENTRAL)
    target_sources(app PRIVATE defy_profile_chooser.c defy_feedback_protocol.c
                               defy_feedback_central.c defy_lighting_protocol.c
                               defy_lighting_central.c)
    zephyr_link_libraries(-Wl,--wrap=zmk_ble_prof_next
                         -Wl,--wrap=zmk_ble_prof_prev
                         -Wl,--wrap=zmk_ble_prof_select
                         -Wl,--wrap=zmk_endpoint_set_preferred_transport
                         -Wl,--wrap=zmk_endpoint_toggle_preferred_transport)
    # A wrap reaches only calls from other objects: here, ZMK's &bt and &out.
    defy_upstream_uses(${APPLICATION_SOURCE_DIR}/src/behaviors/behavior_bt.c
                       "the host overlay's wraps in lighting/defy_feedback_central.c"
                       "zmk_ble_prof_next(" "zmk_ble_prof_prev(" "zmk_ble_prof_select(")
    defy_upstream_uses(${APPLICATION_SOURCE_DIR}/src/behaviors/behavior_outputs.c
                       "the host overlay's wraps in lighting/defy_feedback_central.c"
                       "zmk_endpoint_set_preferred_transport("
                       "zmk_endpoint_toggle_preferred_transport(")
  else()
    target_sources(app PRIVATE defy_feedback_protocol.c defy_lighting_protocol.c
                               defy_lighting_peripheral.c)
  endif()
endif()

# A half's LEDs: the start-up display and the displays stacked over it.
if(CONFIG_DEFY_HALF_LEDS)
  target_sources(app PRIVATE defy_half_leds.c defy_startup_leds.c)
  target_sources_ifdef(CONFIG_DEFY_RADIO_INSTALL app PRIVATE defy_radio_install_leds.c)
endif()
