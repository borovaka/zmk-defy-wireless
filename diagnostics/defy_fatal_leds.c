/*
 * Fatal errors on a half's per-key LEDs. They write the IS31FL3743B through
 * the RP2040's registers, as Dygma's bootloader lights it: one register per
 * three-byte write, with the configuration, global current and scaling of
 * key_leds in the devicetree. Nothing here needs Zephyr's drivers, the kernel
 * or initialised RAM.
 *
 * A Zephyr fatal error flashes channel 0 in turn with another: channel 2 for
 * a CPU exception, channel 3 for a stack overflow, channel 1 for any other
 * reason. On the keyboard the channels are red, green, blue and white, the
 * order of key_leds' color-mapping. defy_fatal_recovery.c shows it briefly
 * and restarts the half; without CONFIG_DEFY_FATAL_RECOVERY it stays, fast,
 * alternating with the faulting address and the last HCI commands on the
 * keys.
 *
 * SPDX-License-Identifier: MIT
 */

#include <stdint.h>

#include <zephyr/arch/arm/exception.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/pinctrl.h>
#include <zephyr/dt-bindings/gpio/gpio.h>
#include <zephyr/fatal.h>
#include <zephyr/sys/util.h>
#include <zephyr/toolchain.h>

#include <hardware/address_mapped.h>
#include <hardware/regs/resets.h>
#include <hardware/regs/spi.h>
#include <hardware/structs/io_bank0.h>
#include <hardware/structs/resets.h>
#include <hardware/structs/sio.h>
#include <hardware/structs/spi.h>

#include <dygma/defy_fatal_leds.h>
#include <dygma/defy_fatal_recovery.h>
#include <dygma/defy_hci_diagnostics.h>

#define KEY_LEDS DT_NODELABEL(key_leds)
#define BUS DT_BUS(KEY_LEDS)

BUILD_ASSERT(DT_SAME_NODE(BUS, DT_NODELABEL(spi0)), "the writes below use SPI0's registers");

#define PIN_SDB DT_GPIO_PIN(KEY_LEDS, sdb_gpios)
#define PIN_LEDS_CS DT_SPI_DEV_CS_GPIOS_PIN(KEY_LEDS)

BUILD_ASSERT(DT_SAME_NODE(DT_GPIO_CTLR(KEY_LEDS, sdb_gpios), DT_NODELABEL(gpio0)));
BUILD_ASSERT(!(DT_GPIO_FLAGS(KEY_LEDS, sdb_gpios) & GPIO_ACTIVE_LOW));

/* Every chip select on the bus. All stay high, inactive, but the LED chip's during a write. */
#define CS_PIN(node, prop, idx) | BIT(DT_GPIO_PIN_BY_IDX(node, prop, idx))
#define CS_PINS (0 DT_FOREACH_PROP_ELEM(BUS, cs_gpios, CS_PIN))
#define CS_CHECK(node, prop, idx)                                                                  \
    BUILD_ASSERT(DT_SAME_NODE(DT_GPIO_CTLR_BY_IDX(node, prop, idx), DT_NODELABEL(gpio0)));         \
    BUILD_ASSERT(DT_GPIO_FLAGS_BY_IDX(node, prop, idx) & GPIO_ACTIVE_LOW);

DT_FOREACH_PROP_ELEM(BUS, cs_gpios, CS_CHECK)

/* The bus's data and clock pins with their functions, from its default pinctrl state. */
#define BUS_PINMUX(node, prop, idx) DT_PROP_BY_IDX(node, prop, idx),
#define BUS_GROUP(group) DT_FOREACH_PROP_ELEM(group, pinmux, BUS_PINMUX)

#define PAGE_PWM 0x50
#define PAGE_SCALING 0x51
#define PAGE_FUNCTION 0x52

#define REG_CONFIG 0x00
#define REG_GLOBAL_CURRENT 0x01

#define LED_CONFIG DT_PROP(KEY_LEDS, config_register)
#define LED_CURRENT DT_PROP(KEY_LEDS, global_current)
#define LED_SCALING DT_PROP(KEY_LEDS, scaling)

