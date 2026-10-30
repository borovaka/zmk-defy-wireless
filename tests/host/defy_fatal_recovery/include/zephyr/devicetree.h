/* SPDX-License-Identifier: MIT */

#pragma once
#define DT_NODELABEL(node) node
#define DT_GPIO_PIN(node, prop) DT_GPIO_PIN_##prop
#define DT_GPIO_PIN_sdb_gpios 15
#define DT_GPIO_PIN_underglow_power_gpios 14
#define DT_GPIO_CTLR(node, prop) 0
#define DT_SAME_NODE(a, b) 1
#define DT_GPIO_FLAGS(node, prop) 0
/* kscan0 as on the right half: inputs on GPIO 0-7 out of order, lines on GPIO 8-12. */
#define DT_FOREACH_PROP_ELEM(node, prop, fn) DT_FOREACH_##prop(node, fn)
#define DT_FOREACH_col_gpios(node, fn)                                                             \
    fn(node, col_gpios, 0) fn(node, col_gpios, 1) fn(node, col_gpios, 2)                           \
    fn(node, col_gpios, 3) fn(node, col_gpios, 4) fn(node, col_gpios, 5)                           \
    fn(node, col_gpios, 6) fn(node, col_gpios, 7)
#define DT_FOREACH_row_gpios(node, fn)                                                             \
    fn(node, row_gpios, 0) fn(node, row_gpios, 1) fn(node, row_gpios, 2)                           \
    fn(node, row_gpios, 3) fn(node, row_gpios, 4)
#define DT_GPIO_PIN_BY_IDX(node, prop, idx) DT_GPIO_PIN_BY_IDX_##prop(idx)
#define DT_GPIO_PIN_BY_IDX_col_gpios(idx) ((idx) ^ 7)
#define DT_GPIO_PIN_BY_IDX_row_gpios(idx) (8 + (idx))
#define DT_PROP_LEN(node, prop) DT_PROP_LEN_##prop
#define DT_PROP_LEN_col_gpios 8
#define DT_PROP_LEN_row_gpios 5
#define DT_GPIO_CTLR_BY_IDX(node, prop, idx) 0
#define DT_GPIO_FLAGS_BY_IDX(node, prop, idx) GPIO_ACTIVE_LOW
