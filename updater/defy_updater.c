/* SPDX-License-Identifier: MIT */

#include <errno.h>
#include <string.h>

#include <zephyr/sys/byteorder.h>
#include <zephyr/sys/crc.h>
#include <zephyr/sys/util.h>
#include <dygma/defy_build_info.h>
#include <dygma/defy_half_package.h>
#include <dygma/defy_updater.h>
#include <dygma/defy_update_guard.h>

enum {
    HELLO = 1, OPEN, PROBE, START, PAGE, FINISH, CLOSE, DIAGNOSTICS, BATTERY_STATUS, DOCK_STATUS,
    KEY_TRACE, PAIRING, RESET_PAIRING, VERSION, HALF_VERSIONS, CLEAR_KEYMAP_CHANGES,
};

/* HELLO's reply: the magic, then the sizes, the flash bounds and the idle limit. */
#define HELLO_MAGIC "DEFYUPD1"
#define HELLO_REPLY 24u

/* PROBE's reply and START's request: the side and three bytes, then four words. */
#define PROBE_REPLY 20u
#define START_REQUEST 20u

/* KEY_TRACE's request: the first record wanted (uint32), then flags. */
#define KEY_TRACE_REQUEST 5u
#define KEY_TRACE_CLEAR 1u

/* PAGE's request: the address (uint32), then the page. */
#define PAGE_REQUEST (4u + DEFY_HALF_PAGE_SIZE)

/* VERSION's reply: the format and three zero bytes, then the module version, the source and the
 * commit, each as a length byte and that many ASCII bytes. */
#define VERSION_FORMAT 1u
#define VERSION_MODULE_MAX 32u
#define VERSION_SOURCE_MAX 140u
#define VERSION_COMMIT_MAX 72u
#define VERSION_REPLY_MAX (4u + 3u + VERSION_MODULE_MAX + VERSION_SOURCE_MAX + VERSION_COMMIT_MAX)

/* The probed and completed masks with both halves set. */
#define BOTH_SIDES (BIT(DEFY_WIRED_LEFT) | BIT(DEFY_WIRED_RIGHT))

/* A failed close is retried after 100 ms, then twice as long each time, at most 1 s apart. */
#define CLOSE_RETRY_FIRST_MS 100
#define CLOSE_RETRY_MAX_MS 1000

BUILD_ASSERT(PAGE_REQUEST <= DEFY_UPDATER_PAYLOAD);
BUILD_ASSERT(sizeof(DEFY_BUILD_MODULE_VERSION) - 1 <= VERSION_MODULE_MAX);
BUILD_ASSERT(sizeof(DEFY_BUILD_SOURCE) - 1 <= VERSION_SOURCE_MAX);
BUILD_ASSERT(sizeof(DEFY_BUILD_COMMIT) - 1 <= VERSION_COMMIT_MAX);
BUILD_ASSERT(VERSION_REPLY_MAX <= DEFY_UPDATER_PAYLOAD);

void defy_updater_init(struct defy_updater *state, const struct defy_updater_ops *ops) {
    memset(state, 0, sizeof(*state));
    state->ops = ops;
}

size_t defy_updater_frame_length(const uint8_t header[DEFY_UPDATER_HEADER]) {
    size_t payload_length = sys_get_le16(header + DEFY_UPDATER_LENGTH_OFFSET);
    if (memcmp(header, "DU", 2) || header[DEFY_UPDATER_VERSION_OFFSET] != DEFY_UPDATER_VERSION ||
        header[DEFY_UPDATER_OPCODE_OFFSET] & DEFY_UPDATER_REPLY ||
        payload_length > DEFY_UPDATER_PAYLOAD) {
        return 0;
    }
    return DEFY_UPDATER_HEADER + payload_length + DEFY_UPDATER_CRC;
}

int defy_updater_close(struct defy_updater *state) {
    if (!state->active && !state->closing) {
        return 0;
    }
    if (!state->closing) {
        state->retry_delay = CLOSE_RETRY_FIRST_MS;
    }
    state->closing = true;
    state->active = false;
    state->writing = false;
    state->session = 0;
    state->probed = 0;
    state->completed = 0;
    state->pairing_reset = false;
    int err = state->ops->close();
    if (err) {
        state->retry_at = -1;
    } else {
        state->closing = false;
        defy_update_release(DEFY_UPDATE_HALVES);
    }
    return err;
}

