/* SPDX-License-Identifier: MIT */

#define DT_DRV_COMPAT dygma_defy_lighting

#include <zephyr/device.h>
#include <drivers/behavior.h>
#include <dt-bindings/dygma/defy_lighting.h>
#include <dygma/defy_lighting.h>

#if DT_HAS_COMPAT_STATUS_OKAY(DT_DRV_COMPAT)
static int lighting_pressed(struct zmk_behavior_binding *binding,
                             struct zmk_behavior_binding_event event) {
    ARG_UNUSED(event);
    if (IS_ENABLED(CONFIG_DEFY_LIGHTING)) {
        defy_lighting_command(binding->param1);
    }
    return ZMK_BEHAVIOR_OPAQUE;
}

static int lighting_released(struct zmk_behavior_binding *binding,
                              struct zmk_behavior_binding_event event) {
    ARG_UNUSED(binding);
    ARG_UNUSED(event);
    return ZMK_BEHAVIOR_OPAQUE;
}

#if IS_ENABLED(CONFIG_ZMK_BEHAVIOR_METADATA)
static const struct behavior_parameter_value_metadata commands[] = {
    {.display_name = "Toggle", .type = BEHAVIOR_PARAMETER_VALUE_TYPE_VALUE,
     .value = DEFY_LIGHTING_TOGGLE},
    {.display_name = "Brighter", .type = BEHAVIOR_PARAMETER_VALUE_TYPE_VALUE,
     .value = DEFY_LIGHTING_BRIGHTER},
    {.display_name = "Dimmer", .type = BEHAVIOR_PARAMETER_VALUE_TYPE_VALUE,
     .value = DEFY_LIGHTING_DIMMER},
    {.display_name = "Next look", .type = BEHAVIOR_PARAMETER_VALUE_TYPE_VALUE,
     .value = DEFY_LIGHTING_NEXT},
};

static const struct behavior_parameter_metadata_set command_set = {
    .param1_values = commands,
    .param1_values_len = ARRAY_SIZE(commands),
};

static const struct behavior_parameter_metadata metadata = {
    .sets_len = 1,
    .sets = &command_set,
};
#endif

static const struct behavior_driver_api lighting_behavior_api = {
    .locality = BEHAVIOR_LOCALITY_CENTRAL,
    .binding_pressed = lighting_pressed,
    .binding_released = lighting_released,
#if IS_ENABLED(CONFIG_ZMK_BEHAVIOR_METADATA)
    .parameter_metadata = &metadata,
#endif
};

BEHAVIOR_DT_INST_DEFINE(0, NULL, NULL, NULL, NULL, POST_KERNEL,
                        CONFIG_KERNEL_INIT_PRIORITY_DEFAULT, &lighting_behavior_api);
#endif
