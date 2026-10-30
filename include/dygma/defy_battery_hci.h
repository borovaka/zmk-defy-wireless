/*
 * A battery sample as the radio's HCI vendor event: apps/defy_radio encodes
 * it and drivers/bluetooth/defy_battery_hci.c decodes it on the RP2040. The
 * event has DEFY_BATTERY_HCI_PAYLOAD_SIZE bytes of parameters, little-endian:
 *
 *    0  the vendor event code, DEFY_HCI_SPI_EVT_BATTERY
 *    2  DEFY_BATTERY_VERSION
 *    3  the status, enum defy_battery_status
 *    4  the sequence, 32 bits
 *    8  the age in milliseconds, 32 bits
 *   12  the eight words, 16 bits each
 *
 * The HCI link writes and checks the event code, and these helpers the rest.
 * Plain C, with no Zephyr dependencies.
 *
 * SPDX-License-Identifier: MIT
 */

#ifndef DYGMA_DEFY_BATTERY_HCI_H
#define DYGMA_DEFY_BATTERY_HCI_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <dygma/defy_battery.h>
#include <dygma/defy_wired_frame.h>

#define DEFY_BATTERY_HCI_VERSION_OFFSET 2u
#define DEFY_BATTERY_HCI_STATUS_OFFSET 3u
#define DEFY_BATTERY_HCI_SEQUENCE_OFFSET 4u
#define DEFY_BATTERY_HCI_AGE_OFFSET 8u
#define DEFY_BATTERY_HCI_WORDS_OFFSET 12u

_Static_assert(DEFY_BATTERY_HCI_WORDS_OFFSET + 2u * DEFY_BATTERY_WORD_COUNT ==
                   DEFY_BATTERY_HCI_PAYLOAD_SIZE,
               "the words end the payload");

/* Whether a sample can be sent: no sample has sequence 0, and the status must be known. */
static inline bool defy_battery_hci_sample_valid(const struct defy_battery_sample *sample) {
    return sample->sequence != 0 && sample->status <= DEFY_BATTERY_SAMPLE_ANALOG_ERROR;
}

/* Writes sample into payload, all but the event code. */
static inline void defy_battery_hci_encode(uint8_t payload[DEFY_BATTERY_HCI_PAYLOAD_SIZE],
                                           const struct defy_battery_sample *sample) {
    payload[DEFY_BATTERY_HCI_VERSION_OFFSET] = DEFY_BATTERY_VERSION;
    payload[DEFY_BATTERY_HCI_STATUS_OFFSET] = sample->status;
    defy_wired_put32(&payload[DEFY_BATTERY_HCI_SEQUENCE_OFFSET], sample->sequence);
    defy_wired_put32(&payload[DEFY_BATTERY_HCI_AGE_OFFSET], sample->age_ms);
    for (size_t i = 0; i < DEFY_BATTERY_WORD_COUNT; i++) {
        defy_wired_put16(&payload[DEFY_BATTERY_HCI_WORDS_OFFSET + 2 * i], sample->words[i]);
    }
}

/* Writes a new age into an encoded payload. */
static inline void defy_battery_hci_set_age(uint8_t payload[DEFY_BATTERY_HCI_PAYLOAD_SIZE],
                                            uint32_t age_ms) {
    defy_wired_put32(&payload[DEFY_BATTERY_HCI_AGE_OFFSET], age_ms);
}

/*
 * Reads the length bytes of payload into sample. Fails for a length or
 * version other than this layout's, or a sample that could not be sent.
 */
static inline bool defy_battery_hci_decode(const uint8_t *payload, size_t length,
                                           struct defy_battery_sample *sample) {
    if (length != DEFY_BATTERY_HCI_PAYLOAD_SIZE ||
        payload[DEFY_BATTERY_HCI_VERSION_OFFSET] != DEFY_BATTERY_VERSION) {
        return false;
    }
    *sample = (struct defy_battery_sample){
        .sequence = defy_wired_get32(&payload[DEFY_BATTERY_HCI_SEQUENCE_OFFSET]),
        .age_ms = defy_wired_get32(&payload[DEFY_BATTERY_HCI_AGE_OFFSET]),
        .status = payload[DEFY_BATTERY_HCI_STATUS_OFFSET],
    };
    for (size_t i = 0; i < DEFY_BATTERY_WORD_COUNT; i++) {
        sample->words[i] = defy_wired_get16(&payload[DEFY_BATTERY_HCI_WORDS_OFFSET + 2 * i]);
    }
    return defy_battery_hci_sample_valid(sample);
}

/* On the RP2040, in drivers/bluetooth/defy_battery_hci.c: the half's latest sample and, from
 * get_generation(), the radio's generation, which advances at each radio restart. refresh()
 * asks the radio for a new sample. */
int defy_battery_hci_get(struct defy_battery_sample *sample);
int defy_battery_hci_refresh(void);
int defy_battery_hci_get_generation(struct defy_battery_sample *sample, uint32_t *generation);
void defy_battery_hci_reset(void);

#endif
