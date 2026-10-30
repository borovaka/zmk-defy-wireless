/*
 * Register access for boot.c. tests/host/defy_radio_boot replaces this file
 * with one that fakes the registers.
 *
 * SPDX-License-Identifier: MIT
 */

#ifndef DEFY_RADIO_BOOT_IO_H
#define DEFY_RADIO_BOOT_IO_H

#include <stdint.h>

static inline uint32_t defy_radio_boot_read(uintptr_t address) {
    return *(volatile uint32_t *)address;
}

static inline void defy_radio_boot_write(uintptr_t address, uint32_t value) {
    *(volatile uint32_t *)address = value;
}

#endif
