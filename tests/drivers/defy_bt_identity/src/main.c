/* SPDX-License-Identifier: MIT */

#include <zephyr/ztest.h>

#include "../../../../drivers/bluetooth/defy_bt_identity.c"

static bt_addr_le_t current_ids[CONFIG_BT_ID_MAX];
static bt_addr_le_t saved_ids[CONFIG_BT_ID_MAX];
static size_t current_count;
static size_t saved_count;
static int save_error;
static unsigned int save_calls;
static unsigned int advertise_calls;
static bool pause_advertising;
static int threaded_advertising_result;
static K_SEM_DEFINE(advertising_entered, 0, 1);
static K_SEM_DEFINE(advertising_release, 0, 1);
static K_SEM_DEFINE(advertising_finished, 0, 1);
static K_SEM_DEFINE(gate_finished, 0, 1);
static K_THREAD_STACK_DEFINE(advertising_stack, 2048);
static K_THREAD_STACK_DEFINE(gate_stack, 2048);
static struct k_thread advertising_thread, gate_thread;

bool bt_is_ready(void) { return true; }

void bt_id_get(bt_addr_le_t *addrs, size_t *count) {
    *count = MIN(*count, current_count);
    memcpy(addrs, current_ids, *count * sizeof(addrs[0]));
}

int settings_save_one(const char *name, const void *value, size_t len) {
    ++save_calls;
    zassert_str_equal(name, "bt/id");
    zassert_equal(len, current_count * sizeof(current_ids[0]));
    zassert_mem_equal(value, current_ids, len);
    if (save_error) {
        return save_error;
    }
    memcpy(saved_ids, value, len);
    saved_count = len / sizeof(saved_ids[0]);
    return 0;
}

int __real_bt_le_adv_start(const struct bt_le_adv_param *param, const struct bt_data *ad,
                          size_t ad_len, const struct bt_data *sd, size_t sd_len) {
    ++advertise_calls;
    zassert_equal(saved_count, current_count, "advertised before saving identity");
    zassert_mem_equal(saved_ids, current_ids, current_count * sizeof(current_ids[0]));
    ARG_UNUSED(param);
    ARG_UNUSED(ad);
    ARG_UNUSED(ad_len);
    ARG_UNUSED(sd);
    ARG_UNUSED(sd_len);
    if (pause_advertising) {
        k_sem_give(&advertising_entered);
        k_sem_take(&advertising_release, K_FOREVER);
    }
    return 0;
}

static bt_addr_le_t address(uint8_t last) {
    return (bt_addr_le_t){.type = BT_ADDR_LE_RANDOM, .a = {{last, 2, 3, 4, 5, 0xc6}}};
}

static void before(void *fixture) {
    ARG_UNUSED(fixture);
    memset(current_ids, 0, sizeof(current_ids));
    memset(saved_ids, 0, sizeof(saved_ids));
    memset(persisted_ids, 0, sizeof(persisted_ids));
    current_ids[0] = address(1);
    current_count = 1;
    saved_count = persisted_count = 0;
    defy_bt_identity_set_advertising(true);
    save_error = 0;
    save_calls = advertise_calls = 0;
    pause_advertising = false;
    k_sem_reset(&advertising_entered);
    k_sem_reset(&advertising_release);
    k_sem_reset(&advertising_finished);
    k_sem_reset(&gate_finished);
}

static int advertise(void) {
    return __wrap_bt_le_adv_start(BT_LE_ADV_CONN_FAST_1, NULL, 0, NULL, 0);
}

ZTEST(identity, test_save_before_first_advertising) {
    zassert_ok(advertise());
    zassert_equal(save_calls, 1);
    zassert_equal(advertise_calls, 1);
    zassert_equal(saved_count, 1);
    zassert_mem_equal(saved_ids, current_ids, sizeof(current_ids[0]));
}

ZTEST(identity, test_failed_save_prevents_advertising_then_retries) {
    save_error = -ENOSPC;
    zassert_equal(advertise(), -ENOSPC);
    zassert_equal(advertise_calls, 0);
    zassert_equal(persisted_count, 0);
    save_error = 0;
    zassert_ok(advertise());
    zassert_equal(save_calls, 2);
    zassert_equal(advertise_calls, 1);
}