int defy_updater_recover(struct defy_updater *state) {
    state->active = true;
    return defy_updater_close(state);
}

void defy_updater_expire(struct defy_updater *state, int64_t now, bool connected) {
    if (state->active && (!connected || now - state->last_request >= DEFY_UPDATER_IDLE_MS)) {
        defy_updater_close(state);
        state->cached_request_length = 0;
        state->cached_reply_length = 0;
    } else if (state->closing) {
        if (state->retry_at < 0) {
            state->retry_at = now + state->retry_delay;
            state->retry_delay = MIN(state->retry_delay * 2, CLOSE_RETRY_MAX_MS);
        } else if (now >= state->retry_at) {
            defy_updater_close(state);
        }
    }
    if (!connected) {
        state->cached_request_length = 0;
        state->cached_reply_length = 0;
    }
}

static size_t response(const uint8_t *request, uint8_t *reply, int status, size_t length) {
    memcpy(reply, request, DEFY_UPDATER_HEADER);
    reply[DEFY_UPDATER_OPCODE_OFFSET] |= DEFY_UPDATER_REPLY;
    sys_put_le16(length, reply + DEFY_UPDATER_LENGTH_OFFSET);
    sys_put_le16((uint16_t)(int16_t)status, reply + DEFY_UPDATER_STATUS_OFFSET);
    sys_put_le32(crc32_ieee(reply, DEFY_UPDATER_HEADER + length),
                 reply + DEFY_UPDATER_HEADER + length);
    return DEFY_UPDATER_HEADER + length + DEFY_UPDATER_CRC;
}

/*
 * Whether the first page's seal fits START: its program CRC32 and version,
 * and a transfer that ends with the sector holding the program's last byte.
 */
static bool seal_matches(struct defy_updater *state, const uint8_t *data) {
    struct defy_half_seal seal;

    if (!defy_half_seal_parse(data, &seal)) {
        return false;
    }
    uint32_t package_length = DEFY_HALF_PROGRAM_START - DEFY_HALF_FLASH_START + seal.program_size;
    return seal.program_size % 4 == 0 &&
           ROUND_UP(package_length, DEFY_HALF_SECTOR_SIZE) ==
               state->end_address - DEFY_HALF_FLASH_START &&
           seal.program_crc == state->program_crc && seal.program_version == state->version;
}

