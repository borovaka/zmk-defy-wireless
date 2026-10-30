/*
 * The SPI link between a Defy half's RP2040, the Bluetooth host, and its
 * nRF52820, the controller. The framing is that of Zephyr's HCI SPI driver and
 * hci_spi sample: every transfer starts with a 5-byte header exchange. The
 * host sends SPI_READ or SPI_WRITE. The controller answers READY_NOW,
 * SANITY_CHECK, 0, the length of its oldest unsent message, 0. A read then
 * clocks that message out; a write clocks one H4 packet in.
 *
 * On top of that come a ready event the controller sends once it has booted,
 * and vendor commands the controller handles itself.
 *
 * SPDX-License-Identifier: MIT
 */

#ifndef DYGMA_DEFY_HCI_SPI_H
#define DYGMA_DEFY_HCI_SPI_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <zephyr/bluetooth/hci_types.h>
#include <zephyr/sys/byteorder.h>

#define DEFY_HCI_SPI_WRITE 0x0a
#define DEFY_HCI_SPI_READ 0x0b
#define DEFY_HCI_SPI_READY_NOW 0x02
#define DEFY_HCI_SPI_SANITY_CHECK 0x02

#define DEFY_HCI_SPI_HEADER_LEN 5
#define DEFY_HCI_SPI_HEADER_TOREAD 3

/* nRF52 SPIS transfers at most 255 bytes, and the header's length is a byte. */
#define DEFY_HCI_SPI_MAX_MSG_LEN 255

/* Vendor event codes: the ready event, and a battery sample (dygma/defy_battery_hci.h). */
#define DEFY_HCI_SPI_EVT_READY 0x0001
#define DEFY_HCI_SPI_EVT_BATTERY 0x0002

/* Vendor commands. Zephyr's own vendor commands stay below OCF 0x100. */
#define DEFY_HCI_SPI_OP_REBOOT BT_OP(BT_OGF_VS, 0x3f0)
#define DEFY_HCI_SPI_OP_POWER_OFF BT_OP(BT_OGF_VS, 0x3f1)
#define DEFY_HCI_SPI_OP_BATTERY_REFRESH BT_OP(BT_OGF_VS, 0x3f2)

/*
 * Asks the radio for a battery sample, outside the Bluetooth host: the host
 * allocates commands without a timeout and sends them from the system work
 * queue, so a request from a system work item could wait forever. Waits at
 * most about 140 ms for the link, and the driver drops the reply. Returns
 * -EAGAIN while the link is down and -EBUSY if the link stays in use.
 */
int defy_hci_spi_battery_refresh(void);

/*
 * The host waits this long for the power-off ack, from the start of its write:
 * the write, the messages queued ahead of the ack and the ack itself.
 */
#define DEFY_HCI_SPI_POWER_OFF_HOST_TIMEOUT_MS 80
/* The radio powers off once the host has read the ack, or this long after queueing it. */
#define DEFY_HCI_SPI_POWER_OFF_RADIO_TIMEOUT_MS 200

/* The ready event: an HCI event, without the H4 packet type. */
#define DEFY_HCI_SPI_READY_EVENT                                                                   \
    {BT_HCI_EVT_VENDOR, 2, DEFY_HCI_SPI_EVT_READY & 0xff, DEFY_HCI_SPI_EVT_READY >> 8}

/* Command Complete for one of the vendor commands, without the H4 packet type. */
#define DEFY_HCI_SPI_COMMAND_COMPLETE(opcode, status)                                              \
    {BT_HCI_EVT_CMD_COMPLETE, 4, 1, (opcode) & 0xff, (opcode) >> 8, (status)}

/* A vendor command with no parameters, with the H4 packet type. */
#define DEFY_HCI_SPI_COMMAND(opcode) {BT_HCI_H4_CMD, (opcode) & 0xff, (opcode) >> 8, 0}

/* Whether msg, with its H4 packet type, is the ready event. */
static inline bool defy_hci_spi_is_ready_event(const uint8_t *msg, size_t len) {
    return len >= 5 && msg[0] == BT_HCI_H4_EVT && msg[1] == BT_HCI_EVT_VENDOR && msg[2] >= 2 &&
           sys_get_le16(&msg[3]) == DEFY_HCI_SPI_EVT_READY;
}

/* A command's reply: Command Complete or Command Status. */
struct defy_hci_spi_reply {
    uint8_t event;
    uint16_t opcode;
    uint8_t status;
};

/*
 * Whether msg, with its H4 packet type, is a whole Command Complete or
 * Command Status event with a status, and if so what it says. Command
 * Complete carries the status as its first return parameter.
 */
static inline bool defy_hci_spi_parse_reply(const uint8_t *msg, size_t len,
                                            struct defy_hci_spi_reply *reply) {
    const struct bt_hci_evt_hdr *hdr = (const struct bt_hci_evt_hdr *)&msg[1];
    const uint8_t *params = &msg[1 + sizeof(*hdr)];

    if (len < 1 + sizeof(*hdr) || msg[0] != BT_HCI_H4_EVT ||
        len < 1 + sizeof(*hdr) + hdr->len) {
        return false;
    }
    if (hdr->evt == BT_HCI_EVT_CMD_COMPLETE &&
        hdr->len > sizeof(struct bt_hci_evt_cmd_complete)) {
        const struct bt_hci_evt_cmd_complete *complete =
            (const struct bt_hci_evt_cmd_complete *)params;

        reply->opcode = sys_le16_to_cpu(complete->opcode);
        reply->status = params[sizeof(*complete)];
    } else if (hdr->evt == BT_HCI_EVT_CMD_STATUS &&
               hdr->len >= sizeof(struct bt_hci_evt_cmd_status)) {
        const struct bt_hci_evt_cmd_status *status = (const struct bt_hci_evt_cmd_status *)params;

        reply->opcode = sys_le16_to_cpu(status->opcode);
        reply->status = status->status;
    } else {
        return false;
    }
    reply->event = hdr->evt;
    return true;
}

#ifdef CONFIG_DEFY_RADIO_INSTALL
/*
 * The RP2040's driver: looks for rf_bootloader as the RP2040 boots and
 * installs the radio firmware there if needed. The driver runs it before ZMK
 * starts; tests call it directly. Once a look has met the radio, later calls
 * return at once.
 */
void defy_hci_spi_catch_bootloader(void);
#endif

#ifdef CONFIG_ZTEST
/* Returns the driver to its state before open(), so that a test can open it again. */
void defy_hci_spi_test_reset(void);
#endif

#endif
