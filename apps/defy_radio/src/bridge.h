/*
 * What main.c, the HCI bridge, shares with the rest of the radio app: the SPI
 * link's devicetree nodes, the chip select line from the RP2040 and the queue
 * of messages for the host.
 *
 * SPDX-License-Identifier: MIT
 */

#ifndef DEFY_RADIO_BRIDGE_H
#define DEFY_RADIO_BRIDGE_H

#include <stdbool.h>

#include <zephyr/devicetree.h>
#include <zephyr/drivers/gpio.h>

/* The SPI slave the RP2040 clocks, and its HCI node with data ready. */
#define DEFY_RADIO_HCI_NODE DT_COMPAT_GET_ANY_STATUS_OKAY(zephyr_bt_hci_spi_slave)
#define DEFY_RADIO_SPI_NODE DT_BUS(DEFY_RADIO_HCI_NODE)

/* The SPI slave's chip select from the RP2040. Going low wakes the radio from System OFF. */
extern const struct gpio_dt_spec defy_radio_chip_select;

struct net_buf;

/*
 * Queues buf, an HCI event or ACL packet without its H4 packet type, for the
 * host. While the radio powers off only control messages, such as the
 * power-off reply, are queued, and anything else is freed.
 */
void defy_radio_queue_for_host(struct net_buf *buf, bool control);

#endif
