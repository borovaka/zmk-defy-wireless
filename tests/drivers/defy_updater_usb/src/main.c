/* SPDX-License-Identifier: MIT */

#include <zephyr/ztest.h>
#include <zephyr/init.h>
#include <zephyr/drivers/uart.h>
#include <zephyr/settings/settings.h>
#include <zephyr/sys/byteorder.h>
#include <zephyr/sys/crc.h>
#include <zephyr/sys/ring_buffer.h>
#include <zmk/keymap.h>
#include <zmk/studio/core.h>
#include <zmk/usb.h>
#include <setjmp.h>
#include <dygma/defy_update_guard.h>

static int64_t transport_uptime(void);
static int32_t transport_sleep(k_timeout_t timeout);
static int transport_take(struct k_sem *sem, k_timeout_t timeout);
static int transport_callback_set(const struct device *dev, uart_irq_callback_user_data_t cb);
static void transport_rx_enable(const struct device *dev);
static void transport_rx_disable(const struct device *dev);
static void transport_tx_enable(const struct device *dev);
static void transport_tx_disable(const struct device *dev);
static int transport_update(const struct device *dev);
static int transport_pending(const struct device *dev);
static int transport_rx_ready(const struct device *dev);
static int transport_tx_ready(const struct device *dev);
static int transport_read(const struct device *dev, uint8_t *data, int length);
static int transport_fill(const struct device *dev, const uint8_t *data, int length);
static int transport_poll_in(const struct device *dev, unsigned char *c);
static int transport_line(const struct device *dev, uint32_t ctrl, uint32_t *value);
static bool transport_ready(void);
static enum usb_dc_status_code transport_status(void);

#undef SYS_INIT
#define SYS_INIT(...)
#define k_uptime_get transport_uptime
#define k_sleep transport_sleep
#define k_sem_take transport_take
#define uart_irq_callback_set transport_callback_set
#define uart_irq_rx_enable transport_rx_enable
#define uart_irq_rx_disable transport_rx_disable
#define uart_irq_tx_enable transport_tx_enable
#define uart_irq_tx_disable transport_tx_disable
#define uart_irq_update transport_update
#define uart_irq_is_pending transport_pending
#define uart_irq_rx_ready transport_rx_ready
#define uart_irq_tx_ready transport_tx_ready
#define uart_fifo_read transport_read
#define uart_fifo_fill transport_fill
#define uart_poll_in transport_poll_in
#define uart_line_ctrl_get transport_line
#define zmk_usb_is_hid_ready transport_ready
#define zmk_usb_get_status transport_status
#include "../../../../updater/defy_updater_usb.c"
#undef k_uptime_get
#undef k_sleep
#undef k_sem_take
#undef uart_irq_callback_set
#undef uart_irq_rx_enable
#undef uart_irq_rx_disable
#undef uart_irq_tx_enable
#undef uart_irq_tx_disable
#undef uart_irq_update
#undef uart_irq_is_pending
#undef uart_irq_rx_ready
#undef uart_irq_tx_ready
#undef uart_fifo_read
#undef uart_fifo_fill
#undef uart_poll_in
#undef uart_line_ctrl_get
#undef zmk_usb_is_hid_ready
#undef zmk_usb_get_status

/* How long a wait for ms lasts in virtual time, rounded up to whole ticks. */
#define WAIT_MS(ms) ((int64_t)k_ticks_to_ms_ceil64(K_MSEC(ms).ticks))

DEVICE_DT_DEFINE(DT_NODELABEL(updater_test_cdc), NULL, NULL, NULL, NULL, POST_KERNEL, 40,
                 NULL);

static int pause_error;
static int resume_error;
static int reset_error;
static int begin_error[2];
static int abort_error;
static bool paused;
static unsigned int pause_calls;
static unsigned int resume_calls;
static unsigned int reset_calls;
static unsigned int recovery_calls;
static unsigned int begin_calls[2];
static unsigned int abort_calls;
static unsigned int acquired_aborts;
static unsigned int flash_calls;
static int pairing_clear_error;
static int erase_settings_error[2];
static unsigned int keymap_resets;
static int keymap_reset_error;
/* How many keymap keys the store held when ZMK's reset ran. */
static size_t keymap_keys_at_reset;
static enum zmk_studio_core_lock_state studio_lock;
/* What RESET PAIRING cleared, in order: N for the Neuron's side, L and R for the halves. */
static char resets[8];
static size_t reset_count;
static uint32_t sequence;
static jmp_buf transport_exit;
static int64_t transport_now;
static int64_t transport_stop;
static int64_t transport_disconnect;
static int64_t transport_reconnect;
static int64_t transport_repair;
static uint8_t input[1024];
static int64_t input_at[sizeof(input)];
static size_t input_length;
static size_t input_offset;
static uint8_t output[1024];
static size_t output_length;
static int fill_limit;
/* The host reads stall_at bytes of output, then nothing more until stall_until. */
static size_t stall_at;
static int64_t stall_until;
static bool hid_ready;
static bool suspended;
static int line_error;
static uart_irq_callback_user_data_t serial_handler;
static bool rx_enabled;
static bool tx_enabled;
static bool in_handler;
static unsigned int rx_pauses;
static unsigned int blocked_waits;
/* When the thread last began to wait for input without a timeout, or -1 if it then had one. */
static int64_t forever_at;
static int64_t released_at;
/* When the USB bus reset, or -1. */
static int64_t usb_reset_at;
NRF_POWER_Type fake_power;

static int64_t transport_uptime(void) { return transport_now; }

/* Moves virtual time on, and ends the run once it reaches transport_stop. */
static void advance(int64_t until) {
    transport_now = MIN(until, transport_stop);
    if (transport_now >= transport_repair) {
        abort_error = resume_error = pause_error = reset_error = 0;
    }
    if (transport_now >= transport_stop) {
        longjmp(transport_exit, 1);
    }
}

static int32_t transport_sleep(k_timeout_t timeout) {
    advance(transport_now + k_ticks_to_ms_ceil64(timeout.ticks));
    return 0;
}

static bool input_due(void) {
    return input_offset < input_length && input_at[input_offset] <= transport_now;
}

