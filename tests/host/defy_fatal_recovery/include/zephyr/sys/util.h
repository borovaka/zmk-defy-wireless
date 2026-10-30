/* SPDX-License-Identifier: MIT */

#pragma once
#define BIT(n) (1u << (n))
#define BUILD_ASSERT(cond) _Static_assert(cond, #cond)

#define MIN(a, b) ((a) < (b) ? (a) : (b))
#define ARG_UNUSED(x) (void)(x)
#define ARRAY_SIZE(array) (sizeof(array) / sizeof((array)[0]))
