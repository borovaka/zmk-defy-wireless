/* SPDX-License-Identifier: MIT */

#include <string.h>

#include <zephyr/device.h>
#include <zephyr/ztest.h>

#include "fake_spi.h"

static int fake_transceive(const struct device *device, const struct spi_config *config,
                          const struct spi_buf_set *tx, const struct spi_buf_set *rx,
                          spi_callback_t callback, void *userdata) {
    struct fake_spi *state = device->data;
    zassert_false(state->active);
    zassert_equal(config->operation,
                  SPI_OP_MODE_SLAVE | SPI_MODE_CPHA | SPI_WORD_SET(8) | SPI_TRANSFER_MSB);
    zassert_equal(tx->count, 1);
    zassert_equal(rx->count, 1);
    zassert_equal(tx->buffers[0].len, DEFY_WIRED_FRAME_SIZE);
    zassert_equal(rx->buffers[0].len, DEFY_WIRED_FRAME_SIZE);
    if (state->arm_error) {
        int err = state->arm_error;
        state->arm_error = 0;
        return err;
    }
    state->tx = tx;
    state->rx = rx;
    state->callback = callback;
    state->userdata = userdata;
    state->active = true;
    memcpy(state->armed_tx, tx->buffers[0].buf, DEFY_WIRED_FRAME_SIZE);
    return 0;
}

static DEVICE_API(spi, fake_spi_api) = {.transceive_async = fake_transceive};
struct fake_spi fake_spi0;
struct fake_spi fake_spi1;
DEVICE_DT_DEFINE(DT_NODELABEL(test_spi0), NULL, NULL, &fake_spi0, NULL, POST_KERNEL, 50,
                 &fake_spi_api);
DEVICE_DT_DEFINE(DT_NODELABEL(test_spi1), NULL, NULL, &fake_spi1, NULL, POST_KERNEL, 50,
                 &fake_spi_api);
