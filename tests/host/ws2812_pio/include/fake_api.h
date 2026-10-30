/* SPDX-License-Identifier: MIT */

#pragma once

#include <assert.h>
#include <errno.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

struct device {
	const char *name;
	const void *config;
	void *data;
	bool ready;
};

struct pinctrl_dev_config {
	int unused;
};

struct led_rgb {
	uint8_t r;
	uint8_t g;
	uint8_t b;
};

struct led_strip_driver_api {
	int (*update_rgb)(const struct device *, struct led_rgb *, size_t);
	size_t (*length)(const struct device *);
};

typedef struct fake_pio *PIO;

typedef struct {
	unsigned int wrap_bottom;
	unsigned int wrap_top;
	unsigned int sideset_bits;
	unsigned int pin;
	unsigned int pull_threshold;
	float clkdiv;
} pio_sm_config;

struct pio_program {
	const uint16_t *instructions;
	uint8_t length;
	int8_t origin;
};

#define DEVICE_API(type, name) struct type##_driver_api name
#define DT_INST_FOREACH_STATUS_OKAY(fn)
#define LOG_MODULE_REGISTER(name, level)
#define LOG_ERR(...)
#define PINCTRL_STATE_DEFAULT 0
#define PIO_FIFO_JOIN_TX 1
#define LED_COLOR_ID_WHITE 0
#define LED_COLOR_ID_RED 1
#define LED_COLOR_ID_GREEN 2
#define LED_COLOR_ID_BLUE 3

bool device_is_ready(const struct device *dev);
uint32_t sys_clock_hw_cycles_per_sec(void);
int k_usleep(int32_t us);
int pinctrl_apply_state(const struct pinctrl_dev_config *config, uint8_t state);
PIO pio_rpi_pico_get_pio(const struct device *dev);
pio_sm_config pio_get_default_sm_config(void);
int pio_claim_unused_sm(PIO pio, bool required);
void pio_sm_unclaim(PIO pio, unsigned int sm);
int pio_add_program(PIO pio, const struct pio_program *program);
void pio_remove_program(PIO pio, const struct pio_program *program, unsigned int offset);
void sm_config_set_wrap(pio_sm_config *config, unsigned int bottom, unsigned int top);
void sm_config_set_sideset(pio_sm_config *config, unsigned int bits, bool optional, bool pindirs);
void sm_config_set_sideset_pins(pio_sm_config *config, unsigned int pin);
void sm_config_set_out_shift(pio_sm_config *config, bool right, bool autopull, unsigned int threshold);
void sm_config_set_fifo_join(pio_sm_config *config, unsigned int join);
void sm_config_set_clkdiv(pio_sm_config *config, float div);
int pio_sm_set_consecutive_pindirs(PIO pio, unsigned int sm, unsigned int pin,
				 unsigned int count, bool out);
int pio_sm_init(PIO pio, unsigned int sm, unsigned int pc, const pio_sm_config *config);
void pio_sm_set_enabled(PIO pio, unsigned int sm, bool enabled);
void pio_sm_put_blocking(PIO pio, unsigned int sm, uint32_t value);
