/* SPDX-License-Identifier: MIT */

#include <zephyr/init.h>
#include <zephyr/settings/settings.h>

extern bool host_initialized;

static int initialize_bluetooth_settings(void) {
    int err = settings_subsys_init();
    host_initialized = err == 0;
    return err;
}

SYS_INIT(initialize_bluetooth_settings, APPLICATION, CONFIG_ZMK_BLE_INIT_PRIORITY);
