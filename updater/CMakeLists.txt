# SPDX-License-Identifier: MIT

if(CONFIG_DEFY_HALF_UPDATER)
  target_sources(app PRIVATE defy_half_bootloader.c defy_updater.c defy_updater_usb.c)
  if(CONFIG_ZMK_KEYMAP_SETTINGS_STORAGE)
    target_sources(app PRIVATE defy_keymap_changes.c)
  endif()
endif()
