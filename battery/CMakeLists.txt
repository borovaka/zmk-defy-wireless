# SPDX-License-Identifier: MIT

if(CONFIG_DEFY_HYBRID AND CONFIG_ZMK_SPLIT_ROLE_CENTRAL)
  target_sources(app PRIVATE behavior_defy_battery.c)
endif()

if(CONFIG_DEFY_BATTERY)
  target_sources(app PRIVATE defy_battery_protocol.c)
  if(CONFIG_ZMK_SPLIT_ROLE_CENTRAL)
    target_sources(app PRIVATE defy_battery_central.c defy_battery_host.c)
  endif()
endif()

# The control channel, which carries the battery telemetry, the lighting and
# the version report.
if(CONFIG_DEFY_CONTROL)
  if(CONFIG_ZMK_SPLIT_ROLE_CENTRAL)
    target_sources(app PRIVATE defy_control_ble.c)
  else()
    target_sources(app PRIVATE defy_control_peripheral.c)
  endif()
endif()

# The version report: a half's own, and the Neuron's records of both halves'.
if(CONFIG_DEFY_HALF_VERSION)
  target_sources(app PRIVATE defy_half_version_protocol.c)
  if(CONFIG_ZMK_SPLIT_ROLE_CENTRAL)
    target_sources(app PRIVATE defy_half_versions.c)
  else()
    target_sources(app PRIVATE defy_half_version_local.c)
  endif()
endif()
