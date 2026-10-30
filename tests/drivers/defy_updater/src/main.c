/* SPDX-License-Identifier: MIT */

#include <errno.h>
#include <string.h>
#include <zephyr/ztest.h>
#include <zephyr/sys/byteorder.h>
#include <zephyr/sys/crc.h>
#include <dygma/defy_half_package.h>
#include <dygma/defy_updater.h>
#include <dygma/defy_update_guard.h>

static struct defy_updater state;
static uint8_t request[DEFY_UPDATER_FRAME], reply[DEFY_UPDATER_FRAME], image[8192];
static uint32_t sequence;
static uint32_t token;
static unsigned int opens, probes, pages, finishes, closes;
static int failure;
static int close_failure;
static bool seal_valid;
static unsigned int diagnostics_calls;
static size_t diagnostics_length;
static uint8_t diagnostics_data[104];
static unsigned int battery_calls;
static unsigned int dock_calls;
static unsigned int trace_calls;
static uint32_t trace_from;
static bool trace_clear;
static unsigned int pairing_calls;
static unsigned int resets;
static int reset_failure;
static const uint8_t pairing_data[4] = {1, 2, 3, 1};
static unsigned int half_versions_calls;
static unsigned int keymap_clears;
static int keymap_clear_failure;

static int fake_open(void) { opens++; return failure; }
static int fake_probe(uint8_t side, struct defy_updater_info *info) {
    zassert_true(side < 2);
    probes++;
    *info = (struct defy_updater_info){.hardware = 1, .flash_start = 0x5000,
                                     .version = 7, .crc = 0x1234, .valid = seal_valid};
    return failure;
}
static int fake_page(uint8_t side, uint32_t address, const uint8_t *data) {
    zassert_true(side < 2);
    zassert_true(address >= 0x5000 && address < 0x7000);
    zassert_mem_equal(data, image + address - 0x5000, 256);
    pages++;
    return failure;
}
static int fake_finish(uint8_t side, struct defy_updater_info *info) {
    finishes++;
    *info = (struct defy_updater_info){.version = 7, .crc = 0x1234, .valid = seal_valid};
    return failure;
}
static int fake_close(void) { closes++; return close_failure; }
static size_t fake_diagnostics(uint8_t *output, size_t capacity) {
    diagnostics_calls++;
    zassert_equal(capacity, DEFY_UPDATER_PAYLOAD);
    if (diagnostics_length <= sizeof(diagnostics_data)) {
        memcpy(output, diagnostics_data, diagnostics_length);
    }
    return diagnostics_length;
}
static size_t fake_battery(uint8_t *output, size_t capacity) {
    battery_calls++;
    zassert_equal(capacity, DEFY_UPDATER_PAYLOAD);
    memcpy(output, diagnostics_data, sizeof(diagnostics_data));
    return sizeof(diagnostics_data);
}
static size_t fake_dock(uint8_t *output, size_t capacity) {
    dock_calls++;
    zassert_equal(capacity, DEFY_UPDATER_PAYLOAD);
    memcpy(output, diagnostics_data, 8);
    return 8;
}
static size_t fake_trace(uint32_t from, bool clear, uint8_t *output, size_t capacity) {
    trace_calls++;
    zassert_equal(capacity, DEFY_UPDATER_PAYLOAD);
    trace_from = from;
    trace_clear = clear;
    memcpy(output, diagnostics_data, 40);
    return 40;
}
static size_t fake_pairing(uint8_t *output, size_t capacity) {
    pairing_calls++;
    zassert_equal(capacity, DEFY_UPDATER_PAYLOAD);
    memcpy(output, pairing_data, sizeof(pairing_data));
    return sizeof(pairing_data);
}
static int fake_reset_pairing(void) {
    zassert_true(state.active);
    zassert_equal(state.probed, 3);
    resets++;
    return reset_failure;
}
static size_t fake_half_versions(uint8_t *output, size_t capacity) {
    half_versions_calls++;
    zassert_equal(capacity, DEFY_UPDATER_PAYLOAD);
    memcpy(output, diagnostics_data, 80);
    return 80;
}
static int fake_clear_keymap_changes(void) {
    zassert_false(state.active);
    zassert_false(state.closing);
    keymap_clears++;
    return keymap_clear_failure;
}
static const struct defy_updater_ops ops = {
    .open = fake_open, .probe = fake_probe, .page = fake_page,
    .finish = fake_finish, .close = fake_close, .diagnostics = fake_diagnostics,
    .battery_status = fake_battery, .dock_status = fake_dock, .key_trace = fake_trace,
    .pairing_status = fake_pairing, .reset_pairing = fake_reset_pairing,
    .half_versions = fake_half_versions, .clear_keymap_changes = fake_clear_keymap_changes,
};