/* SW1-SW9 of 18 CS lines; each LED is four CS lines, and CS17-18 are unused. */
#define DOTS (9 * 18)

BUILD_ASSERT((LED_CONFIG >> 4) == 2, "the configuration must scan SW1-SW9, as DOTS assumes");

#define ON DEFY_FATAL_LEDS_LEVEL

/* A bound on every wait for the SPI, so that a stopped clock cannot hang the handler. */
#define SPIN_LIMIT 100000

/* About a millisecond of the loop in hold() at 125 MHz. */
#define LOOPS_PER_MS 25000

static void hold(uint32_t ms) {
    for (uint32_t i = 0; i < ms * LOOPS_PER_MS; i++) {
        __asm__ volatile("nop");
    }
}

static void spi_byte(uint8_t value) {
    for (int i = 0; i < SPIN_LIMIT && !(spi0_hw->sr & SPI_SSPSR_TNF_BITS); i++) {
    }
    spi0_hw->dr = value;
}

static void spi_finish(void) {
    for (int i = 0; i < SPIN_LIMIT && !(spi0_hw->sr & SPI_SSPSR_TFE_BITS); i++) {
    }
    for (int i = 0; i < SPIN_LIMIT && (spi0_hw->sr & SPI_SSPSR_BSY_BITS); i++) {
    }
    for (int i = 0; i < SPIN_LIMIT && (spi0_hw->sr & SPI_SSPSR_RNE_BITS); i++) {
        (void)spi0_hw->dr;
    }
}

static void write_reg(uint8_t page, uint8_t reg, uint8_t value) {
    sio_hw->gpio_clr = BIT(PIN_LEDS_CS);
    spi_byte(page);
    spi_byte(reg);
    spi_byte(value);
    spi_finish();
    sio_hw->gpio_set = BIT(PIN_LEDS_CS);
}

/* Pins as Dygma's bootloader sets them: every chip select high, SDB high, SPI0 at about 0.5 MHz. */
static void setup(void) {
    static const uint32_t bus_pins[] = {DT_FOREACH_CHILD(DT_PINCTRL_BY_NAME(BUS, default, 0),
                                                         BUS_GROUP)};
    const uint32_t blocks =
        RESETS_RESET_IO_BANK0_BITS | RESETS_RESET_PADS_BANK0_BITS | RESETS_RESET_SPI0_BITS;
    const uint32_t outputs = BIT(PIN_SDB) | CS_PINS;

    hw_clear_bits(&resets_hw->reset, blocks);
    for (int i = 0; i < SPIN_LIMIT && (~resets_hw->reset_done & blocks); i++) {
    }

    sio_hw->gpio_set = outputs;
    sio_hw->gpio_oe_set = outputs;
    for (unsigned int pin = 0; pin < ARRAY_SIZE(io_bank0_hw->io); pin++) {
        if (outputs & BIT(pin)) {
            io_bank0_hw->io[pin].ctrl = RP2_PINCTRL_GPIO_FUNC_SIO;
        }
    }
    for (size_t i = 0; i < ARRAY_SIZE(bus_pins); i++) {
        io_bank0_hw->io[RP2_GET_PIN_NUM(bus_pins[i])].ctrl = RP2_GET_PIN_ALT_FUNC(bus_pins[i]);
    }

    spi0_hw->cr1 = 0;
    spi0_hw->imsc = 0;
    spi0_hw->dmacr = 0;
    spi0_hw->cpsr = 250;
    /* 8-bit Motorola frames, mode 0, as the stock firmware drives the chip. */
    spi0_hw->cr0 = 7;
    spi0_hw->cr1 = SPI_SSPCR1_SSE_BITS;
}

