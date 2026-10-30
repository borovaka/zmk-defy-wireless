/* SPDX-License-Identifier: MIT */

#include <zephyr/sys/atomic.h>
#include <dygma/defy_update_guard.h>

static atomic_t update_owner;

bool defy_update_claim(enum defy_update_owner owner) {
    return atomic_cas(&update_owner, 0, owner);
}

void defy_update_release(enum defy_update_owner owner) {
    atomic_cas(&update_owner, owner, 0);
}

bool defy_update_held(enum defy_update_owner owner) {
    return atomic_get(&update_owner) == owner;
}