static size_t frame(uint8_t op, const void *data, size_t length) {
    memcpy(request, "DU", 2);
    request[2] = 1;
    request[3] = op;
    sys_put_le32(token, request + 4);
    sys_put_le32(sequence++, request + 8);
    sys_put_le16(length, request + 12);
    sys_put_le16(0, request + 14);
    if (length) {
        memcpy(request + 16, data, length);
    }
    sys_put_le32(crc32_ieee(request, 16 + length), request + 16 + length);
    return 20 + length;
}
static int process(size_t length) {
    size_t response = defy_updater_process(&state, request, length, reply, 100);
    zassert_true(response >= 20);
    zassert_equal(reply[3], request[3] | 0x80);
    zassert_mem_equal(reply + 4, request + 4, 8);
    zassert_equal(sys_get_le32(reply + response - 4), crc32_ieee(reply, response - 4));
    return (int16_t)sys_get_le16(reply + 14);
}
static int send(uint8_t op, const void *data, size_t length) {
    return process(frame(op, data, length));
}
static void prepare(void) {
    zassert_ok(send(2, NULL, 0));
    for (uint8_t side = 0; side < 2; side++) {
        zassert_ok(send(3, &side, 1));
    }
}
static void start(void) {
    uint8_t data[20] = {0};
    sys_put_le32(sizeof(image), data + 4);
    sys_put_le32(crc32_ieee(image, sizeof(image)), data + 8);
    sys_put_le32(7, data + 12);
    sys_put_le32(0x1234, data + 16);
    zassert_ok(send(4, data, sizeof(data)));
}
static int page(uint32_t address) {
    uint8_t data[260];
    sys_put_le32(address, data);
    memcpy(data + 4, image + address - 0x5000, 256);
    return send(5, data, sizeof(data));
}
static void all_pages(void) {
    for (uint32_t address = 0x5000; address < 0x7000; address += 256) {
        zassert_ok(page(address));
        zassert_equal(sys_get_le32(reply + 16), address + 256);
    }
}
static size_t trace_frame(uint32_t from, uint8_t flags) {
    uint8_t data[5];
    sys_put_le32(from, data);
    data[4] = flags;
    return frame(11, data, sizeof(data));
}
static void before(void *fixture) {
    ARG_UNUSED(fixture);
    defy_update_release(DEFY_UPDATE_HALVES);
    defy_update_release(DEFY_UPDATE_NEURON);
    defy_updater_init(&state, &ops);
    opens = probes = pages = finishes = closes = 0;
    failure = 0;
    close_failure = 0;
    seal_valid = true;
    diagnostics_calls = 0;
    diagnostics_length = sizeof(diagnostics_data);
    battery_calls = 0;
    dock_calls = 0;
    trace_calls = 0;
    trace_from = 0;
    trace_clear = false;
    pairing_calls = resets = 0;
    reset_failure = 0;
    half_versions_calls = 0;
    keymap_clears = 0;
    keymap_clear_failure = 0;
    for (size_t i = 0; i < sizeof(diagnostics_data); i++) {
        diagnostics_data[i] = i;
    }
    sequence = 1;
    token = 0x12345678;
    memset(image, 0xff, sizeof(image));
    sys_put_le32(1, image);
    sys_put_le32(28, image + 4);
    sys_put_le32(0, image + 8);
    sys_put_le32(0x6000, image + 12);
    sys_put_le32(0x108, image + 16);
    sys_put_le32(0x1234, image + 20);
    sys_put_le32(7, image + 24);
    sys_put_le32(crc32_ieee(image, 28), image + 8);
    memcpy(image + 0x1000, "DFYRP204", 8);
    sys_put_le32(1, image + 0x1008);
    sys_put_le32(32, image + 0x100c);
    sys_put_le32(1, image + 0x1010);
    sys_put_le32(1, image + 0x1014);
    sys_put_le32(7, image + 0x1018);
    sys_put_le32(0, image + 0x101c);
}

