/*
 * Between the parts of drive mode: main.c, the drive's disk (disk.c) and the
 * status LED (led.c).
 *
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include <stdbool.h>

/* Opens ZMK's code_partition, registers the drive's disk and checks it through Zephyr's
 * disk access, as the mass storage class will find it; 0 on success. The flash driver
 * must be ready, so main.c calls it after the kernel's initialisation. The mass storage
 * class looks the disk up by name on each command, and without it answers that no
 * medium is present. */
int defy_loader_disk_init(void);

/* True once a UF2 block for ZMK's range has reached the writer. */
bool defy_loader_disk_written(void);

/* A UF2 file has been written in full: main.c resets into ZMK shortly. */
void defy_loader_copied(void);

/* True if a reset would start ZMK: its vector table is valid, and its unconfirmed
 * starts are below the limit. */
bool defy_loader_zmk_startable(void);

/* Sets up the status LED's PWM and lights the LED for drive mode. */
void defy_loader_led_init(void);

/* A UF2 block has been written, or kept for ZMK's first page: the LED blinks green
 * until COPY_IDLE_MS after the last one. */
void defy_loader_led_block(void);

/* Turns the LED off for good, before System OFF. */
void defy_loader_led_off(void);
