/*
 * The controller's side of the SPI link, as a state machine with no hardware
 * calls. The radio app runs it around its SPI slave transfers, and the RP2040
 * driver's test runs it behind an emulated SPI bus.
 *
 * An SPI slave arms its reply before the host clocks a transfer. So each
 * transfer is defy_link_arm(), the transfer, then defy_link_done() with what
 * the host sent.
 *
 * SPDX-License-Identifier: MIT
 */

#ifndef DEFY_RADIO_LINK_H
#define DEFY_RADIO_LINK_H

#include <stddef.h>
#include <stdint.h>

enum defy_link_phase {
    /* Waiting for a header. */
    DEFY_LINK_HEADER,
    /* The host asked to read, and the oldest unsent message is armed. */
    DEFY_LINK_SEND,
    /* The host asked to write, and its packet comes next. */
    DEFY_LINK_RECEIVE,
};

enum defy_link_result {
    DEFY_LINK_NONE,
    /* The host has read the oldest unsent message. Drop it. */
    DEFY_LINK_SENT,
    /* A command or ACL packet for the controller, packet_len bytes from the start of rx. */
    DEFY_LINK_PACKET,
    /* A vendor command, for the radio app itself. */
    DEFY_LINK_REBOOT,
    DEFY_LINK_POWER_OFF,
    DEFY_LINK_BATTERY_REFRESH,
};

struct defy_link {
    enum defy_link_phase phase;
    /* The message length in the armed header, or the length of the armed message. */
    size_t armed_len;
    /* After DEFY_LINK_PACKET: the packet's length, with its H4 packet type. */
    size_t packet_len;
};

void defy_link_init(struct defy_link *link);

/*
 * Fills tx with the reply to the next transfer and returns its length.
 * message is the oldest unsent message with its H4 packet type, or NULL. It
 * must stay the oldest until defy_link_done() returns DEFY_LINK_SENT. tx must
 * hold DEFY_HCI_SPI_MAX_MSG_LEN bytes.
 */
size_t defy_link_arm(struct defy_link *link, const uint8_t *message, size_t len, uint8_t *tx);

/* Takes the len bytes the host sent in the transfer. */
enum defy_link_result defy_link_done(struct defy_link *link, const uint8_t *rx, size_t len);

#endif
