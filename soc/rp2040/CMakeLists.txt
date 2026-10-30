# SPDX-License-Identifier: MIT

zephyr_library()
# Empty on RP2040 boards other than the Defy halves.
zephyr_library_property(ALLOW_EMPTY TRUE)
zephyr_library_sources_ifdef(CONFIG_POWEROFF poweroff.c)
zephyr_library_sources_ifdef(CONFIG_DEFY_RP2040_FACTORY_HANDOFF factory_handoff.c)
