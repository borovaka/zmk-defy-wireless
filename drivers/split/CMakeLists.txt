# SPDX-License-Identifier: MIT

if(CONFIG_DEFY_WIRED)
  target_sources(app PRIVATE defy_wired_protocol.c)
  if(CONFIG_ZMK_SPLIT_ROLE_CENTRAL)
    target_sources(app PRIVATE defy_wired_central.c)
  else()
    target_sources(app PRIVATE defy_wired_peripheral.c)
  endif()
endif()

if(CONFIG_DEFY_HYBRID)
  if(CONFIG_ZMK_SPLIT_ROLE_CENTRAL)
    target_sources(app PRIVATE defy_hybrid_central.c defy_hybrid_ble.c defy_split_pairing.c)
    # ZMK's split slot reservation, in a table the half updater can report and clear.
    zephyr_link_libraries(-Wl,--wrap=zmk_ble_put_peripheral_addr)
    # ZMK's split central, patched by zmk-split-central.patch.
    include(${CMAKE_CURRENT_LIST_DIR}/zmk_split_central.cmake)
    set(split_central_source ${APPLICATION_SOURCE_DIR}/src/split/bluetooth/central.c)
    set(patched_split_central ${PROJECT_BINARY_DIR}/defy/split_central.c)
    defy_split_central_source(${split_central_source} ${patched_split_central})
    set_property(SOURCE ${split_central_source} TARGET_DIRECTORY app PROPERTY HEADER_FILE_ONLY TRUE)
    target_sources(app PRIVATE ${patched_split_central})
  else()
    target_sources(app PRIVATE defy_hybrid_peripheral.c)
  endif()
endif()