/*
 * CDC ACM's "data waiting" flag. A USB reset clears it but keeps the bytes in the
 * port's buffer, and only data arriving after the reset sets it again.
 */
static bool rx_flagged(void) {
    if (!input_due()) {
        return false;
    }
    if (usb_reset_at < 0 || transport_now < usb_reset_at) {
        return true;
    }
    for (size_t i = input_offset; i < input_length && input_at[i] <= transport_now; i++) {
        if (input_at[i] >= usb_reset_at) {
            return true;
        }
    }
    return false;
}

static bool host_reading(void) {
    return fill_limit != 0 && (output_length < stall_at || transport_now >= stall_until);
}

/* The UART runs its callback while RX is on with input waiting, or TX is on with room. */
static void serial_interrupt(void) {
    if (!serial_handler || in_handler ||
        !((rx_enabled && rx_flagged()) || (tx_enabled && host_reading()))) {
        return;
    }
    in_handler = true;
    serial_handler(updater_uart, NULL);
    in_handler = false;
}

/* When the next byte arrives from the host. */
static int64_t next_input_at(void) {
    for (size_t i = input_offset; i < input_length; i++) {
        if (input_at[i] > transport_now) {
            return input_at[i];
        }
    }
    return INT64_MAX;
}

/* Runs the UART callback for what is due, then lets virtual time pass until the semaphore is
 * given, the timeout ends or the next input byte arrives. */
static int transport_take(struct k_sem *sem, k_timeout_t timeout) {
    serial_interrupt();
    if (k_sem_take(sem, K_NO_WAIT) == 0) {
        return 0;
    }
    if (K_TIMEOUT_EQ(timeout, K_NO_WAIT)) {
        return -EBUSY;
    }
    bool forever = K_TIMEOUT_EQ(timeout, K_FOREVER);
    if (sem == &rx_ready) {
        blocked_waits++;
        forever_at = forever ? transport_now : -1;
    }
    int64_t wake = forever ? INT64_MAX : transport_now + k_ticks_to_ms_ceil64(timeout.ticks);
    if (serial_handler && rx_enabled) {
        wake = MIN(wake, next_input_at());
    }
    advance(wake);
    serial_interrupt();
    return k_sem_take(sem, K_NO_WAIT) == 0 ? 0 : -EAGAIN;
}

static int transport_callback_set(const struct device *dev, uart_irq_callback_user_data_t cb) {
    zassert_equal_ptr(dev, updater_uart);
    serial_handler = cb;
    return 0;
}

static void transport_rx_enable(const struct device *dev) {
    zassert_equal_ptr(dev, updater_uart);
    rx_enabled = true;
    serial_interrupt();
}

static void transport_rx_disable(const struct device *dev) {
    zassert_equal_ptr(dev, updater_uart);
    rx_enabled = false;
    rx_pauses++;
}

static void transport_tx_enable(const struct device *dev) {
    zassert_equal_ptr(dev, updater_uart);
    tx_enabled = true;
    serial_interrupt();
}

static void transport_tx_disable(const struct device *dev) {
    zassert_equal_ptr(dev, updater_uart);
    tx_enabled = false;
}

/* uart.h allows the ready checks and FIFO calls only inside the UART callback. */
static int transport_update(const struct device *dev) {
    zassert_equal_ptr(dev, updater_uart);
    zassert_true(in_handler);
    return 1;
}

static int transport_rx_ready(const struct device *dev) {
    zassert_equal_ptr(dev, updater_uart);
    zassert_true(in_handler);
    return rx_enabled && rx_flagged();
}

static int transport_tx_ready(const struct device *dev) {
    zassert_equal_ptr(dev, updater_uart);
    zassert_true(in_handler);
    return tx_enabled && host_reading();
}

static int transport_pending(const struct device *dev) {
    return transport_rx_ready(dev) || transport_tx_ready(dev);
}

static int transport_read(const struct device *dev, uint8_t *data, int length) {
    zassert_equal_ptr(dev, updater_uart);
    zassert_true(in_handler);
    int count = 0;
    while (count < length && input_due()) {
        data[count++] = input[input_offset++];
    }
    return count;
}

static int transport_fill(const struct device *dev, const uint8_t *data, int length) {
    zassert_equal_ptr(dev, updater_uart);
    zassert_true(in_handler);
    if (fill_limit <= 0) {
        return fill_limit;
    }
    int count = MIN(length, fill_limit);
    if (transport_now < stall_until) {
        count = MIN(count, (int)(stall_at - output_length));
    }
    zassert_true(output_length + count <= sizeof(output));
    memcpy(output + output_length, data, count);
    output_length += count;
    return count;
}

/* Polled reads go to the same buffer; the thread may use them only while RX is off. */
static int transport_poll_in(const struct device *dev, unsigned char *c) {
    zassert_equal_ptr(dev, updater_uart);
    zassert_false(in_handler);
    zassert_false(rx_enabled);
    if (!input_due()) {
        return -1;
    }
    *c = input[input_offset++];
    return 0;
}

static int transport_line(const struct device *dev, uint32_t ctrl, uint32_t *value) {
    zassert_equal_ptr(dev, updater_uart);
    zassert_equal(ctrl, UART_LINE_CTRL_DTR);
    *value = transport_now < transport_disconnect || transport_now >= transport_reconnect;
    return line_error;
}

static bool transport_ready(void) { return hid_ready; }

static enum usb_dc_status_code transport_status(void) {
    return suspended ? USB_DC_SUSPEND : USB_DC_CONFIGURED;
}

static void require_owned(void) {
    zassert_false(defy_update_claim(DEFY_UPDATE_NEURON));
}

int defy_hybrid_central_set_maintenance(bool value) {
    require_owned();
    if (value) {
        pause_calls++;
        paused = true;
        return pause_error;
    }
    resume_calls++;
    zassert_true(!loader.acquired || release_failures >= HALF_RELEASE_ATTEMPTS);
    if (!resume_error) {
        paused = false;
    }
    return resume_error;
}

int defy_half_bootloader_reset_all(struct defy_half_bootloader *instance) {
    require_owned();
    zassert_equal_ptr(instance, &loader);
    zassert_true(paused);
    zassert_true(halves_held());
    reset_calls++;
    instance->acquired = true;
    return reset_error;
}

