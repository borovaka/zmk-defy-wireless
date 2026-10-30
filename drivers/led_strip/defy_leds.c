/*
 * The Defy's lights as one LED strip: the per-key LEDs first, then the
 * underglow chain, for the start-up and lighting code.
 *
 * The underglow needs two things Zephyr's WS2812 PIO driver does not do:
 * - Power. UG_EN switches the chain's supply. The LEDs draw current even when
 *   dark, so the supply is on only while an underglow LED is lit, as in the
 *   stock firmware. Switching it off clears the LEDs, so the next lit frame
 *   powers the chain up and then sends that frame.
 * - Polarity. The data line passes through an inverter, so the stock firmware
 *   runs the WS2812 PIO program with every side-set bit flipped. Here the
 *   RP2040 inverts the pin instead. The strip driver's pinctrl rewrites the
 *   pin's control register, so the inversion goes in after it: this device
 *   depends on the strip, so it initialises later at the same priority.
 *
 * SPDX-License-Identifier: MIT
 */

#define DT_DRV_COMPAT dygma_defy_leds

#include <zephyr/device.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/drivers/led_strip.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/pm/device.h>
#include <zephyr/sys/util.h>

#ifdef CONFIG_SOC_RP2040
#include <hardware/gpio.h>
#endif

LOG_MODULE_REGISTER(defy_leds, CONFIG_LED_STRIP_LOG_LEVEL);

struct defy_leds_config {
    const struct device *key_leds;
    const struct device *underglow;
    struct gpio_dt_spec underglow_power;
    size_t length;
    size_t key_leds_length;
    size_t underglow_length;
    uint32_t power_on_delay_us;
    int8_t underglow_inverted_pin;
};

struct defy_leds_data {
    bool underglow_powered;
    bool suspended;
};

static bool defy_leds_lit(const struct led_rgb *pixels, size_t count) {
    for (size_t i = 0; i < count; i++) {
        if (pixels[i].r || pixels[i].g || pixels[i].b) {
            return true;
        }
    }

    return false;
}

static int defy_leds_set_underglow_power(const struct device *dev, bool on) {
    const struct defy_leds_config *config = dev->config;
    struct defy_leds_data *data = dev->data;

    if (data->underglow_powered == on) {
        return 0;
    }

    int ret = gpio_pin_set_dt(&config->underglow_power, on);

    if (ret < 0) {
        return ret;
    }

    data->underglow_powered = on;
    if (on) {
        k_usleep(config->power_on_delay_us);
    }

    return 0;
}

static int defy_leds_update_underglow(const struct device *dev, struct led_rgb *pixels,
                                      size_t count) {
    const struct defy_leds_config *config = dev->config;
    struct defy_leds_data *data = dev->data;

    /* Suspended, the chain stays off until resumed and sent a lit frame. */
    if (data->suspended || !defy_leds_lit(pixels, count)) {
        return defy_leds_set_underglow_power(dev, false);
    }

    int ret = defy_leds_set_underglow_power(dev, true);

    if (ret < 0) {
        return ret;
    }

    return led_strip_update_rgb(config->underglow, pixels, count);
}

static int defy_leds_update_rgb(const struct device *dev, struct led_rgb *pixels,
                                size_t num_pixels) {
    const struct defy_leds_config *config = dev->config;
    size_t keys = MIN(num_pixels, config->key_leds_length);
    size_t underglow = MIN(num_pixels - keys, config->underglow_length);

    if (keys > 0) {
        int ret = led_strip_update_rgb(config->key_leds, pixels, keys);

        if (ret < 0) {
            return ret;
        }
    }

    if (underglow > 0) {
        return defy_leds_update_underglow(dev, pixels + keys, underglow);
    }

    return 0;
}

static size_t defy_leds_length(const struct device *dev) {
    const struct defy_leds_config *config = dev->config;

    return config->length;
}

