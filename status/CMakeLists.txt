# SPDX-License-Identifier: MIT

if(CONFIG_DEFY_NEURON_STATUS_LED)
  target_sources(app PRIVATE defy_neuron_status.c)
endif()

if(CONFIG_DEFY_NEURON_DOCK)
  target_sources(app PRIVATE defy_neuron_dock.c)
endif()
