/* SPDX-License-Identifier: MIT */

#pragma once

/* Keeps each init function, so the test can call it. */
#define SYS_INIT(fn, init_level, init_priority) \
    static int (*const test_init_##fn)(void) __attribute__((unused)) = fn
