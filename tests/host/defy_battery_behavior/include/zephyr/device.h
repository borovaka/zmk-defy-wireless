/* SPDX-License-Identifier: MIT */

#pragma once
#define ARG_UNUSED(x) (void)(x)
#define BIT(n) (1u << (n))
#define IS_ENABLED(config) (config)
/* The devicetree has a &defy_battery node. */
#define DT_HAS_COMPAT_STATUS_OKAY(compat) 1