int defy_half_bootloader_recover(struct defy_half_bootloader *instance) {
    require_owned();
    zassert_equal_ptr(instance, &loader);
    zassert_true(paused);
    recovery_calls++;
    instance->acquired = reset_error != 0;
    return reset_error;
}

int defy_half_bootloader_begin(struct defy_half_bootloader *instance, enum defy_wired_side side,
                               struct defy_half_bootloader_info *info) {
    require_owned();
    zassert_equal_ptr(instance, &loader);
    zassert_true(instance->acquired);
    zassert_true(paused);
    zassert_true(halves_held());
    zassert_true(side < 2);
    begin_calls[side]++;
    *info = (struct defy_half_bootloader_info){.hardware_version = 1, .flash_start = 0x5000};
    return begin_error[side];
}

int defy_half_bootloader_abort(struct defy_half_bootloader *instance) {
    require_owned();
    zassert_equal_ptr(instance, &loader);
    abort_calls++;
    if (!instance->acquired) {
        return 0;
    }
    acquired_aborts++;
    if (!abort_error) {
        instance->acquired = false;
        released_at = transport_now;
    }
    return abort_error;
}

int defy_half_bootloader_read_seal(struct defy_half_bootloader *instance,
                                   enum defy_wired_side side, struct defy_half_seal *seal) {
    require_owned();
    zassert_equal_ptr(instance, &loader);
    *seal = (struct defy_half_seal){.program_version = 7, .program_crc = 0x1234};
    return 0;
}

int defy_half_bootloader_erase_sector(struct defy_half_bootloader *instance,
                                     enum defy_wired_side side, uint32_t address) {
    flash_calls++;
    return -EIO;
}

int defy_half_bootloader_write_page(struct defy_half_bootloader *instance,
                                   enum defy_wired_side side, uint32_t address,
                                   const uint8_t data[DEFY_HALF_PAGE_SIZE]) {
    flash_calls++;
    return -EIO;
}

int defy_half_bootloader_validate(struct defy_half_bootloader *instance,
                                 enum defy_wired_side side, struct defy_half_seal *seal) {
    flash_calls++;
    return -EIO;
}

int defy_half_bootloader_erase_settings(struct defy_half_bootloader *instance,
                                       enum defy_wired_side side) {
    require_owned();
    zassert_equal_ptr(instance, &loader);
    zassert_true(instance->acquired);
    zassert_true(paused);
    zassert_true(side < 2);
    zassert_true(reset_count < sizeof(resets));
    resets[reset_count++] = side == DEFY_WIRED_LEFT ? 'L' : 'R';
    return erase_settings_error[side];
}

int defy_split_pairing_clear(void) {
    require_owned();
    zassert_true(paused);
    zassert_true(loader.acquired);
    zassert_true(reset_count < sizeof(resets));
    resets[reset_count++] = 'N';
    return pairing_clear_error;
}

size_t defy_split_pairing_status(uint8_t *output, size_t capacity) {
    return 0;
}

size_t defy_hybrid_ble_diagnostics(uint8_t *output, size_t capacity) {
    return 0;
}

size_t defy_neuron_dock_status(uint8_t *output, size_t capacity) {
    return 0;
}

size_t defy_key_trace_read(uint32_t from, bool clear, uint8_t *out, size_t capacity) {
    return 0;
}

/* The records hold no report. */
size_t defy_half_versions(uint8_t *output, size_t capacity) {
    return 0;
}

/* The test's settings store: what loads read, and what deletes change. */
static struct {
    char name[24];
    uint8_t value[10];
    size_t length;
    bool used;
} entries[12];
static unsigned int store_loads;
static unsigned int store_deletes;
/* The key whose delete fails, with delete_error. */
static const char *failing_key;
static int delete_error;

static void put_entry(const char *name, const void *value, size_t length) {
    for (size_t i = 0; i < ARRAY_SIZE(entries); i++) {
        if (!entries[i].used) {
            zassert_true(strlen(name) < sizeof(entries[i].name));
            zassert_true(length <= sizeof(entries[i].value));
            strcpy(entries[i].name, name);
            memcpy(entries[i].value, value, length);
            entries[i].length = length;
            entries[i].used = true;
            return;
        }
    }
    zassert_unreachable("the store is full");
}

static bool stored(const char *name) {
    for (size_t i = 0; i < ARRAY_SIZE(entries); i++) {
        if (entries[i].used && strcmp(entries[i].name, name) == 0) {
            return true;
        }
    }
    return false;
}

static size_t stored_keymap_keys(void) {
    size_t count = 0;
    for (size_t i = 0; i < ARRAY_SIZE(entries); i++) {
        count += entries[i].used && strncmp(entries[i].name, "keymap/", 7) == 0;
    }
    return count;
}

static ssize_t read_entry(void *cb_arg, void *data, size_t length) {
    const __typeof__(entries[0]) *entry = cb_arg;
    size_t count = MIN(length, entry->length);
    memcpy(data, entry->value, count);
    return count;
}

/* As the NVS backend does, a handler's nonzero return ends the load. */
static int store_load(struct settings_store *cs, const struct settings_load_arg *arg) {
    ARG_UNUSED(cs);
    store_loads++;
    for (size_t i = 0; i < ARRAY_SIZE(entries); i++) {
        if (entries[i].used &&
            settings_call_set_handler(entries[i].name, entries[i].length, read_entry,
                                      &entries[i], arg)) {
            break;
        }
    }
    return 0;
}

static int store_save(struct settings_store *cs, const char *name, const char *value,
                      size_t length) {
    ARG_UNUSED(cs);
    zassert_true(!value || !length, "only deletes are expected");
    store_deletes++;
    if (failing_key && strcmp(name, failing_key) == 0) {
        return delete_error;
    }
    for (size_t i = 0; i < ARRAY_SIZE(entries); i++) {
        if (entries[i].used && strcmp(entries[i].name, name) == 0) {
            entries[i].used = false;
        }
    }
    return 0;
}

static const struct settings_store_itf store_itf = {
    .csi_load = store_load,
    .csi_save = store_save,
};
static struct settings_store store = {.cs_itf = &store_itf};

int settings_backend_init(void) {
    settings_src_register(&store);
    settings_dst_register(&store);
    return 0;
}

