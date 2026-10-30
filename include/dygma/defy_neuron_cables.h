/*
 * The Neuron's cable sockets: the board's dygma,defy-neuron-cables node, with
 * each half's SPI port and RUN line and the I2C bus to the halves'
 * bootloaders.
 *
 * SPDX-License-Identifier: MIT
 */

#ifndef DYGMA_DEFY_NEURON_CABLES_H
#define DYGMA_DEFY_NEURON_CABLES_H

#include <zephyr/devicetree.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/toolchain.h>

#define DEFY_NEURON_CABLES_NODE DT_INST(0, dygma_defy_neuron_cables)

/* An initializer for the halves' RUN lines as gpio_dt_specs: the left's, then the right's. */
#define DEFY_NEURON_RUN_LINES                                                                      \
    {                                                                                              \
        GPIO_DT_SPEC_GET_BY_IDX(DEFY_NEURON_CABLES_NODE, run_gpios, 0),                            \
        GPIO_DT_SPEC_GET_BY_IDX(DEFY_NEURON_CABLES_NODE, run_gpios, 1),                            \
    }

#if DT_HAS_COMPAT_STATUS_OKAY(dygma_defy_neuron_cables)
BUILD_ASSERT(DT_NUM_INST_STATUS_OKAY(dygma_defy_neuron_cables) == 1,
             "The Neuron has one cables node");
BUILD_ASSERT(DT_PROP_LEN(DEFY_NEURON_CABLES_NODE, spi_ports) == 2,
             "The cables have two SPI ports, socket 1's and socket 2's");
BUILD_ASSERT(DT_PROP_LEN(DEFY_NEURON_CABLES_NODE, run_gpios) == 2,
             "The cables have two RUN lines, the left half's and the right's");
/* The lines stay inputs without pull and are only ever driven low. */
BUILD_ASSERT(DT_GPIO_FLAGS_BY_IDX(DEFY_NEURON_CABLES_NODE, run_gpios, 0) == 0 &&
                 DT_GPIO_FLAGS_BY_IDX(DEFY_NEURON_CABLES_NODE, run_gpios, 1) == 0,
             "The RUN lines take no flags: active high, without pull");
/* The half updater's bootloader client drives each half's RUN line on its own. */
BUILD_ASSERT(!DT_SAME_NODE(DT_GPIO_CTLR_BY_IDX(DEFY_NEURON_CABLES_NODE, run_gpios, 0),
                           DT_GPIO_CTLR_BY_IDX(DEFY_NEURON_CABLES_NODE, run_gpios, 1)) ||
                 DT_GPIO_PIN_BY_IDX(DEFY_NEURON_CABLES_NODE, run_gpios, 0) !=
                     DT_GPIO_PIN_BY_IDX(DEFY_NEURON_CABLES_NODE, run_gpios, 1),
             "The halves' RUN lines are two pins");
#endif

#endif
