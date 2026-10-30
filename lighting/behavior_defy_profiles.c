/* SPDX-License-Identifier: MIT */

#define DT_DRV_COMPAT dygma_defy_profile_chooser

#include <zephyr/device.h>
#include <drivers/behavior.h>
#include <dygma/defy_profile_chooser.h>

#if DT_HAS_COMPAT_STATUS_OKAY(DT_DRV_COMPAT)
static int profiles_pressed(struct zmk_behavior_binding *binding,
                            struct zmk_behavior_binding_event event) {
    ARG_UNUSED(binding);
    ARG_UNUSED(event);
    if (IS_ENABLED(CONFIG_DEFY_LIGHTING)) {
        defy_profile_chooser_toggle();
    }
    return ZMK_BEHAVIOR_OPAQUE;
}

static int profiles_released(struct zmk_behavior_binding *binding,
                             struct zmk_behavior_binding_event event) {
    ARG_UNUSED(binding);
    ARG_UNUSED(event);
    return ZMK_BEHAVIOR_OPAQUE;
}

static const struct behavior_driver_api profiles_api = {
    .locality = BEHAVIOR_LOCALITY_CENTRAL,
    .binding_pressed = profiles_pressed,
    .binding_released = profiles_released,
#if IS_ENABLED(CONFIG_ZMK_BEHAVIOR_METADATA)
    .get_parameter_metadata = zmk_behavior_get_empty_param_metadata,
#endif
};

BEHAVIOR_DT_INST_DEFINE(0, NULL, NULL, NULL, NULL, POST_KERNEL,
                        CONFIG_KERNEL_INIT_PRIORITY_DEFAULT, &profiles_api);
#endif
