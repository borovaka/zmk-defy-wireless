/*
 * ISSI IS31FL3743B LED matrix driver on SPI, exposed as an LED strip.
 *
 * Each pixel is a group of consecutive PWM registers, one per entry in
 * color-mapping, starting at the pixel's entry in pwm-registers. A white
 * channel takes the part of the pixel common to red, green and blue, as
 * led_rgbw.h describes. The chip stays in hardware shutdown (SDB low) while
 * every pixel is off, and is configured again each time it powers up, as the
 * stock Defy firmware does.
 *
 * SPDX-License-Identifier: MIT
 */

#define DT_DRV_COMPAT issi_is31fl3743b

#include <zephyr/device.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/drivers/led_strip.h>
#include <zephyr/drivers/spi.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/pm/device.h>
#include <zephyr/sys/util.h>

#include "led_rgbw.h"

LOG_MODULE_REGISTER(is31fl3743b, CONFIG_LED_STRIP_LOG_LEVEL);

/* Command byte: 0101 0ppp writes page ppp, then the register address. */
#define IS31FL3743B_CMD_WRITE 0x50

#define IS31FL3743B_PAGE_PWM      0
#define IS31FL3743B_PAGE_SCALING  1
#define IS31FL3743B_PAGE_FUNCTION 2

#define IS31FL3743B_REG_CONFIG         0x00
#define IS31FL3743B_REG_GLOBAL_CURRENT 0x01
#define IS31FL3743B_REG_PULL           0x02

/* 11 SW lines by 18 CS lines. PWM and scaling registers are 0x01-0xC6. */
#define IS31FL3743B_CS_LINES  18
#define IS31FL3743B_SW_LINES  11
#define IS31FL3743B_DOTS      (IS31FL3743B_CS_LINES * IS31FL3743B_SW_LINES)
#define IS31FL3743B_FIRST_DOT 0x01

/* From SDB high to the first write. */
#define IS31FL3743B_SDB_SETUP_MS 1

struct is31fl3743b_config {
    struct spi_dt_spec bus;
    struct gpio_dt_spec sdb;
    const uint8_t *pwm_registers;
    const uint8_t *color_mapping;
    size_t length;
    uint8_t num_colors;
    uint8_t config;
    uint8_t global_current;
    uint8_t pull;
    uint8_t scaling;
};

struct is31fl3743b_data {
    uint8_t frame[IS31FL3743B_DOTS];
    bool powered;
};

static int is31fl3743b_write(const struct device *dev, uint8_t page, uint8_t reg,
                             const struct spi_buf *data, size_t count) {
    const struct is31fl3743b_config *config = dev->config;
    uint8_t header[2] = {IS31FL3743B_CMD_WRITE | page, reg};
    struct spi_buf bufs[1 + IS31FL3743B_SW_LINES] = {{.buf = header, .len = sizeof(header)}};

    __ASSERT_NO_MSG(count < ARRAY_SIZE(bufs));
    for (size_t i = 0; i < count; i++) {
        bufs[1 + i] = data[i];
    }

    const struct spi_buf_set tx = {.buffers = bufs, .count = 1 + count};

    return spi_write_dt(&config->bus, &tx);
}

static int is31fl3743b_write_reg(const struct device *dev, uint8_t page, uint8_t reg,
                                 uint8_t value) {
    const struct spi_buf buf = {.buf = &value, .len = 1};

    return is31fl3743b_write(dev, page, reg, &buf, 1);
}

