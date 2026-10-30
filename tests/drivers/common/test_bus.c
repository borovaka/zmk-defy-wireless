/*
 * The dygma,radio-test-spi controller: drives chip select as a GPIO and passes
 * each transfer, and each change of chip select, to fake_radio.c.
 *
 * SPDX-License-Identifier: MIT
 */

#include <string.h>

#include <zephyr/drivers/gpio/gpio_emul.h>
#include <zephyr/drivers/spi.h>
#include <zephyr/logging/log.h>

LOG_MODULE_REGISTER(defy_test_bus);

#include "spi_context.h"
#include "fake_radio.h"

static struct {
    struct spi_context ctx;
    bool selected;
} bus = {
    SPI_CONTEXT_INIT_LOCK(bus, ctx),
    SPI_CONTEXT_INIT_SYNC(bus, ctx),
    SPI_CONTEXT_CS_GPIOS_INITIALIZE(DT_NODELABEL(spi0), ctx)
};

static void sync_select(void) {
    const struct gpio_dt_spec *cs = &bus.ctx.config->cs.gpio;
    bool selected = gpio_emul_output_get(cs->port, cs->pin) == 0;

    if (selected != bus.selected) {
        bus.selected = selected;
        fake_radio_select(selected);
    }
}

static int transfer(const struct device *dev, const struct spi_config *config,
                    const struct spi_buf_set *tx, const struct spi_buf_set *rx) {
    ARG_UNUSED(dev);
    uint8_t out[260] = {0}, in[260] = {0};
    size_t tx_len = 0, rx_len = 0;
    int err;

    spi_context_lock(&bus.ctx, false, NULL, NULL, config);
    bus.ctx.config = config;
    spi_context_cs_control(&bus.ctx, true);
    sync_select();
    for (size_t i = 0; tx != NULL && i < tx->count; i++) {
        __ASSERT(tx_len + tx->buffers[i].len <= sizeof(out), "TX too long");
        if (tx->buffers[i].buf != NULL) {
            memcpy(out + tx_len, tx->buffers[i].buf, tx->buffers[i].len);
        }
        tx_len += tx->buffers[i].len;
    }
    for (size_t i = 0; rx != NULL && i < rx->count; i++) {
        rx_len += rx->buffers[i].len;
    }
    __ASSERT(rx_len <= sizeof(in), "RX too long");
    err = fake_radio_clock(out, in, MAX(tx_len, rx_len));
    for (size_t i = 0, offset = 0; rx != NULL && i < rx->count; i++) {
        if (rx->buffers[i].buf != NULL) {
            memcpy(rx->buffers[i].buf, in + offset, rx->buffers[i].len);
        }
        offset += rx->buffers[i].len;
    }
    spi_context_cs_control(&bus.ctx, false);
    sync_select();
    spi_context_release(&bus.ctx, err);
    return err;
}

static int release(const struct device *dev, const struct spi_config *config) {
    ARG_UNUSED(dev);
    ARG_UNUSED(config);
    spi_context_unlock_unconditionally(&bus.ctx);
    sync_select();
    return 0;
}

static int init(const struct device *dev) {
    ARG_UNUSED(dev);
    int err = spi_context_cs_configure_all(&bus.ctx);

    spi_context_unlock_unconditionally(&bus.ctx);
    return err;
}

static DEVICE_API(spi, api) = {.transceive = transfer, .release = release};

SPI_DEVICE_DT_DEFINE(DT_NODELABEL(spi0), init, NULL, NULL, NULL, POST_KERNEL,
                     CONFIG_SPI_INIT_PRIORITY, &api);
