/* SPDX-License-Identifier: MIT */

#include <dygma/defy_feedback.h>
#include <dygma/defy_wired_protocol.h>
#include <errno.h>
#include <string.h>
#include <zephyr/sys/byteorder.h>

/*
 * 20 bytes: version, opcode, boot, sequence, remaining time, profile, flags, profile count,
 * clears, then the paired and connected masks, 16 bits each.
 */
int defy_host_feedback_encode(const struct defy_host_feedback *state,
                              uint8_t data[DEFY_HOST_FEEDBACK_SIZE]) {
    if (!data || !defy_host_feedback_valid(state)) {
        return -EINVAL;
    }
    data[0] = DEFY_HOST_FEEDBACK_VERSION;
    data[1] = DEFY_CONTROL_HOST_FEEDBACK;
    sys_put_le32(state->boot, data + 2);
    sys_put_le32(state->sequence, data + 6);
    sys_put_le16(state->remaining_ms, data + 10);
    data[12] = state->profile;
    data[13] = state->flags;
    data[14] = state->profile_count;
    data[15] = state->clears;
    sys_put_le16(state->paired_profiles, data + 16);
    sys_put_le16(state->connected_profiles, data + 18);
    return 0;
}

int defy_host_feedback_decode(const void *data, size_t length,
                              struct defy_host_feedback *state) {
    if (!data || !state || length != DEFY_HOST_FEEDBACK_SIZE) {
        return -EINVAL;
    }
    const uint8_t *bytes = data;
    struct defy_host_feedback decoded;

    /* Whole, padding too, so that equal states compare equal byte for byte. */
    memset(&decoded, 0, sizeof(decoded));
    decoded.boot = sys_get_le32(bytes + 2);
    decoded.sequence = sys_get_le32(bytes + 6);
    decoded.remaining_ms = sys_get_le16(bytes + 10);
    decoded.profile = bytes[12];
    decoded.flags = bytes[13];
    decoded.profile_count = bytes[14];
    decoded.clears = bytes[15];
    decoded.paired_profiles = sys_get_le16(bytes + 16);
    decoded.connected_profiles = sys_get_le16(bytes + 18);
    if (bytes[0] != DEFY_HOST_FEEDBACK_VERSION || bytes[1] != DEFY_CONTROL_HOST_FEEDBACK ||
        !defy_host_feedback_valid(&decoded)) {
        return -EPROTO;
    }
    *state = decoded;
    return 0;
}

/*
 * The 'DL' header holds the version and opcode, and the payload after them fills the
 * frame from DEFY_WIRED_CONTROL_PAYLOAD_OFFSET to the CRC.
 */
#define WIRED_PAYLOAD_SIZE (DEFY_HOST_FEEDBACK_SIZE - DEFY_CONTROL_HEADER_SIZE)

BUILD_ASSERT(WIRED_PAYLOAD_SIZE == DEFY_WIRED_CONTROL_PAYLOAD_SIZE,
             "the host display fills the cable frame");

int defy_wired_host_feedback_encode(const struct defy_wired_host_feedback *command,
                                    uint8_t frame[DEFY_WIRED_FRAME_SIZE]) {
    uint8_t data[DEFY_HOST_FEEDBACK_SIZE];
    if (!frame || !command || command->side > DEFY_WIRED_RIGHT || !command->session) {
        return -EINVAL;
    }
    int err = defy_host_feedback_encode(&command->state, data);
    if (err) {
        return err;
    }
    const struct defy_wired_control control = {
        .version = DEFY_HOST_FEEDBACK_VERSION,
        .opcode = DEFY_CONTROL_HOST_FEEDBACK,
        .side = command->side,
        .session = command->session,
    };
    defy_wired_control_put(&control, frame);
    memcpy(frame + DEFY_WIRED_CONTROL_PAYLOAD_OFFSET, data + DEFY_CONTROL_HEADER_SIZE,
           WIRED_PAYLOAD_SIZE);
    defy_wired_frame_seal(frame);
    return 0;
}

int defy_wired_host_feedback_decode(const uint8_t *frame, const struct defy_wired_control *control,
                                    struct defy_wired_host_feedback *command) {
    if (!frame || !control || !command) {
        return -EINVAL;
    }
    if (control->version != DEFY_HOST_FEEDBACK_VERSION ||
        control->opcode != DEFY_CONTROL_HOST_FEEDBACK || control->flag) {
        return -EPROTO;
    }
    uint8_t data[DEFY_HOST_FEEDBACK_SIZE] = {control->version, control->opcode};
    memcpy(data + DEFY_CONTROL_HEADER_SIZE, frame + DEFY_WIRED_CONTROL_PAYLOAD_OFFSET,
           WIRED_PAYLOAD_SIZE);
    struct defy_wired_host_feedback decoded = {
        .side = control->side,
        .session = control->session,
    };
    int err = defy_host_feedback_decode(data, sizeof(data), &decoded.state);
    if (!err) {
        *command = decoded;
    }
    return err;
}
