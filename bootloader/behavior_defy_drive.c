/*
 * &defy_drive: restarts the Neuron into the UF2 loader's drive
 * (docs/uf2-loader.md). ZMK's &bootloader runs on the side whose key was
 * pressed, and every Defy key is on a half, so it restarts that half instead.
 * This behaviour runs on the Neuron: it sets the bootloader boot mode, which
 * ZMK's magic mapper writes into GPREGRET as DEFY_LOADER_DRIVE_MAGIC, and
 * restarts. It does nothing while a half update or a bootloader entry holds
 * the Neuron.
 *
 * SPDX-License-Identifier: MIT
 */

#define DT_DRV_COMPAT dygma_defy_uf2_drive

#include <zephyr/device.h>
#include <zephyr/logging/log.h>
#include <zephyr/retention/bootmode.h>
#include <zephyr/sys/reboot.h>
#include <drivers/behavior.h>
#include <dygma/defy_loader.h>
#include <dygma/defy_update_guard.h>

LOG_MODULE_DECLARE(zmk, CONFIG_ZMK_LOG_LEVEL);

/* GPREGRET is the one byte at the start of gpregret1. */
BUILD_ASSERT(CONFIG_ZMK_BOOTMODE_BOOTLOADER_MAGIC_VALUE == DEFY_LOADER_DRIVE_MAGIC,
             "ZMK's bootloader magic must be the UF2 loader's drive request");
BUILD_ASSERT(DT_SAME_NODE(DT_PARENT(DT_CHOSEN(zmk_magic_boot_mode)), DT_NODELABEL(gpregret1)) &&
                 DT_REG_ADDR(DT_CHOSEN(zmk_magic_boot_mode)) == 0 &&
                 DT_REG_SIZE(DT_CHOSEN(zmk_magic_boot_mode)) == 1,
             "ZMK's boot-mode magic must go into GPREGRET");

#if DT_HAS_COMPAT_STATUS_OKAY(DT_DRV_COMPAT)
static int drive_pressed(struct zmk_behavior_binding *binding,
                         struct zmk_behavior_binding_event event) {
    ARG_UNUSED(binding);
    ARG_UNUSED(event);
    if (!defy_update_claim(DEFY_UPDATE_NEURON)) {
        LOG_WRN("An update holds the Neuron; not showing the drive");
        return ZMK_BEHAVIOR_OPAQUE;
    }
    int err = bootmode_set(BOOT_MODE_TYPE_BOOTLOADER);
    if (err < 0) {
        LOG_ERR("Failed to set the boot mode for the drive (%d)", err);
        defy_update_release(DEFY_UPDATE_NEURON);
        return ZMK_BEHAVIOR_OPAQUE;
    }
    sys_reboot(SYS_REBOOT_WARM);
    return ZMK_BEHAVIOR_OPAQUE;
}

static int drive_released(struct zmk_behavior_binding *binding,
                          struct zmk_behavior_binding_event event) {
    ARG_UNUSED(binding);
    ARG_UNUSED(event);
    return ZMK_BEHAVIOR_OPAQUE;
}

static const struct behavior_driver_api drive_behavior_api = {
    .locality = BEHAVIOR_LOCALITY_CENTRAL,
    .binding_pressed = drive_pressed,
    .binding_released = drive_released,
#if IS_ENABLED(CONFIG_ZMK_BEHAVIOR_METADATA)
    .get_parameter_metadata = zmk_behavior_get_empty_param_metadata,
#endif
};

BEHAVIOR_DT_INST_DEFINE(0, NULL, NULL, NULL, NULL, POST_KERNEL,
                        CONFIG_KERNEL_INIT_PRIORITY_DEFAULT, &drive_behavior_api);
#endif
