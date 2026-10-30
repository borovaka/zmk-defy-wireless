/* SPDX-License-Identifier: MIT */

#ifndef DYGMA_DEFY_UPDATER_H
#define DYGMA_DEFY_UPDATER_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/*
 * A frame is a 16-byte header, the payload and the CRC32 of both. The header
 * is little-endian: "DU", the version, the opcode, the session, the sequence,
 * the payload's length and a signed status. A reply copies the request's
 * header, sets DEFY_UPDATER_REPLY in the opcode and fills in its own length
 * and status. docs/half-updater.md describes the protocol.
 */
#define DEFY_UPDATER_VERSION 1u
#define DEFY_UPDATER_REPLY 0x80u
#define DEFY_UPDATER_VERSION_OFFSET 2u
#define DEFY_UPDATER_OPCODE_OFFSET 3u
#define DEFY_UPDATER_SESSION_OFFSET 4u
#define DEFY_UPDATER_SEQUENCE_OFFSET 8u
#define DEFY_UPDATER_LENGTH_OFFSET 12u
#define DEFY_UPDATER_STATUS_OFFSET 14u
#define DEFY_UPDATER_HEADER 16u
#define DEFY_UPDATER_CRC 4u
#define DEFY_UPDATER_PAYLOAD 260u
#define DEFY_UPDATER_FRAME (DEFY_UPDATER_HEADER + DEFY_UPDATER_PAYLOAD + DEFY_UPDATER_CRC)
#define DEFY_UPDATER_IDLE_MS 10000

struct defy_updater_info {
    uint32_t hardware;
    uint32_t flash_start;
    uint32_t version;
    uint32_t crc;
    bool valid;
};

struct defy_updater_ops {
    int (*open)(void);
    int (*probe)(uint8_t side, struct defy_updater_info *info);
    int (*page)(uint8_t side, uint32_t address, const uint8_t *data);
    int (*finish)(uint8_t side, struct defy_updater_info *info);
    int (*close)(void);
    size_t (*diagnostics)(uint8_t *output, size_t capacity);
    size_t (*battery_status)(uint8_t *output, size_t capacity);
    size_t (*dock_status)(uint8_t *output, size_t capacity);
    size_t (*key_trace)(uint32_t from, bool clear, uint8_t *output, size_t capacity);
    size_t (*pairing_status)(uint8_t *output, size_t capacity);
    /* Clears the Neuron's split pairing, then erases both halves' settings. */
    int (*reset_pairing)(void);
    size_t (*half_versions)(uint8_t *output, size_t capacity);
    /* Drops the keymap changes ZMK Studio saved and reloads the built-in keymap. */
    int (*clear_keymap_changes)(void);
};

struct defy_updater {
    const struct defy_updater_ops *ops;
    uint32_t session;
    uint32_t sequence;
    uint32_t next_address;
    uint32_t end_address;
    uint32_t transfer_crc;
    uint32_t expected_crc;
    uint32_t version;
    uint32_t program_crc;
    int64_t last_request;
    /* When to retry a failed close; -1 until the next expire sets it. */
    int64_t retry_at;
    uint16_t retry_delay;
    /* BIT(side) of each half probed, and finished. */
    uint8_t probed;
    uint8_t completed;
    uint8_t side;
    bool writing;
    bool active;
    bool closing;
    /* RESET PAIRING has run in this session. */
    bool pairing_reset;
    size_t cached_request_length;
    size_t cached_reply_length;
    uint8_t cached_request[DEFY_UPDATER_FRAME];
    uint8_t cached_reply[DEFY_UPDATER_FRAME];
};

void defy_updater_init(struct defy_updater *state, const struct defy_updater_ops *ops);
/*
 * The length of the request that starts with this header: the header, the
 * payload and the CRC. 0 unless the header has the magic and the version, is
 * not a reply and announces at most DEFY_UPDATER_PAYLOAD bytes.
 */
size_t defy_updater_frame_length(const uint8_t header[DEFY_UPDATER_HEADER]);
int defy_updater_close(struct defy_updater *state);
/*
 * Releases halves that a Neuron reset left in their bootloaders, as a
 * session's close does, retries included. The caller holds the
 * DEFY_UPDATE_HALVES claim, which a successful release gives back.
 */
int defy_updater_recover(struct defy_updater *state);
/*
 * Ends a session that lost its host or idled DEFY_UPDATER_IDLE_MS, and
 * retries a failed close when due.
 */
void defy_updater_expire(struct defy_updater *state, int64_t now, bool connected);
/*
 * Answers one request frame. Returns the reply's length, or 0 for a frame
 * that gets no reply: a bad header, status or CRC.
 */
size_t defy_updater_process(struct defy_updater *state, const uint8_t *request,
                           size_t length, uint8_t reply[DEFY_UPDATER_FRAME], int64_t now);

#endif