enum zmk_studio_core_lock_state zmk_studio_core_get_lock_state(void) { return studio_lock; }

int zmk_keymap_reset_settings(void) {
    zassert_false(state.active);
    zassert_false(state.closing);
    keymap_resets++;
    keymap_keys_at_reset = stored_keymap_keys();
    return keymap_reset_error;
}

static int request(uint8_t opcode) {
    uint8_t frame[20] = {'D', 'U', 1, opcode};
    uint8_t reply[DEFY_UPDATER_FRAME];
    sys_put_le32(0x12345678, frame + 4);
    sys_put_le32(sequence++, frame + 8);
    sys_put_le32(crc32_ieee(frame, 16), frame + 16);
    zassert_equal(defy_updater_process(&state, frame, sizeof(frame), reply, 100), 20);
    zassert_equal(reply[3], opcode | 0x80);
    return (int16_t)sys_get_le16(reply + 14);
}

static void queue_bytes(int64_t at, const uint8_t *bytes, size_t length) {
    zassert_true(input_length + length <= sizeof(input));
    for (size_t i = 0; i < length; i++) {
        input[input_length] = bytes[i];
        input_at[input_length++] = at;
    }
}

static size_t build_request(uint8_t frame[DEFY_UPDATER_FRAME], uint8_t opcode,
                            const uint8_t *payload, size_t length) {
    memset(frame, 0, DEFY_UPDATER_FRAME);
    frame[0] = 'D';
    frame[1] = 'U';
    frame[2] = 1;
    frame[3] = opcode;
    sys_put_le32(0x12345678, frame + 4);
    sys_put_le32(sequence++, frame + 8);
    sys_put_le16(length, frame + 12);
    if (length) {
        memcpy(frame + 16, payload, length);
    }
    sys_put_le32(crc32_ieee(frame, 16 + length), frame + 16 + length);
    return 20 + length;
}

static void queue_payload(int64_t at, uint8_t opcode, const uint8_t *payload, size_t length) {
    uint8_t frame[DEFY_UPDATER_FRAME];
    queue_bytes(at, frame, build_request(frame, opcode, payload, length));
}

static void queue_request(int64_t at, uint8_t opcode) {
    queue_payload(at, opcode, NULL, 0);
}

/* A request without a session or payload, such as HELLO, which leaves nothing cached. */
static void queue_sessionless(int64_t at, uint8_t opcode) {
    uint8_t frame[20] = {'D', 'U', 1, opcode};
    sys_put_le32(crc32_ieee(frame, 16), frame + 16);
    queue_bytes(at, frame, sizeof(frame));
}

static void queue_hello(int64_t at) {
    queue_sessionless(at, 1);
}

static void run_transport(int64_t until) {
    transport_stop = until;
    if (!setjmp(transport_exit)) {
        run_updater(NULL, NULL, NULL);
    }
}

static void reply_status(size_t index, uint8_t opcode, int status) {
    size_t offset = index * 20;
    zassert_true(output_length >= offset + 20);
    zassert_mem_equal(output + offset, "DU", 2);
    zassert_equal(output[offset + 3], opcode | 0x80);
    zassert_equal((int16_t)sys_get_le16(output + offset + 14), status);
    zassert_equal(sys_get_le32(output + offset + 16), crc32_ieee(output + offset, 16));
}

#define HELLO_REPLY 44

static void hello_reply(size_t offset) {
    zassert_true(output_length >= offset + HELLO_REPLY);
    zassert_mem_equal(output + offset, "DU", 2);
    zassert_equal(output[offset + 3], 0x81);
    zassert_equal(sys_get_le16(output + offset + 12), 24);
    zassert_equal(sys_get_le16(output + offset + 14), 0);
    zassert_mem_equal(output + offset + 16, "DEFYUPD1", 8);
    zassert_equal(sys_get_le32(output + offset + 40), crc32_ieee(output + offset, 40));
}

static void retry_cleanup(bool connected) {
    unsigned int aborts = abort_calls;
    unsigned int resumes = resume_calls;
    defy_updater_expire(&state, 1000, connected);
    zassert_true(state.closing);
    zassert_equal(abort_calls, aborts);
    zassert_equal(resume_calls, resumes);
    zassert_true(state.retry_at > 1000);
    defy_updater_expire(&state, state.retry_at, connected);
}

static void expire_until_closed(void) {
    for (int64_t now = 1000; state.closing && now < 10000; now += 10) {
        defy_updater_expire(&state, now, false);
    }
}

static void neuron_reset(void) {
    defy_update_release(DEFY_UPDATE_HALVES);
    defy_updater_init(&state, &operations);
    memset(&loader, 0, sizeof(loader));
    startup_recovery = false;
    release_failures = 0;
    paused = false;
    transport_disconnect = 0;
}

static void recovered(void) {
    zassert_false(state.active);
    zassert_false(state.closing);
    zassert_false(paused);
    zassert_false(loader.acquired);
    zassert_false(halves_held());
    zassert_equal(flash_calls, 0);
    zassert_true(defy_update_claim(DEFY_UPDATE_NEURON));
    defy_update_release(DEFY_UPDATE_NEURON);
}

static void before(void *fixture) {
    ARG_UNUSED(fixture);
    defy_update_release(DEFY_UPDATE_HALVES);
    defy_update_release(DEFY_UPDATE_NEURON);
    defy_updater_init(&state, &operations);
    memset(&loader, 0, sizeof(loader));
    startup_recovery = false;
    release_failures = 0;
    memset(&fake_power, 0, sizeof(fake_power));
    pause_error = resume_error = reset_error = abort_error = 0;
    memset(begin_error, 0, sizeof(begin_error));
    paused = false;
    pause_calls = resume_calls = reset_calls = abort_calls = acquired_aborts = flash_calls = 0;
    recovery_calls = 0;
    memset(begin_calls, 0, sizeof(begin_calls));
    pairing_clear_error = 0;
    memset(erase_settings_error, 0, sizeof(erase_settings_error));
    keymap_resets = 0;
    keymap_reset_error = 0;
    keymap_keys_at_reset = SIZE_MAX;
    studio_lock = ZMK_STUDIO_CORE_LOCK_STATE_LOCKED;
    zassert_ok(settings_subsys_init());
    memset(entries, 0, sizeof(entries));
    store_loads = store_deletes = 0;
    failing_key = NULL;
    delete_error = 0;
    memset(resets, 0, sizeof(resets));
    reset_count = 0;
    sequence = 1;
    transport_now = 0;
    transport_stop = transport_disconnect = transport_reconnect = transport_repair = INT64_MAX;
    input_length = input_offset = output_length = 0;
    fill_limit = INT_MAX;
    stall_at = SIZE_MAX;
    stall_until = 0;
    hid_ready = true;
    suspended = false;
    line_error = 0;
    serial_handler = NULL;
    rx_enabled = tx_enabled = in_handler = false;
    usb_reset_at = -1;
    rx_pauses = blocked_waits = 0;
    forever_at = released_at = -1;
    k_sem_reset(&rx_ready);
    k_sem_reset(&tx_done);
    tx_error = 0;
    atomic_clear(&rx_paused);
    zassert_ok(serial_init());
}

