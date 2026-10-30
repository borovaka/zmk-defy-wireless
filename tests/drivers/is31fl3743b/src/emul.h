/*
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include <stdint.h>

/* Register model of an IS31FL3743B: pages 0 (PWM), 1 (scaling), 2 (function). */
struct is31fl3743b_emul_data {
    uint8_t pages[3][256];
    unsigned int transfers;
};
