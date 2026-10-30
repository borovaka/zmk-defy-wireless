/* SPDX-License-Identifier: MIT */

#pragma once

#include <stdint.h>

uint32_t defy_radio_boot_read(uintptr_t address);
void defy_radio_boot_write(uintptr_t address, uint32_t value);
