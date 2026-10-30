# SPDX-License-Identifier: MIT

if(CONFIG_DEFY_BT_HCI_SPI)
  zephyr_library()
  zephyr_library_sources(defy_hci_spi.c)
  zephyr_library_sources_ifdef(CONFIG_DEFY_BATTERY_HCI defy_battery_hci.c)
  if(CONFIG_DEFY_BT_PERSISTENT_ID)
    zephyr_library_sources(defy_bt_identity.c)
    # Saves the identity before advertising and lets half sleep refuse it: defy_bt_identity.c.
    zephyr_link_libraries(-Wl,--wrap=bt_le_adv_start)
    defy_upstream_uses(${APPLICATION_SOURCE_DIR}/src/split/bluetooth/peripheral.c
                       "the advertising wrap in drivers/bluetooth/defy_bt_identity.c"
                       "bt_le_adv_start(")
  endif()
  if(CONFIG_DEFY_RADIO_INSTALL)
    if(NOT DEFINED DEFY_RADIO_IMAGE_SOURCE)
      message(FATAL_ERROR "DEFY_RADIO_IMAGE_SOURCE is required: build the radio first, as scripts/build does")
    endif()
    zephyr_library_sources(defy_radio_factory.c defy_radio_install.c ${DEFY_RADIO_IMAGE_SOURCE})
  endif()
endif()