ZTEST(updater_usb, test_failed_maintenance_entry_rolls_back_without_resetting_halves) {
    pause_error = -EBUSY;
    zassert_equal(request(2), -EBUSY);
    zassert_equal(pause_calls, 1);
    zassert_equal(resume_calls, 1);
    zassert_equal(abort_calls, 1);
    zassert_equal(reset_calls + begin_calls[0] + begin_calls[1] + acquired_aborts, 0);
    recovered();
}

ZTEST(updater_usb, test_failed_open_rollback_retries_transport_resume) {
    pause_error = -EIO;
    resume_error = -EBUSY;
    zassert_equal(request(2), -EIO);
    zassert_true(state.closing);
    zassert_true(paused);
    zassert_equal(reset_calls + begin_calls[0] + begin_calls[1], 0);
    zassert_equal(resume_calls, 1);
    require_owned();
    resume_error = 0;
    retry_cleanup(true);
    zassert_equal(resume_calls, 2);
    zassert_equal(reset_calls + acquired_aborts, 0);
    recovered();
}

ZTEST(updater_usb, test_failed_reset_defers_resume_until_abort_succeeds) {
    reset_error = -EIO;
    abort_error = -ETIMEDOUT;
    zassert_equal(request(2), -EIO);
    zassert_true(state.closing);
    zassert_true(loader.acquired);
    zassert_true(paused);
    zassert_equal(reset_calls, 1);
    zassert_equal(begin_calls[0] + begin_calls[1] + resume_calls, 0);
    zassert_true(halves_held());
    require_owned();
    abort_error = 0;
    retry_cleanup(false);
    zassert_equal(abort_calls, 2);
    zassert_equal(resume_calls, 1);
    recovered();
}

ZTEST(updater_usb, test_failure_beginning_either_half_runs_cleanup_once) {
    for (size_t side = 0; side < 2; side++) {
        before(NULL);
        begin_error[side] = -EIO;
        zassert_equal(request(2), -EIO);
        zassert_equal(reset_calls, 1);
        zassert_equal(begin_calls[0], 1);
        zassert_equal(begin_calls[1], side);
        zassert_equal(acquired_aborts, 1);
        zassert_equal(resume_calls, 1);
        recovered();
    }
}

ZTEST(updater_usb, test_close_resume_failure_does_not_repeat_physical_abort) {
    zassert_ok(request(2));
    resume_error = -EBUSY;
    zassert_equal(request(7), -EBUSY);
    zassert_false(loader.acquired);
    zassert_true(state.closing);
    zassert_true(paused);
    zassert_equal(acquired_aborts, 1);
    zassert_equal(resume_calls, 1);
    require_owned();
    resume_error = 0;
    retry_cleanup(false);
    zassert_equal(abort_calls, 2);
    zassert_equal(acquired_aborts, 1);
    zassert_equal(reset_calls, 1);
    zassert_equal(resume_calls, 2);
    recovered();
}

ZTEST(updater_usb, test_release_failure_resumes_after_bounded_attempts) {
    zassert_ok(request(2));
    abort_error = -EIO;
    zassert_equal(request(7), -EIO);
    expire_until_closed();
    zassert_false(state.closing);
    zassert_equal(acquired_aborts, HALF_RELEASE_ATTEMPTS);
    zassert_equal(resume_calls, 1);
    zassert_false(paused);
    zassert_true(loader.acquired);
    zassert_true(halves_held());
    zassert_true(defy_update_claim(DEFY_UPDATE_NEURON));
    defy_update_release(DEFY_UPDATE_NEURON);
    abort_error = 0;
    neuron_reset();
    run_transport(10);
    zassert_equal(recovery_calls, 1);
    zassert_equal(resume_calls, 2);
    recovered();
}

/* A boot without the marker for held halves leaves them running, with no recovery. */
ZTEST(updater_usb, test_transport_open_close_with_fragmented_replies) {
    fill_limit = 3;
    queue_request(0, 2);
    queue_request(10, 7);
    run_transport(20);
    zassert_equal(recovery_calls, 0);
    zassert_equal(output_length, 40);
    reply_status(0, 2, 0);
    reply_status(1, 7, 0);
    zassert_equal(acquired_aborts, 1);
    recovered();
}

ZTEST(updater_usb, test_transport_disconnect_retries_cleanup_without_host) {
    queue_request(0, 2);
    transport_disconnect = 10;
    abort_error = -EIO;
    transport_repair = 300;
    run_transport(1000);
    reply_status(0, 2, 0);
    zassert_true(abort_calls > 1);
    zassert_equal(resume_calls, 1);
    recovered();
}

ZTEST(updater_usb, test_transport_reply_error_closes_session) {
    fill_limit = -EIO;
    queue_request(0, 2);
    run_transport(10);
    zassert_equal(output_length, 0);
    zassert_equal(acquired_aborts, 1);
    recovered();
}

ZTEST(updater_usb, test_transport_partial_frame_timeout_resynchronizes) {
    uint8_t partial[] = {'D', 'U', 1, 2, 0};
    queue_bytes(0, partial, sizeof(partial));
    queue_request(1010, 2);
    queue_request(1020, 7);
    run_transport(1030);
    reply_status(0, 2, 0);
    reply_status(1, 7, 0);
    zassert_equal(reset_calls, 1);
    recovered();
}

