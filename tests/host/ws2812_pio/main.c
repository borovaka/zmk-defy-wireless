/* SPDX-License-Identifier: MIT */

#include <string.h>

#include "../../../drivers/led_strip/ws2812_rpi_pico_pio.c"

struct fake_pio {
	int offset;
	int sm;
	unsigned int add_count;
	unsigned int remove_count;
	unsigned int init_count;
	unsigned int pc;
	uint32_t clock_hz;
	uint32_t last_pixel;
	int32_t sleep_us;
	bool enabled;
	pio_sm_config config;
};

static struct fake_pio pio;
static struct device hardware = {.name = "pio"};
static struct pinctrl_dev_config pinctrl;
static const uint8_t mapping[] = {LED_COLOR_ID_GREEN, LED_COLOR_ID_RED,
				  LED_COLOR_ID_BLUE, LED_COLOR_ID_WHITE};
static const uint16_t instructions[] = {0x6221, 0x1123, 0x1400, 0x0400};
static struct ws2812_rpi_pico_pio_data program_data;
static const struct ws2812_rpi_pico_pio_config program_config = {
	.piodev = &hardware,
	.pcfg = &pinctrl,
	.program = {.instructions = instructions, .length = 4, .origin = -1},
};
static struct device program = {
	.name = "ws2812-program", .data = &program_data, .config = &program_config,
};
static struct ws2812_led_strip_data strip_data;
static struct ws2812_led_strip_config strip_config = {
	.piodev = &hardware,
	.program_dev = &program,
	.gpio_pin = 13,
	.num_colors = 4,
	.length = 53,
	.frequency = 800000,
	.color_mapping = mapping,
	.reset_delay = 280,
	.cycles_per_bit = 10,
};
static const struct device strip = {
	.name = "underglow", .data = &strip_data, .config = &strip_config,
};

bool device_is_ready(const struct device *dev) { return dev->ready; }
uint32_t sys_clock_hw_cycles_per_sec(void) { return pio.clock_hz; }
int k_usleep(int32_t us) { pio.sleep_us = us; return 0; }

int pinctrl_apply_state(const struct pinctrl_dev_config *config, uint8_t state)
{
	assert(config == &pinctrl);
	assert(state == PINCTRL_STATE_DEFAULT);
	return 0;
}

PIO pio_rpi_pico_get_pio(const struct device *dev)
{
	assert(dev == &hardware);
	return &pio;
}

pio_sm_config pio_get_default_sm_config(void) { return (pio_sm_config){0}; }

int pio_claim_unused_sm(PIO hw, bool required)
{
	assert(hw == &pio && !required);
	return pio.sm;
}

void pio_sm_unclaim(PIO hw, unsigned int sm)
{
	assert(hw == &pio && sm == (unsigned int)pio.sm);
}

int pio_add_program(PIO hw, const struct pio_program *loaded)
{
	assert(hw == &pio && loaded == &program_config.program);
	pio.add_count++;
	return pio.offset;
}

void pio_remove_program(PIO hw, const struct pio_program *loaded, unsigned int offset)
{
	assert(hw == &pio && loaded == &program_config.program);
	assert(offset == (unsigned int)pio.offset);
	pio.remove_count++;
}

void sm_config_set_wrap(pio_sm_config *config, unsigned int bottom, unsigned int top)
{
	assert(bottom <= top && top < 32);
	config->wrap_bottom = bottom;
	config->wrap_top = top;
}

void sm_config_set_sideset(pio_sm_config *config, unsigned int bits, bool optional, bool pindirs)
{
	assert(!optional && !pindirs);
	config->sideset_bits = bits;
}

void sm_config_set_sideset_pins(pio_sm_config *config, unsigned int pin) { config->pin = pin; }

void sm_config_set_out_shift(pio_sm_config *config, bool right, bool autopull, unsigned int threshold)
{
	assert(!right && autopull);
	config->pull_threshold = threshold;
}

