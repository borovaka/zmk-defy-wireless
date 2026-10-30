/*
 * &defy_battery, the battery display: each half shows its battery, sent over its cable when
 * it has one, else over Bluetooth.
 *
 * SPDX-License-Identifier: MIT
 */

#define DT_DRV_COMPAT dygma_defy_battery_display

#include <zephyr/device.h>
#include <drivers/behavior.h>

#include <dygma/defy_control.h>
#include <dygma/defy_wired_protocol.h>

#if DT_HAS_COMPAT_STATUS_OKAY(DT_DRV_COMPAT)

static int battery_pressed(struct zmk_behavior_binding *binding,
                           struct zmk_behavior_binding_event event) {
    ARG_UNUSED(binding);
    ARG_UNUSED(event);
    if (!IS_ENABLED(CONFIG_DEFY_BATTERY)) {
        return ZMK_BEHAVIOR_OPAQUE;
    }
    uint8_t wireless = 0;
    for (uint8_t side = DEFY_WIRED_LEFT; side <= DEFY_WIRED_RIGHT; side++) {
        if (defy_wired_battery_show(side) < 0) {
            wireless |= BIT(side);
        }
    }
    if (wireless) {
        defy_control_ble_battery_show(wireless);
    }
    return ZMK_BEHAVIOR_OPAQUE;
}

static int battery_released(struct zmk_behavior_binding *binding,
                            struct zmk_behavior_binding_event event) {
    ARG_UNUSED(binding);
    ARG_UNUSED(event);
    return ZMK_BEHAVIOR_OPAQUE;
}

static const struct behavior_driver_api battery_behavior_api = {
    .locality = BEHAVIOR_LOCALITY_CENTRAL,
    .binding_pressed = battery_pressed,
    .binding_released = battery_released,
#if IS_ENABLED(CONFIG_ZMK_BEHAVIOR_METADATA)
    .get_parameter_metadata = zmk_behavior_get_empty_param_metadata,
#endif
};

BEHAVIOR_DT_INST_DEFINE(0, NULL, NULL, NULL, NULL, POST_KERNEL,
                        CONFIG_KERNEL_INIT_PRIORITY_DEFAULT, &battery_behavior_api);

#endif
