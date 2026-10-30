/*
 * The Neuron's two cable ports, as SPI slaves on the test nodes of app.overlay.
 * A transfer the central arms stays active until the test completes it.
 *
 * SPDX-License-Identifier: MIT
 */

#ifndef DEFY_TEST_FAKE_SPI_H
#define DEFY_TEST_FAKE_SPI_H

#include <stdbool.h>
#include <stdint.h>

#include <zephyr/drivers/spi.h>

#include <dygma/defy_wired_protocol.h>

struct fake_spi {
    const struct spi_buf_set *tx;
    const struct spi_buf_set *rx;
    spi_callback_t callback;
    void *userdata;
    bool active;
    /* Returned once by the next transfer the central arms. */
    int arm_error;
    uint8_t armed_tx[DEFY_WIRED_FRAME_SIZE];
};

extern struct fake_spi fake_spi0;
extern struct fake_spi fake_spi1;

#endif
