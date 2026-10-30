/*
 * The halves' wrap of bt_le_adv_start(), linked with --wrap.
 *
 * It saves the Bluetooth identity to settings before advertising with it
 * (CONFIG_DEFY_BT_PERSISTENT_ID): a half then keeps its address across
 * restarts, and the Neuron finds it again with the bond it has.
 *
 * It is also the gate half sleep closes on advertising: while a half shuts
 * Bluetooth down to sleep, every new advertising start is refused, so that
 * ZMK cannot advertise again in between. The hybrid peripheral closes it
 * with defy_bt_identity_set_advertising(), and opens it again if sleep is
 * called off.
 *
 * SPDX-License-Identifier: MIT
 */

#include <errno.h>
#include <string.h>

#include <zephyr/bluetooth/bluetooth.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/settings/settings.h>

#include <dygma/defy_bt_identity.h>
#include <dygma/defy_wrap.h>

LOG_MODULE_REGISTER(defy_bt_identity);

static K_MUTEX_DEFINE(identity_lock);
static bt_addr_le_t persisted_ids[CONFIG_BT_ID_MAX];
static size_t persisted_count;

#ifdef CONFIG_DEFY_HALF_SLEEP
static bool advertising_allowed = true;

void defy_bt_identity_set_advertising(bool allowed) {
    k_mutex_lock(&identity_lock, K_FOREVER);
    advertising_allowed = allowed;
    k_mutex_unlock(&identity_lock);
}
#endif

DEFY_WRAP_REAL(bt_le_adv_start);

int __wrap_bt_le_adv_start(const struct bt_le_adv_param *param, const struct bt_data *ad,
                          size_t ad_len, const struct bt_data *sd, size_t sd_len) {
    bt_addr_le_t ids[CONFIG_BT_ID_MAX];
    size_t count = ARRAY_SIZE(ids);
    int err = 0;

    if (!bt_is_ready()) {
        return -EAGAIN;
    }
    if (!param) {
        return -EINVAL;
    }

    k_mutex_lock(&identity_lock, K_FOREVER);
#ifdef CONFIG_DEFY_HALF_SLEEP
    if (!advertising_allowed) {
        err = -EAGAIN;
        goto unlock;
    }
#endif
    bt_id_get(ids, &count);
    if (!count || param->id >= count || bt_addr_le_eq(&ids[param->id], BT_ADDR_LE_ANY)) {
        err = -EINVAL;
        goto unlock;
    }

    size_t size = count * sizeof(ids[0]);
    if (count != persisted_count || memcmp(ids, persisted_ids, size)) {
        /*
         * The key and layout Zephyr's host loads at bt_enable():
         * subsys/bluetooth/host/settings.c.
         */
        err = settings_save_one("bt/id", ids, size);
        if (err) {
            LOG_ERR("Cannot save Bluetooth identity: %d", err);
            goto unlock;
        }
        memcpy(persisted_ids, ids, size);
        persisted_count = count;
    }

    err = __real_bt_le_adv_start(param, ad, ad_len, sd, sd_len);
unlock:
    k_mutex_unlock(&identity_lock);
    return err;
}
DEFY_WRAP_CHECK(bt_le_adv_start);
