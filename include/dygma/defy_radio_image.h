/*
 * The nRF52820 firmware a half installs through rf_bootloader, and the seal
 * the bootloader stores for it. scripts/package_radio.py generates the
 * definitions from the radio build.
 *
 * SPDX-License-Identifier: MIT
 */

#ifndef DYGMA_DEFY_RADIO_IMAGE_H
#define DYGMA_DEFY_RADIO_IMAGE_H

#include <stddef.h>
#include <stdint.h>

#include <dygma/defy_radio_factory.h>

extern const uint8_t defy_radio_image[];
extern const size_t defy_radio_image_size;
extern const uint8_t defy_radio_image_seal[DEFY_RADIO_SEAL_SIZE];
extern const uint32_t defy_radio_image_version;

#endif
