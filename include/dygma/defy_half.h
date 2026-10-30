/* SPDX-License-Identifier: MIT */

#ifndef DYGMA_DEFY_HALF_H
#define DYGMA_DEFY_HALF_H

#include <zephyr/devicetree.h>

/* Which half this is, DEFY_WIRED_LEFT or DEFY_WIRED_RIGHT: the half-id of its
 * cable node, which every half's devicetree has. */
#define DEFY_HALF_ID DT_PROP(DT_INST(0, dygma_defy_wired_peripheral), half_id)

#endif