ZTEST(defy_updater, test_hello_does_not_enter_maintenance) {
    token = sequence = 0;
    zassert_ok(process(frame(1, NULL, 0)));
    zassert_mem_equal(reply + 16, "DEFYUPD1", 8);
    zassert_equal(opens, 0);
    zassert_false(state.active);
}
ZTEST(defy_updater, test_snapshots_without_session_reach_only_their_callback) {
    static const struct {
        uint8_t opcode;
        unsigned int *calls;
        const uint8_t *data;
        size_t length;
    } snapshots[] = {
        {8, &diagnostics_calls, diagnostics_data, sizeof(diagnostics_data)},
        {9, &battery_calls, diagnostics_data, sizeof(diagnostics_data)},
        {10, &dock_calls, diagnostics_data, 8},
        {12, &pairing_calls, pairing_data, sizeof(pairing_data)},
        {15, &half_versions_calls, diagnostics_data, 80},
    };
    struct defy_updater previous = state;
    for (size_t i = 0; i < ARRAY_SIZE(snapshots); i++) {
        token = sequence = 0;
        zassert_ok(send(snapshots[i].opcode, NULL, 0));
        zassert_equal(sys_get_le16(reply + 12), snapshots[i].length);
        zassert_mem_equal(reply + 16, snapshots[i].data, snapshots[i].length);
        zassert_equal(*snapshots[i].calls, 1);
        zassert_equal(diagnostics_calls + battery_calls + dock_calls + pairing_calls +
                          half_versions_calls,
                      i + 1);
    }
    zassert_mem_equal(&state, &previous, sizeof(state));
    zassert_equal(opens + probes + pages + finishes + closes + resets + trace_calls + keymap_clears,
                  0);
}
ZTEST(defy_updater, test_missing_empty_or_oversized_snapshot_has_empty_error_reply) {
    static const uint8_t opcodes[] = {8, 9, 10, 11, 12, 15};
    struct defy_updater_ops missing[ARRAY_SIZE(opcodes)];
    for (size_t i = 0; i < ARRAY_SIZE(opcodes); i++) {
        missing[i] = ops;
    }
    missing[0].diagnostics = NULL;
    missing[1].battery_status = NULL;
    missing[2].dock_status = NULL;
    missing[3].key_trace = NULL;
    missing[4].pairing_status = NULL;
    missing[5].half_versions = NULL;
    for (size_t i = 0; i < ARRAY_SIZE(opcodes); i++) {
        state.ops = &missing[i];
        token = sequence = 0;
        size_t length = opcodes[i] == 11 ? trace_frame(0, 0) : frame(opcodes[i], NULL, 0);
        zassert_equal(process(length), -ENOTSUP);
        zassert_equal(sys_get_le16(reply + 12), 0);
    }
    zassert_equal(diagnostics_calls + battery_calls + dock_calls + trace_calls + pairing_calls +
                      half_versions_calls,
                  0);
    state.ops = &ops;
    diagnostics_length = 0;
    sequence = 0;
    zassert_equal(send(8, NULL, 0), -ENOTSUP);
    zassert_equal(sys_get_le16(reply + 12), 0);
    diagnostics_length = DEFY_UPDATER_PAYLOAD + 1;
    sequence = 0;
    zassert_equal(send(8, NULL, 0), -EOVERFLOW);
    zassert_equal(sys_get_le16(reply + 12), 0);
    zassert_equal(diagnostics_calls, 2);
    zassert_equal(opens + closes, 0);
}
ZTEST(defy_updater, test_key_trace_passes_start_and_clear_and_keeps_active_write) {
    prepare();
    start();
    zassert_ok(page(0x5000));
    struct defy_updater previous = state;
    uint8_t cached_request[DEFY_UPDATER_FRAME];
    memcpy(cached_request, request, sizeof(cached_request));
    token = sequence = 0;
    size_t length = trace_frame(0x01020304, 0);
    zassert_equal(defy_updater_process(&state, request, length, reply, 9000), 60);
    zassert_equal((int16_t)sys_get_le16(reply + 14), 0);
    zassert_equal(sys_get_le16(reply + 12), 40);
    zassert_mem_equal(reply + 16, diagnostics_data, 40);
    zassert_equal(trace_from, 0x01020304);
    zassert_false(trace_clear);
    sequence = 0;
    zassert_ok(process(trace_frame(7, 1)));
    zassert_equal(trace_from, 7);
    zassert_true(trace_clear);
    zassert_equal(trace_calls, 2);
    zassert_mem_equal(&state, &previous, sizeof(state));
    memcpy(request, cached_request, sizeof(request));
    zassert_ok(process(sizeof(request)));
    zassert_equal(sys_get_le32(reply + 16), 0x5100);
    zassert_equal(pages, 1);
    zassert_equal(diagnostics_calls + battery_calls + dock_calls + closes, 0);
}
ZTEST(defy_updater, test_invalid_key_trace_never_reaches_callback) {
    prepare();
    struct defy_updater previous = state;
    zassert_equal(process(trace_frame(0, 0)), -EINVAL);
    token = 0;
    zassert_equal(process(trace_frame(0, 0)), -EINVAL);
    uint8_t data[6] = {0};
    sequence = 0;
    zassert_equal(send(11, NULL, 0), -EINVAL);
    sequence = 0;
    zassert_equal(send(11, data, 4), -EINVAL);
    sequence = 0;
    zassert_equal(send(11, data, 6), -EINVAL);
    for (uint8_t flags = 2; flags; flags <<= 1) {
        sequence = 0;
        zassert_equal(process(trace_frame(0, flags | 1)), -EINVAL);
    }
    zassert_equal(trace_calls, 0);
    zassert_mem_equal(&state, &previous, sizeof(state));
    zassert_equal(closes, 0);
}
ZTEST(defy_updater, test_reset_pairing_runs_after_both_probes_and_before_start) {
    prepare();
    zassert_ok(send(13, NULL, 0));
    zassert_equal(sys_get_le16(reply + 12), 0);
    zassert_equal(resets, 1);
    zassert_true(state.active);
    start();
    all_pages();
    zassert_ok(send(6, NULL, 0));
    zassert_ok(send(7, NULL, 0));
    zassert_equal(resets, 1);
    zassert_equal(closes, 1);
    token++;
    sequence = 1;
    prepare();
    zassert_ok(send(13, NULL, 0));
    zassert_equal(resets, 2);
}
ZTEST(defy_updater, test_repeated_reset_pairing_gets_cached_reply_and_runs_once) {
    prepare();
    size_t length = frame(13, NULL, 0);
    zassert_ok(process(length));
    zassert_ok(process(length));
    zassert_equal(resets, 1);
    zassert_true(state.active);
    zassert_equal(send(13, NULL, 0), -EINVAL);
    zassert_equal(resets, 1);
    zassert_false(state.active);
    zassert_equal(closes, 1);
}
ZTEST(defy_updater, test_reset_pairing_out_of_order_closes_session) {
    zassert_equal(send(13, NULL, 0), -ENOTCONN);
    sequence = 1;
    zassert_ok(send(2, NULL, 0));
    zassert_equal(send(13, NULL, 0), -EINVAL);
    zassert_false(state.active);
    token++;
    sequence = 1;
    zassert_ok(send(2, NULL, 0));
    uint8_t side = 1;
    zassert_ok(send(3, &side, 1));
    zassert_equal(send(13, NULL, 0), -EINVAL);
    token++;
    sequence = 1;
    prepare();
    start();
    zassert_equal(send(13, NULL, 0), -EINVAL);
    token++;
    sequence = 1;
    prepare();
    start();
    all_pages();
    zassert_ok(send(6, NULL, 0));
    zassert_equal(send(13, NULL, 0), -EINVAL);
    token++;
    sequence = 1;
    prepare();
    uint8_t extra = 0;
    zassert_equal(send(13, &extra, 1), -EINVAL);
    zassert_equal(resets, 0);
    zassert_equal(closes, 5);
    zassert_equal(pages, 32);
}
ZTEST(defy_updater, test_failed_reset_pairing_closes_session_without_repeating) {
    prepare();
    reset_failure = -EIO;
    size_t length = frame(13, NULL, 0);
    zassert_equal(process(length), -EIO);
    zassert_false(state.active);
    zassert_equal(closes, 1);
    zassert_equal(process(length), -EIO);
    zassert_equal(resets, 1);
    zassert_equal(closes, 1);
    reset_failure = 0;
    token++;
    sequence = 1;
    prepare();
    zassert_ok(send(13, NULL, 0));
    zassert_equal(resets, 2);
}
ZTEST(defy_updater, test_reset_pairing_without_support_closes_session) {
    struct defy_updater_ops missing = ops;
    missing.reset_pairing = NULL;
    state.ops = &missing;
    prepare();
    zassert_equal(send(13, NULL, 0), -ENOTSUP);
    zassert_false(state.active);
    zassert_equal(closes, 1);
}
ZTEST(defy_updater, test_snapshots_preserve_write_state_cached_reply_and_expiry) {
    static const uint8_t opcodes[] = {8, 9, 10, 12};
    prepare();
    start();
    zassert_ok(page(0x5000));
    struct defy_updater previous = state;
    uint8_t cached_request[DEFY_UPDATER_FRAME];
    memcpy(cached_request, request, sizeof(cached_request));
    for (size_t i = 0; i < ARRAY_SIZE(opcodes); i++) {
        token = sequence = 0;
        size_t length = frame(opcodes[i], NULL, 0);
        zassert_true(defy_updater_process(&state, request, length, reply, 9000) > 20);
        zassert_equal((int16_t)sys_get_le16(reply + 14), 0);
        zassert_mem_equal(&state, &previous, sizeof(state));
    }
    token = sequence = 0;
    diagnostics_data[0]++;
    zassert_ok(send(8, NULL, 0));
    zassert_equal(reply[16], diagnostics_data[0]);
    zassert_equal(diagnostics_calls, 2);
    memcpy(request, cached_request, sizeof(request));
    zassert_ok(process(sizeof(request)));
    zassert_equal(pages, 1);
    defy_updater_expire(&state, 100 + DEFY_UPDATER_IDLE_MS, true);
    zassert_false(state.active);
    zassert_equal(closes, 1);
}
ZTEST(defy_updater, test_bad_crc_length_version_do_not_reach_hardware) {
    size_t length = frame(2, NULL, 0);
    request[length - 1] ^= 1;
    zassert_equal(defy_updater_process(&state, request, length, reply, 0), 0);
    length = frame(2, NULL, 0);
    zassert_equal(defy_updater_process(&state, request, length - 1, reply, 0), 0);
    request[2] = 2;
    zassert_equal(defy_updater_process(&state, request, length, reply, 0), 0);
    zassert_equal(opens, 0);
}
ZTEST(defy_updater, test_duplicate_open_does_not_reset_twice) {
    size_t length = frame(2, NULL, 0);
    zassert_ok(process(length));
    zassert_ok(process(length));
    zassert_equal(opens, 1);
}
ZTEST(defy_updater, test_changed_duplicate_and_wrong_nonce_are_rejected) {
    prepare();
    sequence--;
    uint8_t other = 0;
    zassert_equal(send(3, &other, 1), -EPROTO);
    token++;
    zassert_equal(send(7, NULL, 0), -ENOTCONN);
    zassert_true(state.active);
    zassert_equal(probes, 2);
}
ZTEST(defy_updater, test_new_open_while_active_is_busy) {
    prepare();
    token++;
    sequence = 1;
    zassert_equal(send(2, NULL, 0), -EBUSY);
    zassert_equal(opens, 1);
}
ZTEST(defy_updater, test_requires_both_probes_before_start) {
    zassert_ok(send(2, NULL, 0));
    uint8_t side = 0;
    zassert_ok(send(3, &side, 1));
    uint8_t data[20] = {0};
    sys_put_le32(8192, data + 4);
    zassert_equal(send(4, data, sizeof(data)), -EINVAL);
    zassert_equal(pages, 0);
    zassert_equal(closes, 1);
}
ZTEST(defy_updater, test_invalid_old_seal_still_allows_recovery) {
    seal_valid = false;
    prepare();
    zassert_equal(reply[17], 0);
    start();
    zassert_true(state.writing);
}
ZTEST(defy_updater, test_padded_image_range_is_bounded_before_write) {
    prepare();
    uint8_t data[20] = {0};
    sys_put_le32(0xfffff000, data + 4);
    zassert_equal(send(4, data, sizeof(data)), -EINVAL);
    zassert_equal(pages, 0);
}
ZTEST(defy_updater, test_complete_image_and_close) {
    prepare();
    start();
    all_pages();
    zassert_ok(send(6, NULL, 0));
    zassert_equal(finishes, 1);
    zassert_equal(pages, 32);
    zassert_equal(state.completed, 1);
    zassert_ok(send(7, NULL, 0));
    zassert_false(state.active);
    zassert_equal(closes, 1);
    zassert_ok(process(20));
    zassert_equal(closes, 1);
}
ZTEST(defy_updater, test_duplicate_page_never_erases_or_writes_again) {
    prepare();
    start();
    zassert_ok(page(0x5000));
    zassert_ok(process(280));
    zassert_equal(pages, 1);
    zassert_equal(state.next_address, 0x5100);
}
ZTEST(defy_updater, test_nonsequential_page_is_rejected_and_session_closed) {
    prepare();
    start();
    zassert_equal(page(0x5100), -EINVAL);
    zassert_equal(pages, 0);
    zassert_equal(closes, 1);
}
ZTEST(defy_updater, test_bad_seal_is_rejected_before_erase) {
    image[8] ^= 1;
    prepare();
    start();
    zassert_equal(page(0x5000), -EBADMSG);
    zassert_equal(pages, 0);
}
ZTEST(defy_updater, test_wrong_side_metadata_is_rejected) {
    sys_put_le32(0, image + 0x1014);
    prepare();
    start();
    for (uint32_t address = 0x5000; address < 0x6000; address += 256) {
        zassert_ok(page(address));
    }
    zassert_equal(page(0x6000), -EBADMSG);
    zassert_equal(pages, 16);
}
ZTEST(defy_updater, test_incomplete_image_cannot_validate) {
    prepare();
    start();
    zassert_ok(page(0x5000));
    zassert_equal(send(6, NULL, 0), -EINVAL);
    zassert_equal(finishes, 0);
}
ZTEST(defy_updater, test_transfer_crc_mismatch_cannot_validate) {
    prepare();
    start();
    image[0x1100] ^= 1;
    all_pages();
    zassert_equal(send(6, NULL, 0), -EBADMSG);
    zassert_equal(finishes, 0);
}
ZTEST(defy_updater, test_bootloader_validation_failure_closes) {
    prepare();
    start();
    all_pages();
    seal_valid = false;
    zassert_equal(send(6, NULL, 0), -EBADMSG);
    zassert_equal(finishes, 1);
    zassert_false(state.active);
}
ZTEST(defy_updater, test_failed_page_reply_is_cached_without_retrying_flash) {
    prepare();
    start();
    failure = -EIO;
    zassert_equal(page(0x5000), -EIO);
    zassert_equal(process(280), -EIO);
    zassert_equal(pages, 1);
    zassert_equal(closes, 1);
}
ZTEST(defy_updater, test_timeout_and_disconnect_drop_old_session) {
    prepare();
    defy_updater_expire(&state, 100 + DEFY_UPDATER_IDLE_MS - 1, true);
    zassert_true(state.active);
    defy_updater_expire(&state, 100 + DEFY_UPDATER_IDLE_MS, true);
    zassert_false(state.active);
    zassert_equal(closes, 1);
    zassert_equal(send(7, NULL, 0), -ENOTCONN);
    token++;
    sequence = 1;
    zassert_ok(send(2, NULL, 0));
    defy_updater_expire(&state, 101, false);
    zassert_equal(closes, 2);
    zassert_false(state.active);
}
ZTEST(defy_updater, test_open_failure_does_not_create_session) {
    failure = -EIO;
    zassert_equal(send(2, NULL, 0), -EIO);
    zassert_false(state.active);
    zassert_equal(closes, 1);
}
ZTEST(defy_updater, test_unrecognized_command_cannot_leave_session_open) {
    prepare();
    zassert_equal(send(99, NULL, 0), -ENOTSUP);
    zassert_equal(closes, 1);
}
ZTEST(defy_updater, test_failed_close_retries_with_backoff_without_reopening_write_session) {
    prepare();
    start();
    close_failure = -EIO;
    zassert_equal(send(7, NULL, 0), -EIO);
    zassert_equal(closes, 1);
    zassert_false(state.active);
    zassert_false(state.writing);
    zassert_equal(state.session, 0);
    zassert_true(state.closing);
    zassert_equal(process(20), -EIO);
    zassert_equal(closes, 1);
    zassert_false(defy_update_claim(DEFY_UPDATE_NEURON));
    zassert_equal(page(0x5000), -ENOTCONN);
    zassert_equal(pages, 0);
    token++;
    sequence = 1;
    zassert_equal(send(2, NULL, 0), -EBUSY);
    zassert_equal(opens, 1);
    int64_t now = 100;
    const int delays[] = {100, 200, 400, 800, 1000, 1000};
    for (size_t i = 0; i < ARRAY_SIZE(delays); i++) {
        defy_updater_expire(&state, now, true);
        defy_updater_expire(&state, now + delays[i] - 1, true);
        zassert_equal(closes, i + 1);
        now += delays[i];
        defy_updater_expire(&state, now, true);
        zassert_equal(closes, i + 2);
    }
    close_failure = 0;
    defy_updater_expire(&state, now, true);
    defy_updater_expire(&state, now + 1000, true);
    zassert_false(state.closing);
    zassert_true(defy_update_claim(DEFY_UPDATE_NEURON));
    defy_update_release(DEFY_UPDATE_NEURON);
    sequence = 1;
    zassert_ok(send(2, NULL, 0));
    zassert_equal(opens, 2);
}
ZTEST(defy_updater, test_failed_close_reply_stays_cached_after_recovery) {
    prepare();
    close_failure = -EIO;
    zassert_equal(send(7, NULL, 0), -EIO);
    uint8_t cached_reply[DEFY_UPDATER_FRAME];
    memcpy(cached_reply, reply, 20);
    close_failure = 0;
    defy_updater_expire(&state, 100, true);
    defy_updater_expire(&state, 200, true);
    zassert_equal(closes, 2);
    zassert_false(state.closing);
    zassert_equal(process(20), -EIO);
    zassert_mem_equal(cached_reply, reply, 20);
    zassert_equal(closes, 2);
}
ZTEST(defy_updater, test_partial_open_failure_keeps_recovery_and_cached_failure) {
    failure = -EIO;
    close_failure = -EBUSY;
    zassert_equal(send(2, NULL, 0), -EIO);
    zassert_false(state.active);
    zassert_true(state.closing);
    zassert_equal(closes, 1);
    zassert_false(defy_update_claim(DEFY_UPDATE_NEURON));
    zassert_equal(process(20), -EIO);
    zassert_equal(opens, 1);
    zassert_equal(closes, 1);
    close_failure = 0;
    defy_updater_expire(&state, 100, true);
    defy_updater_expire(&state, 200, true);
    zassert_false(state.closing);
    zassert_equal(process(20), -EIO);
    zassert_equal(opens, 1);
    zassert_equal(closes, 2);
}
ZTEST(defy_updater, test_timeout_recovery_continues_after_disconnect) {
    prepare();
    close_failure = -EIO;
    int64_t now = 100 + DEFY_UPDATER_IDLE_MS;
    defy_updater_expire(&state, now, true);
    zassert_true(state.closing);
    zassert_equal(closes, 1);
    zassert_equal(process(21), -ENOTCONN);
    defy_updater_expire(&state, now, false);
    defy_updater_expire(&state, now + 99, false);
    zassert_equal(closes, 1);
    defy_updater_expire(&state, now + 100, false);
    zassert_equal(closes, 2);
    close_failure = 0;
    defy_updater_expire(&state, now + 100, false);
    defy_updater_expire(&state, now + 300, false);
    zassert_equal(closes, 3);
    zassert_false(state.closing);
    zassert_true(defy_update_claim(DEFY_UPDATE_NEURON));
}
ZTEST(defy_updater, test_bootloader_owner_rejects_open_without_cleanup_or_release) {
    zassert_true(defy_update_claim(DEFY_UPDATE_NEURON));
    zassert_equal(send(2, NULL, 0), -EBUSY);
    zassert_equal(opens + closes, 0);
    zassert_false(state.active);
    zassert_false(state.closing);
    defy_updater_expire(&state, DEFY_UPDATER_IDLE_MS, false);
    defy_update_release(DEFY_UPDATE_HALVES);
    zassert_false(defy_update_claim(DEFY_UPDATE_HALVES));
    defy_update_release(DEFY_UPDATE_NEURON);
    sequence = 1;
    zassert_ok(send(2, NULL, 0));
}
ZTEST(defy_updater, test_frame_length_needs_magic_version_request_and_payload_limit) {
    frame(5, NULL, 0);
    zassert_equal(defy_updater_frame_length(request), 20);
    sys_put_le16(260, request + 12);
    zassert_equal(defy_updater_frame_length(request), 280);
    sys_put_le16(261, request + 12);
    zassert_equal(defy_updater_frame_length(request), 0);
    frame(5, NULL, 0);
    request[3] |= 0x80;
    zassert_equal(defy_updater_frame_length(request), 0);
    frame(5, NULL, 0);
    request[2] = 2;
    zassert_equal(defy_updater_frame_length(request), 0);
    frame(5, NULL, 0);
    request[1] = 'V';
    zassert_equal(defy_updater_frame_length(request), 0);
    /* The status and the CRC are the request's to check, not the header's. */
    frame(5, NULL, 0);
    sys_put_le16(1, request + 14);
    zassert_equal(defy_updater_frame_length(request), 20);
}
ZTEST(defy_updater, test_metadata_side_ids_are_the_reverse_of_cable_sides) {
    const uint8_t *metadata = image + 0x1000;
    zassert_true(defy_half_metadata_matches(metadata, DEFY_WIRED_LEFT, 7));
    zassert_false(defy_half_metadata_matches(metadata, DEFY_WIRED_RIGHT, 7));
    zassert_false(defy_half_metadata_matches(metadata, DEFY_WIRED_LEFT, 8));
    sys_put_le32(0, image + 0x1014);
    zassert_true(defy_half_metadata_matches(metadata, DEFY_WIRED_RIGHT, 7));
    zassert_false(defy_half_metadata_matches(metadata, DEFY_WIRED_LEFT, 7));
    sys_put_le32(2, image + 0x1010);
    zassert_false(defy_half_metadata_matches(metadata, DEFY_WIRED_RIGHT, 7));
}
ZTEST(defy_updater, test_recover_releases_held_halves_with_close_retries) {
    zassert_true(defy_update_claim(DEFY_UPDATE_HALVES));
    close_failure = -EIO;
    zassert_equal(defy_updater_recover(&state), -EIO);
    zassert_equal(closes, 1);
    zassert_false(state.active);
    zassert_true(state.closing);
    zassert_true(defy_update_held(DEFY_UPDATE_HALVES));
    sequence = 1;
    zassert_equal(send(2, NULL, 0), -EBUSY);
    close_failure = 0;
    defy_updater_expire(&state, 1000, false);
    zassert_equal(state.retry_at, 1100);
    defy_updater_expire(&state, 1100, false);
    zassert_equal(closes, 2);
    zassert_false(state.closing);
    zassert_false(defy_update_held(DEFY_UPDATE_HALVES));
    zassert_equal(opens, 0);
}
/* The values this suite's CMakeLists.txt passes, the longest the VERSION command carries. */
static const char module_version[] = "v0.1.0-12-g2826cb2abcdef01-dirty";
static const char commit[] =
    "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef01234567";
