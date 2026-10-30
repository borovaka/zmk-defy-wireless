/*
 * Emulated IS31FL3743B on the SPI emulator. It accepts write frames only:
 * command byte 0101 0ppp, register address, then data with auto-increment.
 *
 * SPDX-License-Identifier: MIT
 */

#define DT_DRV_COMPAT issi_is31fl3743b

#include <errno.h>

#include <zephyr/device.h>
#include <zephyr/drivers/emul.h>
#include <zephyr/drivers/spi.h>
#include <zephyr/drivers/spi_emul.h>

#include "emul.h"

static int is31fl3743b_emul_io(const struct emul *target, const struct spi_config *config,
                               const struct spi_buf_set *tx_bufs,
                               const struct spi_buf_set *rx_bufs) {
    struct is31fl3743b_emul_data *data = target->data;
    uint8_t page = 0;
    uint8_t reg = 0;
    size_t pos = 0;

    ARG_UNUSED(config);
    ARG_UNUSED(rx_bufs);

    if (tx_bufs == NULL) {
        return -EIO;
    }

    for (size_t i = 0; i < tx_bufs->count; i++) {
        const uint8_t *bytes = tx_bufs->buffers[i].buf;

        for (size_t j = 0; j < tx_bufs->buffers[i].len; j++, pos++) {
            if (pos == 0) {
                if ((bytes[j] & 0xf8) != 0x50 || (bytes[j] & 0x07) > 2) {
                    return -EIO;
                }
                page = bytes[j] & 0x07;
            } else if (pos == 1) {
                reg = bytes[j];
            } else {
                data->pages[page][reg++] = bytes[j];
            }
        }
    }

    data->transfers++;
    return 0;
}

static const struct spi_emul_api is31fl3743b_emul_api = {
    .io = is31fl3743b_emul_io,
};

static int is31fl3743b_emul_init(const struct emul *target, const struct device *parent) {
    ARG_UNUSED(target);
    ARG_UNUSED(parent);
    return 0;
}

#define IS31FL3743B_EMUL(n)                                                                        \
    static struct is31fl3743b_emul_data is31fl3743b_emul_data_##n;                                \
    EMUL_DT_INST_DEFINE(n, is31fl3743b_emul_init, &is31fl3743b_emul_data_##n, NULL,               \
                        &is31fl3743b_emul_api, NULL)

DT_INST_FOREACH_STATUS_OKAY(IS31FL3743B_EMUL)
