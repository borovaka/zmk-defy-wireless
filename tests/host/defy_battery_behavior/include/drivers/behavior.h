/* SPDX-License-Identifier: MIT */

#pragma once

#include <stdint.h>

struct zmk_behavior_binding { uint32_t param1; };
struct zmk_behavior_binding_event { uint32_t position; };
enum behavior_locality { BEHAVIOR_LOCALITY_CENTRAL, BEHAVIOR_LOCALITY_GLOBAL };
struct behavior_driver_api {
    enum behavior_locality locality;
    int (*binding_pressed)(struct zmk_behavior_binding *, struct zmk_behavior_binding_event);
    int (*binding_released)(struct zmk_behavior_binding *, struct zmk_behavior_binding_event);
};

#define ZMK_BEHAVIOR_OPAQUE 0
#define BEHAVIOR_DT_INST_DEFINE(...)