void sm_config_set_fifo_join(pio_sm_config *config, unsigned int join)
{
	(void)config;
	assert(join == PIO_FIFO_JOIN_TX);
}

void sm_config_set_clkdiv(pio_sm_config *config, float div) { config->clkdiv = div; }

int pio_sm_set_consecutive_pindirs(PIO hw, unsigned int sm, unsigned int pin,
				 unsigned int count, bool out)
{
	assert(hw == &pio && sm == (unsigned int)pio.sm);
	assert(pin == strip_config.gpio_pin && count == 1 && out);
	return 0;
}

int pio_sm_init(PIO hw, unsigned int sm, unsigned int pc, const pio_sm_config *config)
{
	assert(hw == &pio && sm == (unsigned int)pio.sm);
	assert(pc < 32);
	pio.init_count++;
	pio.pc = pc;
	pio.config = *config;
	return 0;
}

void pio_sm_set_enabled(PIO hw, unsigned int sm, bool enabled)
{
	assert(hw == &pio && sm == (unsigned int)pio.sm);
	pio.enabled = enabled;
}

void pio_sm_put_blocking(PIO hw, unsigned int sm, uint32_t value)
{
	assert(hw == &pio && sm == (unsigned int)pio.sm && pio.enabled);
	pio.last_pixel = value;
}

static void reset(void)
{
	memset(&pio, 0, sizeof(pio));
	pio.offset = 28;
	pio.sm = 2;
	pio.clock_hz = 125000000;
	hardware.ready = true;
	program.ready = false;
	program_data.offset = UINT32_MAX;
	strip_data.sm = UINT32_MAX;
	strip_config.num_colors = 4;
}

static void load_program(void)
{
	assert(ws2812_rpi_pico_pio_init(&program) == 0);
	program.ready = true;
}

static void test_allocation_offsets(void)
{
	const int offsets[] = {0, 9, 28};
	for (size_t i = 0; i < sizeof(offsets) / sizeof(offsets[0]); i++) {
		reset();
		pio.offset = offsets[i];
		load_program();
		assert(ws2812_led_strip_init(&strip) == 0);
		assert(program_data.offset == (unsigned int)offsets[i]);
		assert(pio.pc == (unsigned int)offsets[i]);
		assert(pio.config.wrap_bottom == pio.pc);
		assert(pio.config.wrap_top == pio.pc + 3);
		assert(pio.config.pin == 13 && pio.config.sideset_bits == 1);
		assert(pio.config.pull_threshold == 32);
		assert(pio.config.clkdiv == 15.625f);
		assert(strip_data.sm == 2 && pio.enabled);
		assert(pio.add_count == 1 && pio.init_count == 1 && pio.remove_count == 0);
	}
	/* The Defy's underglow is GRBW, as in defy.dtsi. Zephyr's driver sent 00 22 11 00. */
	struct led_rgb pixel = {.r = 0x11, .g = 0x22};
	assert(ws2812_led_strip_api.update_rgb(&strip, &pixel, 1) == 0);
	assert(pio.last_pixel == 0x22110000);
}

static void test_low_clock_and_rgb_output(void)
{
	reset();
	pio.clock_hz = 48000000;
	strip_config.num_colors = 3;
	load_program();
	assert(ws2812_led_strip_init(&strip) == 0);
	assert(pio.config.clkdiv == 6.0f && pio.config.pull_threshold == 24);
	struct led_rgb pixel = {.r = 0x11, .g = 0x22, .b = 0x33};
	assert(ws2812_led_strip_api.update_rgb(&strip, &pixel, 1) == 0);
	assert(pio.last_pixel == 0x22113300 && pio.sleep_us == 280);
	assert(ws2812_led_strip_api.length(&strip) == 53);
}

int main(void)
{
	test_allocation_offsets();
	test_low_clock_and_rgb_output();
	return 0;
}
