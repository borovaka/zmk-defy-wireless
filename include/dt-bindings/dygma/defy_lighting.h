/*
 * The Defy's lighting, for a keymap: the commands &defy_lighting takes, and
 * colours for its looks, &defy_layer_lighting and &defy_key_lighting. A colour
 * is given at full strength, as 0xRRGGBB, and the lighting's brightness scales
 * it. See docs/lighting.md.
 *
 * SPDX-License-Identifier: MIT
 */

#pragma once

/* &defy_lighting's commands. */
#define DEFY_LIGHTING_TOGGLE 0
#define DEFY_LIGHTING_BRIGHTER 1
#define DEFY_LIGHTING_DIMMER 2
#define DEFY_LIGHTING_NEXT 3

/* A colour from its red, green and blue, 0 to 255 each. */
#define DEFY_RGB(r, g, b) ((((r) & 0xff) << 16) | (((g) & 0xff) << 8) | ((b) & 0xff))

/*
 * The part of a colour common to red, green and blue lights each LED's white
 * LED, so the whites below light the white LEDs, with a little red and green
 * for the warm one.
 */
#define DEFY_OFF DEFY_RGB(0, 0, 0)
#define DEFY_WHITE DEFY_RGB(255, 255, 255)
#define DEFY_WARM_WHITE DEFY_RGB(255, 220, 180)
#define DEFY_RED DEFY_RGB(255, 0, 0)
#define DEFY_ORANGE DEFY_RGB(255, 80, 0)
#define DEFY_YELLOW DEFY_RGB(255, 200, 0)
#define DEFY_GREEN DEFY_RGB(0, 255, 0)
#define DEFY_CYAN DEFY_RGB(0, 255, 255)
#define DEFY_BLUE DEFY_RGB(0, 0, 255)
#define DEFY_PURPLE DEFY_RGB(128, 0, 255)
#define DEFY_PINK DEFY_RGB(255, 64, 128)
