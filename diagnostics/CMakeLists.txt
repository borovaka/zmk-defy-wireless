# SPDX-License-Identifier: MIT

# Into ZMK's app target, whose include directory is private to it.
target_sources_ifdef(CONFIG_DEFY_FATAL_LEDS app PRIVATE defy_fatal_leds.c)
target_sources_ifdef(CONFIG_DEFY_FATAL_RECOVERY app PRIVATE defy_fatal_recovery.c)
target_sources_ifdef(CONFIG_DEFY_KEY_TRACE app PRIVATE defy_key_trace.c)
