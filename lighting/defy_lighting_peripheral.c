/* SPDX-License-Identifier: MIT */

#include <dygma/defy_half_leds.h>
#include <dygma/defy_lighting.h>
#include <errno.h>
#include <zephyr/kernel.h>

static struct k_spinlock applied_lock;
static struct defy_lighting_state applied;
/* What applied shows, kept to restore it if a newer state cannot stay. */
static struct defy_lighting_view applied_view;
static uint32_t retired_boot;
/* The groups the Neuron with boot group_boot sent, by index; held has a bit for each. */
static struct defy_lighting_group groups[DEFY_LIGHTING_GROUPS];
static uint32_t group_boot;
static uint16_t held;
static uint32_t active_ble_epoch;
/* By link: the boot of the last state that link brought, and its connection's epoch then. */
static struct {
    uint32_t boot;
    uint32_t epoch;
} lighting_routes[DEFY_LINK_BLE + 1];

void defy_lighting_peripheral_ble_epoch(uint32_t epoch) {
    k_spinlock_key_t key = k_spin_lock(&applied_lock);
    if (active_ble_epoch != epoch) {
        active_ble_epoch = epoch;
        lighting_routes[DEFY_LINK_BLE].epoch = epoch;
        lighting_routes[DEFY_LINK_BLE].boot = 0;
    }
    k_spin_unlock(&applied_lock, key);
}

int defy_lighting_peripheral_group(const struct defy_lighting_group *group) {
    if (!defy_lighting_group_valid(group)) {
        return -EINVAL;
    }
    k_spinlock_key_t key = k_spin_lock(&applied_lock);
    if (group->boot == retired_boot) {
        k_spin_unlock(&applied_lock, key);
        return -ESTALE;
    }
    if (group->boot != group_boot) {
        group_boot = group->boot;
        held = 0;
    }
    groups[group->index] = *group;
    held |= BIT(group->index);
    k_spin_unlock(&applied_lock, key);
    return 0;
}

bool defy_lighting_peripheral_get(struct defy_lighting_state *state) {
    if (!state) {
        return false;
    }
    k_spinlock_key_t key = k_spin_lock(&applied_lock);
    bool available = applied.boot != 0;
    if (available) {
        *state = applied;
    }
    k_spin_unlock(&applied_lock, key);
    return available;
}

int defy_lighting_peripheral_apply(const struct defy_lighting_state *state, enum defy_link route,
                                   uint32_t route_epoch) {
    if (!defy_lighting_state_valid(state) || route > DEFY_LINK_BLE || !route_epoch) {
        return -EINVAL;
    }
    /* No second apply() may run while this one draws without the lock. */
    __ASSERT(k_current_get() == &k_sys_work_q.thread,
             "lighting is applied on the system work queue");
    k_spinlock_key_t key = k_spin_lock(&applied_lock);
    if (route == DEFY_LINK_BLE && route_epoch != active_ble_epoch) {
        k_spin_unlock(&applied_lock, key);
        return -ESTALE;
    }
    if (state->boot == retired_boot) {
        k_spin_unlock(&applied_lock, key);
        return -ESTALE;
    }
    if (applied.boot == state->boot) {
        int32_t difference = (int32_t)(state->revision - applied.revision);
        if (difference < 0) {
            k_spin_unlock(&applied_lock, key);
            return -ESTALE;
        }
        if (difference == 0) {
            if (!defy_lighting_equal(state, &applied)) {
                k_spin_unlock(&applied_lock, key);
                return -EPROTO;
            }
            lighting_routes[route].boot = state->boot;
            lighting_routes[route].epoch = route_epoch;
            k_spin_unlock(&applied_lock, key);
            return 0;
        }
    } else if (lighting_routes[route].epoch == route_epoch &&
               lighting_routes[route].boot) {
        k_spin_unlock(&applied_lock, key);
        return -ESTALE;
    }
    uint16_t state_held = group_boot == state->boot ? held : 0;
    if (state->groups & ~state_held) {
        k_spin_unlock(&applied_lock, key);
        return -EAGAIN;
    }
    struct defy_lighting_view view;
    defy_lighting_view(state, groups, state_held, &view);
    struct defy_lighting_state previous = applied;
    struct defy_lighting_view previous_view = applied_view;
    k_spin_unlock(&applied_lock, key);
    int err = defy_half_leds_lighting_set(&view);
    if (err) {
        return err;
    }
    key = k_spin_lock(&applied_lock);
    if (route == DEFY_LINK_BLE && route_epoch != active_ble_epoch) {
        k_spin_unlock(&applied_lock, key);
        if (defy_half_leds_lighting_set(previous.boot ? &previous_view : NULL)) {
            key = k_spin_lock(&applied_lock);
            applied.boot = 0;
            k_spin_unlock(&applied_lock, key);
        }
        return -ESTALE;
    }
    if (applied.boot && applied.boot != state->boot) {
        retired_boot = applied.boot;
    }
    if (route == DEFY_LINK_WIRED && applied.boot != state->boot && active_ble_epoch) {
        lighting_routes[DEFY_LINK_BLE].epoch = active_ble_epoch;
        lighting_routes[DEFY_LINK_BLE].boot = state->boot;
    }
    applied = *state;
    applied_view = view;
    lighting_routes[route].boot = state->boot;
    lighting_routes[route].epoch = route_epoch;
    k_spin_unlock(&applied_lock, key);
    return 0;
}