static int is31fl3743b_power_up(const struct device *dev) {
    const struct is31fl3743b_config *config = dev->config;
    struct is31fl3743b_data *data = dev->data;
    int err;

    if (config->sdb.port != NULL) {
        err = gpio_pin_set_dt(&config->sdb, 1);
        if (err < 0) {
            return err;
        }
        k_sleep(K_MSEC(IS31FL3743B_SDB_SETUP_MS));
    }

    /* Same order as the stock firmware: pull and phase delay, current, configuration. */
    err = is31fl3743b_write_reg(dev, IS31FL3743B_PAGE_FUNCTION, IS31FL3743B_REG_PULL,
                                config->pull);
    if (err == 0) {
        err = is31fl3743b_write_reg(dev, IS31FL3743B_PAGE_FUNCTION,
                                    IS31FL3743B_REG_GLOBAL_CURRENT, config->global_current);
    }
    if (err == 0) {
        err = is31fl3743b_write_reg(dev, IS31FL3743B_PAGE_FUNCTION, IS31FL3743B_REG_CONFIG,
                                    config->config);
    }
    if (err < 0) {
        LOG_ERR("Configuration failed (%d)", err);
        return err;
    }

    /* The scaling page takes one value for all 198 dots, sent as 11 rows of 18. */
    uint8_t row[IS31FL3743B_CS_LINES];
    struct spi_buf rows[IS31FL3743B_SW_LINES];

    memset(row, config->scaling, sizeof(row));
    for (size_t i = 0; i < ARRAY_SIZE(rows); i++) {
        rows[i] = (struct spi_buf){.buf = row, .len = sizeof(row)};
    }

    err = is31fl3743b_write(dev, IS31FL3743B_PAGE_SCALING, IS31FL3743B_FIRST_DOT, rows,
                            ARRAY_SIZE(rows));
    if (err < 0) {
        LOG_ERR("Scaling write failed (%d)", err);
        return err;
    }

    data->powered = true;
    return 0;
}

/* Only with an SDB pin. */
static int is31fl3743b_power_down(const struct device *dev) {
    const struct is31fl3743b_config *config = dev->config;
    struct is31fl3743b_data *data = dev->data;

    data->powered = false;
    return gpio_pin_set_dt(&config->sdb, 0);
}

static bool is31fl3743b_frame_lit(const struct is31fl3743b_data *data) {
    for (size_t i = 0; i < sizeof(data->frame); i++) {
        if (data->frame[i] != 0) {
            return true;
        }
    }
    return false;
}

static int is31fl3743b_show_frame(const struct device *dev) {
    struct is31fl3743b_data *data = dev->data;
    const struct spi_buf frame = {.buf = data->frame, .len = sizeof(data->frame)};

    return is31fl3743b_write(dev, IS31FL3743B_PAGE_PWM, IS31FL3743B_FIRST_DOT, &frame, 1);
}

static int is31fl3743b_update_rgb(const struct device *dev, struct led_rgb *pixels,
                                  size_t num_pixels) {
    const struct is31fl3743b_config *config = dev->config;
    struct is31fl3743b_data *data = dev->data;
    bool white = led_rgbw_has_white(config->color_mapping, config->num_colors);
    int err;

    for (size_t i = 0; i < num_pixels; i++) {
        uint8_t *dot = &data->frame[config->pwm_registers[i] - IS31FL3743B_FIRST_DOT];

        for (size_t c = 0; c < config->num_colors; c++) {
            dot[c] = led_rgbw_channel(&pixels[i], config->color_mapping[c], white);
        }
    }

    enum pm_device_state state;

    /* While suspended, keep the frame and show it on resume. */
    if (pm_device_state_get(dev, &state) == 0 && state != PM_DEVICE_STATE_ACTIVE) {
        return 0;
    }

    if (!is31fl3743b_frame_lit(data)) {
        /* Without an SDB pin, the dark frame is shown instead. */
        if (config->sdb.port != NULL) {
            return is31fl3743b_power_down(dev);
        }
    } else if (!data->powered) {
        err = is31fl3743b_power_up(dev);
        if (err < 0) {
            return err;
        }
    }

    return is31fl3743b_show_frame(dev);
}

static size_t is31fl3743b_length(const struct device *dev) {
    const struct is31fl3743b_config *config = dev->config;

    return config->length;
}

static int is31fl3743b_init(const struct device *dev) {
    const struct is31fl3743b_config *config = dev->config;

    if (!spi_is_ready_dt(&config->bus)) {
        LOG_ERR("SPI bus not ready");
        return -ENODEV;
    }

    if (config->sdb.port != NULL) {
        if (!gpio_is_ready_dt(&config->sdb)) {
            LOG_ERR("SDB GPIO not ready");
            return -ENODEV;
        }
        /* Start in hardware shutdown. The first lit frame powers the chip up. */
        return gpio_pin_configure_dt(&config->sdb, GPIO_OUTPUT_INACTIVE);
    }

    return 0;
}