static int execute(struct defy_updater *state, uint8_t opcode, const uint8_t *data,
                   size_t length, uint8_t *output, size_t *output_length) {
    struct defy_updater_info info = {0};
    int err;
    switch (opcode) {
    case PROBE:
        if (length != 1 || data[0] > DEFY_WIRED_RIGHT || state->writing || state->completed) {
            return -EINVAL;
        }
        err = state->ops->probe(data[0], &info);
        if (err) {
            return err;
        }
        state->probed |= BIT(data[0]);
        output[0] = data[0];
        output[1] = info.valid;
        sys_put_le16(0, output + 2);
        sys_put_le32(info.hardware, output + 4);
        sys_put_le32(info.flash_start, output + 8);
        sys_put_le32(info.version, output + 12);
        sys_put_le32(info.crc, output + 16);
        *output_length = PROBE_REPLY;
        return 0;
    case START: {
        if (length != START_REQUEST || data[0] > DEFY_WIRED_RIGHT || data[1] || data[2] ||
            data[3] || state->probed != BOTH_SIDES || state->writing ||
            (state->completed & BIT(data[0]))) {
            return -EINVAL;
        }
        /* At least the seal's sector and one of the program's. */
        uint32_t transfer_length = sys_get_le32(data + 4);
        if (transfer_length < 2 * DEFY_HALF_SECTOR_SIZE ||
            transfer_length % DEFY_HALF_SECTOR_SIZE ||
            transfer_length > DEFY_HALF_FLASH_END - DEFY_HALF_FLASH_START) {
            return -EINVAL;
        }
        state->side = data[0];
        state->next_address = DEFY_HALF_FLASH_START;
        state->end_address = DEFY_HALF_FLASH_START + transfer_length;
        state->expected_crc = sys_get_le32(data + 8);
        state->version = sys_get_le32(data + 12);
        state->program_crc = sys_get_le32(data + 16);
        state->transfer_crc = 0;
        state->writing = true;
        return 0;
    }
    case PAGE: {
        if (length != PAGE_REQUEST || !state->writing ||
            sys_get_le32(data) != state->next_address ||
            state->next_address >= state->end_address) {
            return -EINVAL;
        }
        const uint8_t *page = data + 4;
        if ((state->next_address == DEFY_HALF_FLASH_START && !seal_matches(state, page)) ||
            (state->next_address == DEFY_HALF_PROGRAM_START &&
             !defy_half_metadata_matches(page, state->side, state->version))) {
            return -EBADMSG;
        }
        err = state->ops->page(state->side, state->next_address, page);
        if (err) {
            return err;
        }
        state->transfer_crc = crc32_ieee_update(state->transfer_crc, page, DEFY_HALF_PAGE_SIZE);
        state->next_address += DEFY_HALF_PAGE_SIZE;
        sys_put_le32(state->next_address, output);
        *output_length = 4;
        return 0;
    }
    case FINISH:
        if (length || !state->writing || state->next_address != state->end_address) {
            return -EINVAL;
        }
        if (state->transfer_crc != state->expected_crc) {
            return -EBADMSG;
        }
        err = state->ops->finish(state->side, &info);
        if (err) {
            return err;
        }
        if (!info.valid || info.version != state->version || info.crc != state->program_crc) {
            return -EBADMSG;
        }
        state->completed |= BIT(state->side);
        state->writing = false;
        sys_put_le32(info.version, output);
        sys_put_le32(info.crc, output + 4);
        *output_length = 8;
        return 0;
    case CLOSE:
        return length ? -EINVAL : defy_updater_close(state);
    case RESET_PAIRING:
        if (!state->ops->reset_pairing) {
            return -ENOTSUP;
        }
        /* After both PROBEs and before the first START, once per session. */
        if (length || state->probed != BOTH_SIDES || state->writing || state->completed ||
            state->pairing_reset) {
            return -EINVAL;
        }
        err = state->ops->reset_pairing();
        if (err) {
            return err;
        }
        state->pairing_reset = true;
        return 0;
    default:
        return -ENOTSUP;
    }
}

static size_t put_string(uint8_t *output, const char *text) {
    size_t length = strlen(text);
    output[0] = length;
    memcpy(output + 1, text, length);
    return 1 + length;
}

static size_t build_version(uint8_t *output) {
    output[0] = VERSION_FORMAT;
    memset(output + 1, 0, 3);
    size_t length = 4;
    length += put_string(output + length, DEFY_BUILD_MODULE_VERSION);
    length += put_string(output + length, DEFY_BUILD_SOURCE);
    length += put_string(output + length, DEFY_BUILD_COMMIT);
    return length;
}

static bool sessionless(uint8_t opcode) {
    return (opcode >= DIAGNOSTICS && opcode <= PAIRING) ||
           (opcode >= VERSION && opcode <= CLEAR_KEYMAP_CHANGES);
}

/* A sessionless request whose arguments have been checked. It leaves the session, its cached
 * reply and its idle limit alone. */
static int answer(struct defy_updater *state, uint8_t opcode, const uint8_t *data,
                  uint8_t *output, size_t *output_length) {
    size_t count;
    switch (opcode) {
    case VERSION:
        *output_length = build_version(output);
        return 0;
    case CLEAR_KEYMAP_CHANGES:
        /* Only between sessions. */
        if (state->active || state->closing) {
            return -EBUSY;
        }
        return state->ops->clear_keymap_changes ? state->ops->clear_keymap_changes() : -ENOTSUP;
    case KEY_TRACE:
        count = state->ops->key_trace
            ? state->ops->key_trace(sys_get_le32(data), data[4] & KEY_TRACE_CLEAR, output,
                                    DEFY_UPDATER_PAYLOAD)
            : 0;
        break;
    default: {
        size_t (*snapshot)(uint8_t *, size_t) =
            opcode == DIAGNOSTICS      ? state->ops->diagnostics
            : opcode == BATTERY_STATUS ? state->ops->battery_status
            : opcode == DOCK_STATUS    ? state->ops->dock_status
            : opcode == PAIRING        ? state->ops->pairing_status
            : opcode == HALF_VERSIONS  ? state->ops->half_versions
                                       : NULL;
        count = snapshot ? snapshot(output, DEFY_UPDATER_PAYLOAD) : 0;
        break;
    }
    }
    if (!count || count > DEFY_UPDATER_PAYLOAD) {
        return count ? -EOVERFLOW : -ENOTSUP;
    }
    *output_length = count;
    return 0;
}

