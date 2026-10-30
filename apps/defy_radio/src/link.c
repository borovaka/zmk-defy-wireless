/*
 * The controller's side of the SPI link, a state machine with no hardware
 * calls; link.h describes it.
 *
 * SPDX-License-Identifier: MIT
 */

#include <string.h>

#include <zephyr/bluetooth/hci_types.h>
#include <zephyr/sys/byteorder.h>

#include <dygma/defy_hci_spi.h>

#include "link.h"

/* The first byte the host reads in a write tells it the controller was armed. */
#define WRITE_READY 0xff

void defy_link_init(struct defy_link *link) {
    link->phase = DEFY_LINK_HEADER;
    link->armed_len = 0;
    link->packet_len = 0;
}

static bool is_header(const uint8_t *rx, size_t len) {
    return len == DEFY_HCI_SPI_HEADER_LEN &&
           (rx[0] == DEFY_HCI_SPI_READ || rx[0] == DEFY_HCI_SPI_WRITE) && rx[1] == 0 &&
           rx[2] == 0 && rx[3] == 0 && rx[4] == 0;
}

size_t defy_link_arm(struct defy_link *link, const uint8_t *message, size_t len, uint8_t *tx) {
    if (link->phase == DEFY_LINK_SEND && message != NULL) {
        memcpy(tx, message, len);
        link->armed_len = len;
        return len;
    }

    if (link->phase == DEFY_LINK_RECEIVE) {
        tx[0] = WRITE_READY;
        return 1;
    }

    link->phase = DEFY_LINK_HEADER;
    link->armed_len = message != NULL ? len : 0;
    tx[0] = DEFY_HCI_SPI_READY_NOW;
    tx[1] = DEFY_HCI_SPI_SANITY_CHECK;
    tx[2] = 0;
    tx[3] = link->armed_len;
    tx[4] = 0;
    return DEFY_HCI_SPI_HEADER_LEN;
}

static enum defy_link_result take_packet(struct defy_link *link, const uint8_t *rx, size_t len) {
    if (len >= 1 + BT_HCI_CMD_HDR_SIZE && rx[0] == BT_HCI_H4_CMD &&
        len >= 1 + BT_HCI_CMD_HDR_SIZE + rx[3]) {
        link->packet_len = 1 + BT_HCI_CMD_HDR_SIZE + rx[3];

        switch (sys_get_le16(&rx[1])) {
        case DEFY_HCI_SPI_OP_REBOOT:
            return DEFY_LINK_REBOOT;
        case DEFY_HCI_SPI_OP_POWER_OFF:
            return DEFY_LINK_POWER_OFF;
        case DEFY_HCI_SPI_OP_BATTERY_REFRESH:
            return rx[3] == 0 ? DEFY_LINK_BATTERY_REFRESH : DEFY_LINK_PACKET;
        default:
            return DEFY_LINK_PACKET;
        }
    }

    if (len >= 1 + BT_HCI_ACL_HDR_SIZE && rx[0] == BT_HCI_H4_ACL &&
        len >= 1 + BT_HCI_ACL_HDR_SIZE + sys_get_le16(&rx[3])) {
        link->packet_len = 1 + BT_HCI_ACL_HDR_SIZE + sys_get_le16(&rx[3]);
        return DEFY_LINK_PACKET;
    }

    return DEFY_LINK_NONE;
}

enum defy_link_result defy_link_done(struct defy_link *link, const uint8_t *rx, size_t len) {
    enum defy_link_phase phase = link->phase;

    link->phase = DEFY_LINK_HEADER;

    if (is_header(rx, len)) {
        /*
         * A header after a header asks for the next step. A header in place
         * of a message or packet means the host started over part-way through
         * an exchange: it got the wrong reply, so it asks again, and the
         * message stays unsent.
         */
        if (phase == DEFY_LINK_HEADER) {
            if (rx[0] == DEFY_HCI_SPI_READ && link->armed_len > 0) {
                link->phase = DEFY_LINK_SEND;
            } else if (rx[0] == DEFY_HCI_SPI_WRITE) {
                link->phase = DEFY_LINK_RECEIVE;
            }
        }
        return DEFY_LINK_NONE;
    }

    switch (phase) {
    case DEFY_LINK_SEND:
        /*
         * The host reads exactly the length in the header. Anything else,
         * such as a wake pulse, is not a read.
         */
        return len == link->armed_len ? DEFY_LINK_SENT : DEFY_LINK_NONE;
    case DEFY_LINK_RECEIVE:
        return take_packet(link, rx, len);
    default:
        /* A wake pulse, or noise. */
        return DEFY_LINK_NONE;
    }
}
