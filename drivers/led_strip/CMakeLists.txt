# SPDX-License-Identifier: MIT

if(CONFIG_IS31FL3743B OR CONFIG_DEFY_LEDS OR CONFIG_DEFY_WS2812_PIO)
  zephyr_library()
  zephyr_library_sources_ifdef(CONFIG_IS31FL3743B is31fl3743b.c)
  zephyr_library_sources_ifdef(CONFIG_DEFY_LEDS defy_leds.c)
  zephyr_library_sources_ifdef(CONFIG_DEFY_WS2812_PIO ws2812_rpi_pico_pio.c)
endif()
