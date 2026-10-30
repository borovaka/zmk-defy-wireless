/* SPDX-License-Identifier: MIT */

#pragma once

#include <stdint.h>

void hw_set_bits(volatile uint32_t *address, uint32_t mask);
void hw_clear_bits(volatile uint32_t *address, uint32_t mask);
