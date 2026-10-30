/*
 * The last command the RP2040's HCI driver was asked to send, other than Host
 * Number of Completed Packets, and the opcode of the last reply it read, for
 * fatal error reports.
 *
 * SPDX-License-Identifier: MIT
 */

#ifndef DYGMA_DEFY_HCI_DIAGNOSTICS_H
#define DYGMA_DEFY_HCI_DIAGNOSTICS_H

#include <stdint.h>

struct defy_hci_diagnostics {
    uint16_t tx_opcode;
    uint16_t rx_opcode;
};

#ifdef CONFIG_DEFY_BT_HCI_SPI
void defy_hci_diagnostics_get(struct defy_hci_diagnostics *result);
#else
static inline void defy_hci_diagnostics_get(struct defy_hci_diagnostics *result) {
    *result = (struct defy_hci_diagnostics){0};
}
#endif

#endif
