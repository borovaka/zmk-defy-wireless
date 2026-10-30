/* SPDX-License-Identifier: MIT */

#include <assert.h>
#include <errno.h>
#include <string.h>

#define CONFIG_DEFY_BATTERY 1

#include "../../../battery/behavior_defy_battery.c"

static int wired_result[2];
static int wired_calls[2];
static int wireless_calls;
static uint8_t wireless_mask;

int defy_wired_battery_show(uint8_t side) {
    assert(side < 2);
    wired_calls[side]++;
    return wired_result[side];
}

void defy_control_ble_battery_show(uint8_t side_mask) {
    wireless_calls++;
    wireless_mask = side_mask;
}

/* A press on the central, with each cable's result: both halves are asked over their cables. */
static void press(int left, int right) {
    memset(wired_calls, 0, sizeof(wired_calls));
    wireless_calls = wireless_mask = 0;
    wired_result[0] = left;
    wired_result[1] = right;
    assert(battery_behavior_api.locality == BEHAVIOR_LOCALITY_CENTRAL);
    assert(battery_behavior_api.binding_pressed(NULL, (struct zmk_behavior_binding_event){0}) ==
           ZMK_BEHAVIOR_OPAQUE);
    assert(wired_calls[0] == 1 && wired_calls[1] == 1);
}

static void test_both_cabled_do_not_send_duplicate_bluetooth_commands(void) {
    press(0, 0);
    assert(wireless_calls == 0);
}

static void test_both_wireless_are_requested(void) {
    press(-ENOTCONN, -ENOTCONN);
    assert(wireless_calls == 1 && wireless_mask == 3);
}

static void test_left_wireless_right_cabled(void) {
    press(-ENOTCONN, 0);
    assert(wireless_calls == 1 && wireless_mask == 1);
}

int main(void) {
    test_both_cabled_do_not_send_duplicate_bluetooth_commands();
    test_both_wireless_are_requested();
    test_left_wireless_right_cabled();
    return 0;
}
