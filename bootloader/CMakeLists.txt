# SPDX-License-Identifier: MIT

if(CONFIG_DEFY_NEURON_BOOTLOADER)
  target_sources(app PRIVATE defy_neuron_bootloader.c)
endif()

if(CONFIG_DEFY_UF2_DRIVE)
  target_sources(app PRIVATE behavior_defy_drive.c)
endif()

if(CONFIG_DEFY_LOADER_CONFIRM)
  target_sources(app PRIVATE defy_loader_confirm.c)
endif()

if(CONFIG_DEFY_NEURON_FATAL_RESET)
  target_sources(app PRIVATE defy_neuron_fatal.c)
endif()

if(CONFIG_DEFY_UPDATE_GUARD)
  target_sources(app PRIVATE defy_update_guard.c)
endif()