ZTEST(updater_usb, test_transport_invalid_frames_do_not_acquire_halves) {
    uint8_t frame[20] = {'D', 'U', 1, 2};
    sys_put_le32(1, frame + 4);
    sys_put_le32(1, frame + 8);
    queue_bytes(0, frame, sizeof(frame));
    sys_put_le16(DEFY_UPDATER_PAYLOAD + 1, frame + 12);
    queue_bytes(0, frame, sizeof(frame));
    frame[2] = 2;
    queue_bytes(0, frame, sizeof(frame));
    frame[2] = 1;
    frame[3] |= 0x80;
    queue_bytes(0, frame, sizeof(frame));
    queue_request(10, 2);
    queue_request(20, 7);
    run_transport(30);
    zassert_equal(output_length, 40);
    zassert_equal(reset_calls, 1);
    recovered();
}

ZTEST(updater_usb, test_transport_disconnect_discards_partial_frame) {
    uint8_t partial[] = {'D', 'U', 1, 2, 0};
    queue_bytes(0, partial, sizeof(partial));
    transport_disconnect = 10;
    transport_reconnect = 120;
    queue_request(130, 2);
    queue_request(140, 7);
    run_transport(400);
    reply_status(0, 2, 0);
    reply_status(1, 7, 0);
    zassert_equal(reset_calls, 1);
    recovered();
}

ZTEST(updater_usb, test_transport_drops_requests_while_disconnected) {
    transport_disconnect = 0;
    transport_reconnect = 100;
    queue_request(0, 2);
    sequence = 1;
    queue_request(100, 2);
    queue_request(110, 7);
    run_transport(120);
    zassert_equal(output_length, 40);
    zassert_equal(reset_calls, 1);
    recovered();
}

ZTEST(updater_usb, test_transport_requires_ready_resumed_usb_and_dtr) {
    for (size_t mode = 0; mode < 3; mode++) {
        before(NULL);
        hid_ready = mode != 0;
        suspended = mode == 1;
        line_error = mode == 2 ? -EIO : 0;
        queue_request(0, 2);
        run_transport(100);
        zassert_equal(output_length + reset_calls, 0);
        zassert_equal(pause_calls + recovery_calls, 0);
        recovered();
    }
}

/* VERSION's reply in a build given no values: unknown, local and an empty commit. */
ZTEST(updater_usb, test_version_reports_the_defaults_through_the_serial_port) {
    const uint8_t payload[19] = {1, 0, 0, 0, 7, 'u', 'n', 'k', 'n', 'o', 'w', 'n',
                                 5, 'l', 'o', 'c', 'a', 'l', 0};
    queue_sessionless(0, 14);
    run_transport(10);
    zassert_equal(output_length, 16 + sizeof(payload) + 4);
    zassert_equal(output[3], 14 | 0x80);
    zassert_equal(sys_get_le16(output + 12), sizeof(payload));
    zassert_equal(sys_get_le16(output + 14), 0);
    zassert_mem_equal(output + 16, payload, sizeof(payload));
    zassert_equal(sys_get_le32(output + 16 + sizeof(payload)),
                  crc32_ieee(output, 16 + sizeof(payload)));
}

ZTEST(updater_usb, test_clear_keymap_changes_waits_for_startup_recovery) {
    mark_halves_held(true);
    reset_error = -EIO;
    transport_repair = 50;
    queue_sessionless(0, 16);
    queue_sessionless(100, 16);
    run_transport(110);
    zassert_equal(output_length, 40);
    reply_status(0, 16, -EBUSY);
    reply_status(1, 16, 0);
    zassert_equal(keymap_resets, 1);
    recovered();
}

/* CLEAR KEYMAP CHANGES, without a session. */
static int clear_keymap(void) {
    uint8_t frame[20] = {'D', 'U', 1, 16};
    uint8_t reply[DEFY_UPDATER_FRAME];
    sys_put_le32(crc32_ieee(frame, 16), frame + 16);
    zassert_equal(defy_updater_process(&state, frame, sizeof(frame), reply, 100), 20);
    zassert_equal(reply[3], 16 | 0x80);
    return (int16_t)sys_get_le16(reply + 14);
}

/* What Studio saves, by the key formats of ZMK's keymap.c, beside keys that are not the keymap's. */
static void store_studio_changes(void) {
    /* A mod-tap with its tap key, the second parameter, changed: local ID 3, left shift, B.
     * zmk_keymap_reset_settings() alone would keep this key. */
    const uint8_t mod_tap[10] = {3, 0, 0xe1, 0, 7, 0, 0x05, 0, 7, 0};
    const uint8_t order[3] = {0, 2, 1};
    put_entry("keymap/l/1/11", mod_tap, sizeof(mod_tap));
    put_entry("keymap/l/0/0", mod_tap, 6);
    put_entry("keymap/l_n/2", "Arrows", 6);
    put_entry("keymap/layer_order", order, sizeof(order));
    /* From an image with more layers or positions, and a form keymap.c does not write. */
    put_entry("keymap/l/3/0", mod_tap, 6);
    put_entry("keymap/l/0/12", mod_tap, 6);
    put_entry("keymap/l/01/2", mod_tap, 6);
    put_entry("keymap/l_n/3", "Old", 3);
    /* Not the keymap's. */
    put_entry("ble/profiles/0", order, 1);
    put_entry("keymapx/0", order, 1);
}

ZTEST(updater_usb, test_clear_keymap_changes_deletes_every_saved_key_then_reloads) {
    store_studio_changes();
    zassert_ok(clear_keymap());
    zassert_equal(stored_keymap_keys(), 0);
    zassert_true(stored("ble/profiles/0"));
    zassert_true(stored("keymapx/0"));
    zassert_equal(store_deletes, 8);
    /* One load collects; each of the four others takes one more. */
    zassert_equal(store_loads, 5);
    zassert_equal(keymap_resets, 1);
    zassert_equal(keymap_keys_at_reset, 0);
    zassert_false(state.active);
    zassert_false(state.closing);
}