FUNC_NO_STACK_PROTECTOR void defy_fatal_leds_show(uint8_t c0, uint8_t c1, uint8_t c2,
                                                  uint8_t c3) {
    const uint32_t cr0 = spi0_hw->cr0, cr1 = spi0_hw->cr1, cpsr = spi0_hw->cpsr;
    const uint32_t imsc = spi0_hw->imsc, dmacr = spi0_hw->dmacr;
    const uint8_t channels[] = {c0, c1, c2, c3};

    setup();
    /* No division: it can run before anything is set up. */
    for (unsigned int sw = 0, reg = 1; sw < DOTS / 18; sw++) {
        for (unsigned int cs = 0; cs < 18; cs++, reg++) {
            write_reg(PAGE_PWM, reg, cs < 16 ? channels[cs & 3] : 0);
        }
    }
    for (unsigned int reg = 1; reg <= DOTS; reg++) {
        write_reg(PAGE_SCALING, reg, LED_SCALING);
    }
    write_reg(PAGE_FUNCTION, REG_GLOBAL_CURRENT, LED_CURRENT);
    write_reg(PAGE_FUNCTION, REG_CONFIG, LED_CONFIG);

    spi0_hw->cr1 = 0;
    spi0_hw->cr0 = cr0;
    spi0_hw->cpsr = cpsr;
    spi0_hw->dmacr = dmacr;
    spi0_hw->imsc = imsc;
    spi0_hw->cr1 = cr1;
}

/* The fault report on the 35 per-key LEDs: keys 0 to 31 hold a 32-bit value, the faulting
 * address or the last HCI opcodes, and keys 32 to 34 the low bits of the fault's reason. */
#define REPORT_VALUE_BITS 32U
#define REPORT_KEY_COUNT 35U

static const uint8_t fatal_pwm_registers[] = DT_PROP(KEY_LEDS, pwm_registers);

BUILD_ASSERT(ARRAY_SIZE(fatal_pwm_registers) == REPORT_KEY_COUNT);

static inline bool report_bit(uint32_t value, unsigned int reason, unsigned int key) {
    if (key < REPORT_VALUE_BITS) {
        return ((value >> key) & 1U) != 0;
    }
    return ((reason >> (key - REPORT_VALUE_BITS)) & 1U) != 0;
}

static void show_fatal_report(uint32_t value, unsigned int reason, bool hci_page) {
    setup();
    for (unsigned int reg = 1; reg <= DOTS; reg++) {
        write_reg(PAGE_PWM, reg, 0);
        write_reg(PAGE_SCALING, reg, LED_SCALING);
    }
    for (unsigned int key = 0; key < ARRAY_SIZE(fatal_pwm_registers); key++) {
        if (report_bit(value, reason, key)) {
            uint8_t reg = fatal_pwm_registers[key];

            /* The address page in cyan, green with blue; the HCI page in yellow, red with green. */
            write_reg(PAGE_PWM, reg + 1, ON);
            write_reg(PAGE_PWM, reg + (hci_page ? 0 : 2), ON);
        }
    }
    write_reg(PAGE_FUNCTION, REG_GLOBAL_CURRENT, LED_CURRENT);
    write_reg(PAGE_FUNCTION, REG_CONFIG, LED_CONFIG);
}

FUNC_NO_STACK_PROTECTOR void k_sys_fatal_error_handler(unsigned int reason,
                                                     const struct arch_esf *esf) {
    if (IS_ENABLED(CONFIG_DEFY_FATAL_RECOVERY)) {
        defy_fatal_recover(reason);
    }

    uint32_t pc = esf != NULL ? esf->basic.pc : 0;
    struct defy_hci_diagnostics hci;

    defy_hci_diagnostics_get(&hci);
    /* The HCI page's value: the last command the driver sent, then the last reply it read. */
    uint32_t hci_word = (uint32_t)hci.tx_opcode | ((uint32_t)hci.rx_opcode << 16);

    for (;;) {
        for (int pulse = 0; pulse < 2; pulse++) {
            defy_fatal_leds_show(ON, 0, 0, 0);
            hold(250);
            defy_fatal_leds_show_reason(reason);
            hold(250);
        }
        show_fatal_report(pc, reason, false);
        hold(6000);
        show_fatal_report(hci_word, reason, true);
        hold(6000);
    }
}
