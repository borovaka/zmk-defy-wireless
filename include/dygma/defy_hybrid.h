/* SPDX-License-Identifier: MIT */

#ifndef DYGMA_DEFY_HYBRID_H
#define DYGMA_DEFY_HYBRID_H

/*
 * The hybrid split transport: each half keeps its cable and its Bluetooth link
 * up, and the Neuron picks one for each half. docs/automatic-transports.md
 * describes it.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* The split diagnostics (opcode 8): an 8-byte header, then a
 * 48-byte record for each of the two split slots. */
#define DEFY_HYBRID_BLE_DIAGNOSTICS_HEADER_SIZE 8u
#define DEFY_HYBRID_BLE_DIAGNOSTICS_RECORD_SIZE 48u
#define DEFY_HYBRID_BLE_DIAGNOSTICS_SIZE 104u

/* Byte 1 of a split diagnostics record. The values are on the wire: add new
 * ones at the end. */
enum defy_hybrid_ble_phase {
    DEFY_HYBRID_BLE_IDLE,
    DEFY_HYBRID_BLE_CONNECTED,
    DEFY_HYBRID_BLE_SERVICE_DISCOVERY,
    DEFY_HYBRID_BLE_CHARACTERISTIC_DISCOVERY,
    DEFY_HYBRID_BLE_CCC_DISCOVERY,
    DEFY_HYBRID_BLE_SUBSCRIBING,
    DEFY_HYBRID_BLE_SUBSCRIBED,
    DEFY_HYBRID_BLE_REPORTING,
    DEFY_HYBRID_BLE_SUBSCRIPTION_REMOVED,
    DEFY_HYBRID_BLE_SETUP_FAILED,
    DEFY_HYBRID_BLE_RECOVERY,
};

/* Into the hybrid central: the half on this Bluetooth slot and connection
 * epoch claims a side that conflicts with its keys or with the other half.
 * The central releases its keys and passes the rejection on to
 * defy_hybrid_ble_peer_rejected(). */
void defy_hybrid_central_ble_reject(uint8_t slot, uint32_t epoch);
/* Opcode 8's payload: DEFY_HYBRID_BLE_DIAGNOSTICS_SIZE bytes, or 0 when
 * capacity is smaller. */
size_t defy_hybrid_ble_diagnostics(uint8_t *out, size_t capacity);
bool defy_hybrid_central_is_maintenance(void);
/*
 * true pauses key input on both links and releases both halves' keys; false
 * resumes them. It waits on the system work queue, so it fails with
 * -EWOULDBLOCK from an ISR or that queue, and with -EBUSY while another call
 * runs. Each wait is limited to 500 ms; after a failed wait maintenance stays
 * on until a later call succeeds. See docs/half-updater.md.
 */
int defy_hybrid_central_set_maintenance(bool value);

/*
 * For half sleep, in power/defy_half_sleep.c. prepare_sleep() quiesces the
 * half's transports once no key has been pressed for min_idle_ms, none is
 * held and the cable is down: 0 once they are, -EINPROGRESS while Bluetooth
 * stops, and -EBUSY or -ETIMEDOUT when the half stays awake.
 * cancel_sleep() brings them back.
 */
int defy_hybrid_peripheral_prepare_sleep(int64_t min_idle_ms);
void defy_hybrid_peripheral_cancel_sleep(void);

#endif