size_t defy_updater_process(struct defy_updater *state, const uint8_t *request,
                           size_t length, uint8_t reply[DEFY_UPDATER_FRAME], int64_t now) {
    if (length < DEFY_UPDATER_HEADER || defy_updater_frame_length(request) != length ||
        sys_get_le16(request + DEFY_UPDATER_STATUS_OFFSET) != 0 ||
        sys_get_le32(request + length - DEFY_UPDATER_CRC) !=
            crc32_ieee(request, length - DEFY_UPDATER_CRC)) {
        return 0;
    }
    size_t payload_length = length - DEFY_UPDATER_HEADER - DEFY_UPDATER_CRC;
    uint8_t opcode = request[DEFY_UPDATER_OPCODE_OFFSET];
    uint32_t session = sys_get_le32(request + DEFY_UPDATER_SESSION_OFFSET);
    uint32_t sequence = sys_get_le32(request + DEFY_UPDATER_SEQUENCE_OFFSET);
    uint8_t *output = reply + DEFY_UPDATER_HEADER;
    const uint8_t *data = request + DEFY_UPDATER_HEADER;
    if (opcode == HELLO) {
        if (session || sequence || payload_length) {
            return response(request, reply, -EINVAL, 0);
        }
        memcpy(output, HELLO_MAGIC, sizeof(HELLO_MAGIC) - 1);
        sys_put_le16(DEFY_HALF_PAGE_SIZE, output + 8);
        sys_put_le16(DEFY_UPDATER_PAYLOAD, output + 10);
        sys_put_le32(DEFY_HALF_FLASH_START, output + 12);
        sys_put_le32(DEFY_HALF_FLASH_END, output + 16);
        sys_put_le32(DEFY_UPDATER_IDLE_MS, output + 20);
        return response(request, reply, 0, HELLO_REPLY);
    }
    if (sessionless(opcode)) {
        bool trace = opcode == KEY_TRACE;
        if (session || sequence || payload_length != (trace ? KEY_TRACE_REQUEST : 0) ||
            (trace && data[4] & ~KEY_TRACE_CLEAR)) {
            return response(request, reply, -EINVAL, 0);
        }
        size_t count = 0;
        int status = answer(state, opcode, data, output, &count);
        return response(request, reply, status, count);
    }
    if (state->cached_request_length &&
        session == sys_get_le32(state->cached_request + DEFY_UPDATER_SESSION_OFFSET) &&
        sequence == sys_get_le32(state->cached_request + DEFY_UPDATER_SEQUENCE_OFFSET)) {
        if (length != state->cached_request_length ||
            memcmp(request, state->cached_request, length)) {
            return response(request, reply, -EPROTO, 0);
        }
        memcpy(reply, state->cached_reply, state->cached_reply_length);
        return state->cached_reply_length;
    }
    int status;
    size_t output_length = 0;
    if (opcode == OPEN) {
        if (state->active || state->closing) {
            return response(request, reply, -EBUSY, 0);
        }
        if (!session || sequence != 1 || payload_length) {
            return response(request, reply, -EINVAL, 0);
        }
        if (!defy_update_claim(DEFY_UPDATE_HALVES)) {
            return response(request, reply, -EBUSY, 0);
        }
        state->active = true;
        status = state->ops->open();
        if (!status) {
            state->session = session;
            state->probed = 0;
            state->completed = 0;
            state->writing = false;
            state->pairing_reset = false;
        } else {
            defy_updater_close(state);
        }
    } else {
        if (!state->active || !session || session != state->session) {
            return response(request, reply, -ENOTCONN, 0);
        }
        if (state->sequence == UINT32_MAX || sequence != state->sequence + 1) {
            return response(request, reply, -EPROTO, 0);
        }
        status = execute(state, opcode, data, payload_length, output, &output_length);
        if (status) {
            if (state->active) {
                defy_updater_close(state);
            }
            output_length = 0;
        }
    }
    state->sequence = sequence;
    state->last_request = now;
    size_t result = response(request, reply, status, output_length);
    memcpy(state->cached_request, request, length);
    state->cached_request_length = length;
    memcpy(state->cached_reply, reply, result);
    state->cached_reply_length = result;
    return result;
}