#ifdef CONFIG_PM_DEVICE
static int is31fl3743b_pm_action(const struct device *dev, enum pm_device_action action) {
    const struct is31fl3743b_config *config = dev->config;
    struct is31fl3743b_data *data = dev->data;
    int err;

    switch (action) {
    case PM_DEVICE_ACTION_SUSPEND:
        /* Shut the chip down whatever is lit. Resume shows the kept frame. */
        return config->sdb.port != NULL ? is31fl3743b_power_down(dev) : 0;
    case PM_DEVICE_ACTION_RESUME:
        if (!is31fl3743b_frame_lit(data)) {
            return 0;
        }
        err = is31fl3743b_power_up(dev);
        if (err < 0) {
            return err;
        }
        return is31fl3743b_show_frame(dev);
    default:
        return -ENOTSUP;
    }
}
#endif

static DEVICE_API(led_strip, is31fl3743b_api) = {
    .update_rgb = is31fl3743b_update_rgb,
    .length = is31fl3743b_length,
};

/* Each pixel's registers, one per colour, must lie within the frame. */
#define IS31FL3743B_PWM_REGISTER_CHECK(node, prop, idx)                                            \
    BUILD_ASSERT(IN_RANGE(DT_PROP_BY_IDX(node, prop, idx), IS31FL3743B_FIRST_DOT,                  \
                          IS31FL3743B_FIRST_DOT + IS31FL3743B_DOTS -                               \
                              DT_PROP_LEN(node, color_mapping)),                                   \
                 "pwm-registers entry out of range");

#define IS31FL3743B_DEFINE(n)                                                                      \
    BUILD_ASSERT(DT_INST_PROP_LEN(n, pwm_registers) == DT_INST_PROP(n, chain_length),              \
                 "pwm-registers needs one entry per pixel");                                       \
    DT_INST_FOREACH_PROP_ELEM(n, pwm_registers, IS31FL3743B_PWM_REGISTER_CHECK)                    \
                                                                                                   \
    static const uint8_t is31fl3743b_pwm_registers_##n[] = DT_INST_PROP(n, pwm_registers);        \
    static const uint8_t is31fl3743b_color_mapping_##n[] = DT_INST_PROP(n, color_mapping);        \
    static struct is31fl3743b_data is31fl3743b_data_##n;                                           \
                                                                                                   \
    static const struct is31fl3743b_config is31fl3743b_config_##n = {                              \
        .bus = SPI_DT_SPEC_INST_GET(n, SPI_OP_MODE_MASTER | SPI_TRANSFER_MSB | SPI_WORD_SET(8),   \
                                    0),                                                            \
        .sdb = GPIO_DT_SPEC_INST_GET_OR(n, sdb_gpios, {0}),                                        \
        .pwm_registers = is31fl3743b_pwm_registers_##n,                                            \
        .color_mapping = is31fl3743b_color_mapping_##n,                                            \
        .length = DT_INST_PROP(n, chain_length),                                                   \
        .num_colors = DT_INST_PROP_LEN(n, color_mapping),                                          \
        .config = DT_INST_PROP(n, config_register),                                                \
        .global_current = DT_INST_PROP(n, global_current),                                         \
        .pull = DT_INST_PROP(n, pull_register),                                                    \
        .scaling = DT_INST_PROP(n, scaling),                                                       \
    };                                                                                             \
                                                                                                   \
    PM_DEVICE_DT_INST_DEFINE(n, is31fl3743b_pm_action);                                            \
                                                                                                   \
    DEVICE_DT_INST_DEFINE(n, is31fl3743b_init, PM_DEVICE_DT_INST_GET(n), &is31fl3743b_data_##n,    \
                          &is31fl3743b_config_##n, POST_KERNEL, CONFIG_LED_STRIP_INIT_PRIORITY,    \
                          &is31fl3743b_api);

DT_INST_FOREACH_STATUS_OKAY(IS31FL3743B_DEFINE)