ZTEST(updater_usb, test_clear_keymap_changes_passes_the_first_delete_error_and_still_reloads) {
    store_studio_changes();
    failing_key = "keymap/l_n/2";
    delete_error = -EIO;
    zassert_equal(clear_keymap(), -EIO);
    /* The rest of the keys this keymap uses go; the others wait for a retry. */
    zassert_true(stored("keymap/l_n/2"));
    zassert_false(stored("keymap/l/1/11"));
    zassert_false(stored("keymap/l/0/0"));
    zassert_false(stored("keymap/layer_order"));
    zassert_equal(stored_keymap_keys(), 5);
    zassert_equal(store_loads, 1);
    zassert_equal(keymap_resets, 1);
    failing_key = NULL;
    zassert_ok(clear_keymap());
    zassert_equal(stored_keymap_keys(), 0);
    zassert_equal(keymap_resets, 2);

    before(NULL);
    store_studio_changes();
    failing_key = "keymap/l/3/0";
    delete_error = -ENOSPC;
    zassert_equal(clear_keymap(), -ENOSPC);
    zassert_true(stored("keymap/l/3/0"));
    zassert_equal(keymap_resets, 1);

    before(NULL);
    store_studio_changes();
    keymap_reset_error = -ENOTSUP;
    zassert_equal(clear_keymap(), -ENOTSUP);
    zassert_equal(stored_keymap_keys(), 0);
}

ZTEST(updater_usb, test_clear_keymap_changes_is_busy_while_studio_is_unlocked) {
    store_studio_changes();
    studio_lock = ZMK_STUDIO_CORE_LOCK_STATE_UNLOCKED;
    zassert_equal(clear_keymap(), -EBUSY);
    zassert_equal(store_loads + store_deletes + keymap_resets, 0);
    zassert_equal(stored_keymap_keys(), 8);
    studio_lock = ZMK_STUDIO_CORE_LOCK_STATE_LOCKED;
    zassert_ok(clear_keymap());
    zassert_equal(stored_keymap_keys(), 0);
}

/* OPEN, both PROBEs, RESET PAIRING and CLOSE through the serial port. */
static void run_pairing_reset(int status) {
    uint8_t left = 0;
    uint8_t right = 1;
    queue_request(0, 2);
    queue_payload(0, 3, &left, 1);
    queue_payload(0, 3, &right, 1);
    queue_request(0, 13);
    queue_request(0, 7);
    run_transport(10);
    zassert_equal(output_length, 140);
    reply_status(0, 2, 0);
    reply_status(5, 13, status);
    /* A failed command has closed the session already. */
    reply_status(6, 7, status ? -ENOTCONN : 0);
    zassert_equal(acquired_aborts, 1);
    zassert_equal(resume_calls, 1);
    recovered();
}

ZTEST(updater_usb, test_reset_pairing_clears_the_neuron_then_the_left_then_the_right) {
    run_pairing_reset(0);
    zassert_str_equal(resets, "NLR");
}

ZTEST(updater_usb, test_failed_neuron_clear_erases_no_half) {
    pairing_clear_error = -EIO;
    run_pairing_reset(-EIO);
    zassert_str_equal(resets, "N");
}

ZTEST(updater_usb, test_failed_settings_erase_stops_and_releases_the_halves) {
    erase_settings_error[DEFY_WIRED_LEFT] = -EIO;
    run_pairing_reset(-EIO);
    zassert_str_equal(resets, "NL");
    before(NULL);
    erase_settings_error[DEFY_WIRED_RIGHT] = -ERANGE;
    run_pairing_reset(-ERANGE);
    zassert_str_equal(resets, "NLR");
}

ZTEST(updater_usb, test_startup_recovers_before_accepting_host_commands) {
    mark_halves_held(true);
    queue_request(0, 2);
    queue_request(10, 7);
    run_transport(20);
    zassert_equal(recovery_calls, 1);
    zassert_equal(pause_calls, 2);
    zassert_equal(resume_calls, 2);
    reply_status(0, 2, 0);
    reply_status(1, 7, 0);
    recovered();
}

ZTEST(updater_usb, test_startup_recovers_after_losing_active_session_state) {
    zassert_ok(request(2));
    zassert_true(loader.acquired);
    neuron_reset();
    run_transport(10);
    zassert_equal(recovery_calls, 1);
    zassert_equal(acquired_aborts, 0);
    recovered();
}

/* OPEN is refused while maintenance fails, and the halves are recovered once it is available. */
ZTEST(updater_usb, test_startup_retries_failed_maintenance_before_any_reset) {
    mark_halves_held(true);
    pause_error = -EBUSY;
    transport_repair = 200;
    queue_request(0, 2);
    run_transport(1000);
    reply_status(0, 2, -EBUSY);
    zassert_true(pause_calls > 1);
    zassert_equal(reset_calls, 0);
    zassert_equal(recovery_calls, 1);
    zassert_equal(resume_calls, 1);
    recovered();
}

/* OPEN is refused while the reset fails, and the halves are released once it succeeds. */
ZTEST(updater_usb, test_startup_reset_failure_blocks_host_updates_and_retries) {
    mark_halves_held(true);
    reset_error = -EIO;
    transport_repair = 200;
    queue_request(0, 2);
    run_transport(1000);
    reply_status(0, 2, -EBUSY);
    zassert_true(recovery_calls > 1);
    zassert_equal(reset_calls, 0);
    zassert_equal(resume_calls, 1);
    recovered();
}

ZTEST(updater_usb, test_startup_permanent_reset_failure_resumes_typing) {
    mark_halves_held(true);
    reset_error = -EIO;
    transport_disconnect = 0;
    run_transport(5000);
    zassert_equal(recovery_calls, HALF_RELEASE_ATTEMPTS);
    zassert_equal(resume_calls, 1);
    zassert_false(startup_recovery);
    zassert_false(state.closing);
    zassert_false(paused);
    zassert_true(halves_held());
    zassert_true(defy_update_claim(DEFY_UPDATE_NEURON));
    defy_update_release(DEFY_UPDATE_NEURON);
}

ZTEST(updater_usb, test_startup_resume_failure_does_not_repeat_reset) {
    mark_halves_held(true);
    resume_error = -EBUSY;
    transport_repair = 200;
    run_transport(1000);
    zassert_equal(recovery_calls, 1);
    zassert_true(resume_calls > 1);
    recovered();
}