ZTEST(defy_updater, test_version_reports_what_the_image_was_built_from) {
    zassert_equal(sizeof(module_version) - 1, 32);
    zassert_equal(sizeof(commit) - 1, 72);
    struct defy_updater previous = state;
    token = sequence = 0;
    zassert_ok(send(14, NULL, 0));
    zassert_equal(sys_get_le16(reply + 12), 251);
    const uint8_t *payload = reply + 16;
    const uint8_t header[4] = {1, 0, 0, 0};
    zassert_mem_equal(payload, header, sizeof(header));
    zassert_equal(payload[4], 32);
    zassert_mem_equal(payload + 5, module_version, 32);
    const uint8_t *source = payload + 37;
    zassert_equal(source[0], 140);
    for (size_t i = 0; i < 140; i++) {
        zassert_equal(source[1 + i], i < 39 ? 'o' : i == 39 ? '/' : 'r');
    }
    zassert_equal(source[141], 72);
    zassert_mem_equal(source + 142, commit, 72);
    zassert_mem_equal(&state, &previous, sizeof(state));
    zassert_equal(opens + closes + diagnostics_calls + battery_calls + dock_calls + trace_calls +
                      pairing_calls + half_versions_calls + keymap_clears,
                  0);
}
ZTEST(defy_updater, test_clear_keymap_changes_runs_without_session) {
    struct defy_updater previous = state;
    token = sequence = 0;
    size_t length = frame(16, NULL, 0);
    zassert_ok(process(length));
    zassert_equal(sys_get_le16(reply + 12), 0);
    zassert_equal(keymap_clears, 1);
    keymap_clear_failure = -EIO;
    zassert_equal(process(length), -EIO);
    zassert_equal(keymap_clears, 2);
    zassert_mem_equal(&state, &previous, sizeof(state));
    zassert_equal(opens + closes, 0);
}
ZTEST(defy_updater, test_clear_keymap_changes_is_busy_while_a_session_is_open_or_closing) {
    prepare();
    start();
    zassert_ok(page(0x5000));
    struct defy_updater previous = state;
    uint8_t cached_request[DEFY_UPDATER_FRAME];
    memcpy(cached_request, request, sizeof(cached_request));
    uint32_t next = sequence;
    token = sequence = 0;
    zassert_equal(send(16, NULL, 0), -EBUSY);
    zassert_equal(sys_get_le16(reply + 12), 0);
    struct defy_updater_ops missing = ops;
    missing.clear_keymap_changes = NULL;
    state.ops = &missing;
    sequence = 0;
    zassert_equal(send(16, NULL, 0), -EBUSY);
    state.ops = &ops;
    zassert_equal(keymap_clears, 0);
    zassert_mem_equal(&state, &previous, sizeof(state));
    memcpy(request, cached_request, sizeof(request));
    zassert_ok(process(sizeof(request)));
    zassert_equal(sys_get_le32(reply + 16), 0x5100);
    zassert_equal(pages, 1);
    token = 0x12345678;
    sequence = next;
    close_failure = -EIO;
    zassert_equal(send(7, NULL, 0), -EIO);
    zassert_true(state.closing);
    token = sequence = 0;
    zassert_equal(send(16, NULL, 0), -EBUSY);
    zassert_equal(keymap_clears, 0);
    close_failure = 0;
    defy_updater_expire(&state, 100, true);
    defy_updater_expire(&state, 200, true);
    zassert_false(state.closing);
    sequence = 0;
    zassert_ok(send(16, NULL, 0));
    zassert_equal(keymap_clears, 1);
}
ZTEST(defy_updater, test_clear_keymap_changes_without_support) {
    struct defy_updater_ops missing = ops;
    missing.clear_keymap_changes = NULL;
    state.ops = &missing;
    token = sequence = 0;
    zassert_equal(send(16, NULL, 0), -ENOTSUP);
    zassert_equal(sys_get_le16(reply + 12), 0);
    zassert_equal(keymap_clears + opens + closes, 0);
}
ZTEST(defy_updater, test_version_requests_keep_an_active_write_its_cached_reply_and_expiry) {
    prepare();
    start();
    zassert_ok(page(0x5000));
    struct defy_updater previous = state;
    uint8_t cached_request[DEFY_UPDATER_FRAME];
    memcpy(cached_request, request, sizeof(cached_request));
    token = sequence = 0;
    zassert_equal(defy_updater_process(&state, request, frame(14, NULL, 0), reply, 9000), 271);
    zassert_equal((int16_t)sys_get_le16(reply + 14), 0);
    sequence = 0;
    zassert_equal(defy_updater_process(&state, request, frame(15, NULL, 0), reply, 9000), 100);
    zassert_equal((int16_t)sys_get_le16(reply + 14), 0);
    zassert_mem_equal(&state, &previous, sizeof(state));
    memcpy(request, cached_request, sizeof(request));
    zassert_ok(process(sizeof(request)));
    zassert_equal(sys_get_le32(reply + 16), 0x5100);
    zassert_equal(pages, 1);
    defy_updater_expire(&state, 100 + DEFY_UPDATER_IDLE_MS, true);
    zassert_false(state.active);
    zassert_equal(closes, 1);
}
ZTEST(defy_updater, test_invalid_sessionless_requests_never_reach_callbacks_or_close) {
    static const uint8_t opcodes[] = {8, 9, 10, 12, 14, 15, 16};
    prepare();
    struct defy_updater previous = state;
    for (size_t i = 0; i < ARRAY_SIZE(opcodes); i++) {
        token = 0x12345678;
        sequence = 0;
        zassert_equal(send(opcodes[i], NULL, 0), -EINVAL);
        token = 0;
        sequence = 1;
        zassert_equal(send(opcodes[i], NULL, 0), -EINVAL);
        sequence = 0;
        uint8_t extra = 0;
        zassert_equal(send(opcodes[i], &extra, 1), -EINVAL);
        zassert_equal(sys_get_le16(reply + 12), 0);
    }
    zassert_mem_equal(&state, &previous, sizeof(state));
    zassert_equal(diagnostics_calls + battery_calls + dock_calls + pairing_calls +
                      half_versions_calls + keymap_clears + closes,
                  0);
}
ZTEST_SUITE(defy_updater, NULL, NULL, before, NULL, NULL);