#ifdef CONFIG_PM_DEVICE
static int defy_leds_pm_action(const struct device *dev, enum pm_device_action action) {
    struct defy_leds_data *data = dev->data;

    switch (action) {
    case PM_DEVICE_ACTION_SUSPEND:
        data->suspended = true;
        return defy_leds_set_underglow_power(dev, false);
    case PM_DEVICE_ACTION_RESUME:
        data->suspended = false;
        return 0;
    default:
        return -ENOTSUP;
    }
}
#endif /* CONFIG_PM_DEVICE */

static int defy_leds_init(const struct device *dev) {
    const struct defy_leds_config *config = dev->config;

    if (!device_is_ready(config->key_leds)) {
        LOG_ERR("%s: per-key LEDs not ready", dev->name);
        return -ENODEV;
    }

    if (!device_is_ready(config->underglow)) {
        LOG_ERR("%s: underglow not ready", dev->name);
        return -ENODEV;
    }

    if (!gpio_is_ready_dt(&config->underglow_power)) {
        LOG_ERR("%s: underglow power GPIO not ready", dev->name);
        return -ENODEV;
    }

    int ret = gpio_pin_configure_dt(&config->underglow_power, GPIO_OUTPUT_INACTIVE);

    if (ret < 0) {
        return ret;
    }

#ifdef CONFIG_SOC_RP2040
    if (config->underglow_inverted_pin >= 0) {
        gpio_set_outover(config->underglow_inverted_pin, GPIO_OVERRIDE_INVERT);
    }
#endif

    return 0;
}

static DEVICE_API(led_strip, defy_leds_api) = {
    .update_rgb = defy_leds_update_rgb,
    .length = defy_leds_length,
};

#define DEFY_LEDS_PART_LENGTH(inst, prop) DT_PROP(DT_INST_PHANDLE(inst, prop), chain_length)

#define DEFY_LEDS_INIT(inst)                                                                       \
    BUILD_ASSERT(DEFY_LEDS_PART_LENGTH(inst, key_leds) + DEFY_LEDS_PART_LENGTH(inst, underglow) == \
                     DT_INST_PROP(inst, chain_length),                                             \
                 "chain-length must be the sum of the parts' chain-length");                       \
    BUILD_ASSERT(!DT_INST_PROP(inst, underglow_inverted) || IS_ENABLED(CONFIG_SOC_RP2040),         \
                 "underglow-inverted is for an RP2040");                                           \
                                                                                                   \
    static const struct defy_leds_config defy_leds_config_##inst = {                               \
        .key_leds = DEVICE_DT_GET(DT_INST_PHANDLE(inst, key_leds)),                                \
        .underglow = DEVICE_DT_GET(DT_INST_PHANDLE(inst, underglow)),                              \
        .underglow_power = GPIO_DT_SPEC_INST_GET(inst, underglow_power_gpios),                     \
        .length = DT_INST_PROP(inst, chain_length),                                                \
        .key_leds_length = DEFY_LEDS_PART_LENGTH(inst, key_leds),                                  \
        .underglow_length = DEFY_LEDS_PART_LENGTH(inst, underglow),                                \
        .power_on_delay_us = DT_INST_PROP(inst, underglow_power_on_delay_us),                      \
        .underglow_inverted_pin =                                                                  \
            COND_CODE_1(DT_INST_PROP(inst, underglow_inverted),                                    \
                        (DT_GPIO_PIN(DT_INST_PHANDLE(inst, underglow), gpios)), (-1)),             \
    };                                                                                             \
                                                                                                   \
    static struct defy_leds_data defy_leds_data_##inst;                                            \
                                                                                                   \
    PM_DEVICE_DT_INST_DEFINE(inst, defy_leds_pm_action);                                           \
                                                                                                   \
    DEVICE_DT_INST_DEFINE(inst, defy_leds_init, PM_DEVICE_DT_INST_GET(inst),                       \
                          &defy_leds_data_##inst, &defy_leds_config_##inst, POST_KERNEL,           \
                          CONFIG_LED_STRIP_INIT_PRIORITY, &defy_leds_api);

DT_INST_FOREACH_STATUS_OKAY(DEFY_LEDS_INIT)