ZTEST(identity, test_repeated_advertising_does_not_save_again) {
    zassert_ok(advertise());
    for (int i = 0; i < 5; ++i) {
        zassert_ok(advertise());
    }
    zassert_equal(save_calls, 1);
    zassert_equal(advertise_calls, 6);
}

ZTEST(identity, test_changed_identity_must_save_again) {
    zassert_ok(advertise());
    current_ids[0] = address(42);
    save_error = -EIO;
    zassert_equal(advertise(), -EIO);
    zassert_equal(advertise_calls, 1);
    save_error = 0;
    zassert_ok(advertise());
    zassert_equal(save_calls, 3);
    zassert_equal(advertise_calls, 2);
}

ZTEST(identity, test_all_identities_use_native_storage_format) {
    current_ids[1] = address(2);
    current_count = 2;
    struct bt_le_adv_param param = *BT_LE_ADV_CONN_FAST_1;
    param.id = 1;
    zassert_ok(__wrap_bt_le_adv_start(&param, NULL, 0, NULL, 0));
    zassert_equal(saved_count, 2);
    zassert_mem_equal(saved_ids, current_ids, sizeof(current_ids));
}

ZTEST(identity, test_missing_identity_does_not_advertise) {
    current_count = 0;
    zassert_equal(advertise(), -EINVAL);
    zassert_equal(save_calls, 0);
    zassert_equal(advertise_calls, 0);
}

ZTEST(identity, test_closed_gate_rejects_late_advertising_without_identity_write) {
    defy_bt_identity_set_advertising(false);
    for (int i = 0; i < 3; i++) {
        zassert_equal(advertise(), -EAGAIN);
    }
    zassert_equal(save_calls, 0);
    zassert_equal(advertise_calls, 0);
    defy_bt_identity_set_advertising(true);
    zassert_ok(advertise());
    zassert_equal(save_calls, 1);
    zassert_equal(advertise_calls, 1);
}

static void advertise_thread(void *a, void *b, void *c) {
    ARG_UNUSED(a);
    ARG_UNUSED(b);
    ARG_UNUSED(c);
    threaded_advertising_result = advertise();
    k_sem_give(&advertising_finished);
}

static void close_gate_thread(void *a, void *b, void *c) {
    ARG_UNUSED(a);
    ARG_UNUSED(b);
    ARG_UNUSED(c);
    defy_bt_identity_set_advertising(false);
    k_sem_give(&gate_finished);
}

ZTEST(identity, test_gate_close_waits_for_inflight_start_then_rejects_late_work) {
    pause_advertising = true;
    k_thread_create(&advertising_thread, advertising_stack,
                    K_THREAD_STACK_SIZEOF(advertising_stack), advertise_thread,
                    NULL, NULL, NULL, 5, 0, K_NO_WAIT);
    zassert_ok(k_sem_take(&advertising_entered, K_MSEC(100)));
    k_thread_create(&gate_thread, gate_stack, K_THREAD_STACK_SIZEOF(gate_stack),
                    close_gate_thread, NULL, NULL, NULL, 5, 0, K_NO_WAIT);
    zassert_equal(k_sem_take(&gate_finished, K_MSEC(20)), -EAGAIN);
    k_sem_give(&advertising_release);
    zassert_ok(k_sem_take(&advertising_finished, K_MSEC(100)));
    zassert_ok(k_sem_take(&gate_finished, K_MSEC(100)));
    zassert_ok(k_thread_join(&advertising_thread, K_MSEC(100)));
    zassert_ok(k_thread_join(&gate_thread, K_MSEC(100)));
    zassert_ok(threaded_advertising_result);
    pause_advertising = false;
    zassert_equal(advertise(), -EAGAIN);
    zassert_equal(advertise_calls, 1);
    zassert_equal(save_calls, 1);
}

ZTEST_SUITE(identity, NULL, NULL, before, NULL, NULL);