ZTEST(updater_usb, test_startup_waits_for_other_update_owner) {
    mark_halves_held(true);
    zassert_true(defy_update_claim(DEFY_UPDATE_NEURON));
    run_transport(300);
    zassert_equal(pause_calls + recovery_calls + reset_calls, 0);
    zassert_false(state.active);
    zassert_false(state.closing);
    defy_update_release(DEFY_UPDATE_NEURON);
}

ZTEST(updater_usb, test_transport_idle_thread_sleeps_without_timeout) {
    queue_hello(0);
    run_transport(60000);
    zassert_equal(output_length, HELLO_REPLY);
    hello_reply(0);
    zassert_equal(blocked_waits, 1);
    zassert_equal(forever_at, 0);
}

ZTEST(updater_usb, test_transport_disconnected_thread_sleeps_without_timeout) {
    transport_disconnect = 0;
    queue_hello(10);
    run_transport(60000);
    zassert_equal(output_length, 0);
    zassert_equal(blocked_waits, 2);
    zassert_equal(forever_at, 10);
}

ZTEST(updater_usb, test_transport_sleeps_once_host_closes_port_after_session) {
    queue_request(0, 2);
    queue_request(10, 7);
    transport_disconnect = 50;
    run_transport(60000);
    reply_status(0, 2, 0);
    reply_status(1, 7, 0);
    zassert_between_inclusive(forever_at, 50, 50 + WAIT_MS(LINE_POLL_MS));
    zassert_true(blocked_waits <= 2 + 50 / WAIT_MS(LINE_POLL_MS));
    recovered();
}

ZTEST(updater_usb, test_transport_session_expires_on_time) {
    queue_request(0, 2);
    run_transport(60000);
    reply_status(0, 2, 0);
    zassert_between_inclusive(released_at, DEFY_UPDATER_IDLE_MS,
                              DEFY_UPDATER_IDLE_MS + WAIT_MS(LINE_POLL_MS));
    zassert_equal(forever_at, released_at);
    zassert_equal(acquired_aborts, 1);
    recovered();
}

ZTEST(updater_usb, test_transport_partial_frame_dropped_on_time) {
    uint8_t partial[] = {'D', 'U', 1, 2, 0};
    queue_bytes(0, partial, sizeof(partial));
    run_transport(60000);
    zassert_equal(output_length, 0);
    zassert_between_inclusive(forever_at, FRAME_GAP_MS, FRAME_GAP_MS + WAIT_MS(LINE_POLL_MS));
}

ZTEST(updater_usb, test_transport_frame_bytes_within_gap_are_kept) {
    uint8_t frame[DEFY_UPDATER_FRAME];
    size_t length = build_request(frame, 2, NULL, 0);
    queue_bytes(0, frame, 10);
    queue_bytes(FRAME_GAP_MS - 1, frame + 10, length - 10);
    queue_request(FRAME_GAP_MS + 10, 7);
    run_transport(FRAME_GAP_MS + 20);
    reply_status(0, 2, 0);
    reply_status(1, 7, 0);
    recovered();
}

ZTEST(updater_usb, test_transport_failed_reply_leaves_nothing_queued) {
    stall_at = 10;
    stall_until = 1500;
    queue_request(0, 2);
    queue_hello(1500);
    run_transport(1600);
    zassert_equal(acquired_aborts, 1);
    zassert_between_inclusive(released_at, REPLY_TIMEOUT_MS, REPLY_TIMEOUT_MS + WAIT_MS(1));
    zassert_equal(output_length, 10 + HELLO_REPLY);
    hello_reply(10);
    zassert_false(tx_enabled);
    zassert_true(ring_buf_is_empty(&tx_ring));
    recovered();
}

ZTEST(updater_usb, test_transport_reply_done_once_handed_to_uart) {
    stall_at = 20;
    stall_until = REPLY_TIMEOUT_MS + 500;
    queue_request(0, 2);
    queue_request(REPLY_TIMEOUT_MS + 600, 7);
    run_transport(REPLY_TIMEOUT_MS + 610);
    reply_status(0, 2, 0);
    reply_status(1, 7, 0);
    zassert_equal(released_at, REPLY_TIMEOUT_MS + 600);
    recovered();
}

ZTEST(updater_usb, test_transport_disconnect_during_reply_fails_at_once) {
    fill_limit = 0;
    queue_request(0, 2);
    transport_disconnect = 20;
    run_transport(100);
    zassert_equal(output_length, 0);
    zassert_between_inclusive(released_at, 20, 20 + WAIT_MS(1));
    zassert_false(tx_enabled);
    zassert_true(ring_buf_is_empty(&tx_ring));
    recovered();
}

ZTEST(updater_usb, test_transport_full_rx_ring_pauses_input_without_loss) {
    size_t count = sizeof(rx_buffer) / 20 + 2;
    zassert_true(count * HELLO_REPLY <= sizeof(output));
    for (size_t i = 0; i < count; i++) {
        queue_hello(0);
    }
    run_transport(10);
    zassert_true(rx_pauses > 0);
    zassert_true(rx_enabled);
    zassert_equal(output_length, count * HELLO_REPLY);
    for (size_t i = 0; i < count; i++) {
        hello_reply(i * HELLO_REPLY);
    }
}

ZTEST(updater_usb, test_transport_input_left_in_the_port_at_a_usb_reset_is_dropped) {
    /* More HELLOs than the ring holds while the host reads no reply, so input pauses with
     * the rest in the port. The bus resets at 20 ms and the host comes back at 50 ms. */
    for (size_t i = 0; i < sizeof(rx_buffer) / 20 + 2; i++) {
        queue_hello(0);
    }
    fill_limit = 0;
    transport_disconnect = 20;
    transport_reconnect = 50;
    usb_reset_at = 20;
    run_transport(40);
    zassert_equal(output_length, 0);
    zassert_true(rx_enabled);

    /* The new host's HELLO gets one reply, with nothing of the old input before it. */
    fill_limit = INT_MAX;
    queue_hello(60);
    run_transport(100);
    zassert_equal(output_length, HELLO_REPLY);
    hello_reply(0);
}

ZTEST_SUITE(updater_usb, NULL, NULL, before, NULL, NULL);
