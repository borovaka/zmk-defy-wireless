/*
 * Fake LED strip that records the frames it is sent.
 *
 * SPDX-License-Identifier: MIT
 */

#define DT_DRV_COMPAT vnd_led_strip

#include <string.h>

#include <zephyr/device.h>
#include <zephyr/drivers/led_strip.h>

#include "fake_led_strip.h"

struct fake_led_strip_config {
    size_t length;
};

static int fake_led_strip_update_rgb(const struct device *dev, struct led_rgb *pixels,
                                     size_t num_pixels) {
    struct fake_led_strip_data *data = dev->data;

    memcpy(data->frame, pixels, num_pixels * sizeof(*pixels));
    data->count = num_pixels;
    data->updates++;
    if (data->on_update != NULL) {
        data->on_update(dev);
    }

    return 0;
}

static size_t fake_led_strip_length(const struct device *dev) {
    const struct fake_led_strip_config *config = dev->config;

    return config->length;
}

static DEVICE_API(led_strip, fake_led_strip_api) = {
    .update_rgb = fake_led_strip_update_rgb,
    .length = fake_led_strip_length,
};

#define FAKE_LED_STRIP_INIT(inst)                                                                  \
    BUILD_ASSERT(DT_INST_PROP(inst, chain_length) <= FAKE_LED_STRIP_MAX_PIXELS);                   \
                                                                                                   \
    static const struct fake_led_strip_config fake_led_strip_config_##inst = {                     \
        .length = DT_INST_PROP(inst, chain_length),                                                \
    };                                                                                             \
                                                                                                   \
    static struct fake_led_strip_data fake_led_strip_data_##inst;                                  \
                                                                                                   \
    DEVICE_DT_INST_DEFINE(inst, NULL, NULL, &fake_led_strip_data_##inst,                           \
                          &fake_led_strip_config_##inst, POST_KERNEL,                              \
                          CONFIG_LED_STRIP_INIT_PRIORITY, &fake_led_strip_api);

DT_INST_FOREACH_STATUS_OKAY(FAKE_LED_STRIP_INIT)
