/* SPDX-License-Identifier: MIT */

#include <string.h>

#include <zephyr/drivers/gpio/gpio_emul.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/atomic.h>
#include <zephyr/sys/reboot.h>
#include <zephyr/ztest.h>

static void test_reboot(int type);
static void test_error_log(const char *format, ...);

#undef LOG_ERR
#define LOG_ERR(...) test_error_log(__VA_ARGS__)
#define sys_reboot test_reboot
#define main radio_application_main
#include "main.c"
#undef main
#undef LOG_ERR
#include "watchdog.c"
#undef sys_reboot
#include "battery_hci.c"

NRF_WDT_Type watchdog_registers;
unsigned int reload_count[NRF_WDT_CHANNEL_NUMBER];

static K_SEM_DEFINE(armed, 0, 1);
static K_SEM_DEFINE(powered_off, 0, 1);
static atomic_t cs_active;
static atomic_t off_attempts;
static atomic_t arm_count;
static atomic_t reset_count;
static atomic_t power_off_error;
static atomic_t battery_stop_count;
static atomic_t battery_refresh_count;
static int battery_refresh_result;
static struct spi_buf armed_tx;
static struct spi_buf armed_rx;
static spi_callback_t completion;
static void *completion_data;
static bool thread_started;

void defy_radio_battery_start(void) {}
void defy_radio_battery_stop(void) { atomic_inc(&battery_stop_count); }
int defy_radio_battery_refresh(void) {
    atomic_inc(&battery_refresh_count);
    return battery_refresh_result;
}

/* Whether message, with its H4 packet type, is Command Complete for opcode with status. */
static bool is_command_complete(const uint8_t *message, size_t len, uint16_t opcode,
                                uint8_t status) {
    struct defy_hci_spi_reply reply;

    return defy_hci_spi_parse_reply(message, len, &reply) &&
           reply.event == BT_HCI_EVT_CMD_COMPLETE && reply.opcode == opcode &&
           reply.status == status;
}

static void test_error_log(const char *format, ...) {
    zassert_unreachable("Unexpected error log: %s", format);
}

/* The watchdog's reboot. A real one does not return, so nothing feeds or reboots after it. */
static void test_reboot(int type) {
    zassert_equal(type, SYS_REBOOT_COLD);
    zassert_true(k_is_in_isr());
    atomic_inc(&reset_count);
    k_timer_stop(&watchdog_timer);
}

static int transceive_async(const struct device *dev, const struct spi_config *cfg,
                           const struct spi_buf_set *tx_set, const struct spi_buf_set *rx_set,
                           spi_callback_t callback, void *userdata) {
    ARG_UNUSED(dev);
    ARG_UNUSED(cfg);

    atomic_inc(&arm_count);
    armed_tx = tx_set->buffers[0];
    armed_rx = rx_set->buffers[0];
    completion = callback;
    completion_data = userdata;
    k_sem_give(&armed);
    return 0;
}

static DEVICE_API(spi, test_spi_api) = {
    .transceive_async = transceive_async,
};

DEVICE_DT_DEFINE(DT_NODELABEL(test_spi), NULL, NULL, NULL, NULL, POST_KERNEL,
                 CONFIG_SPI_INIT_PRIORITY, &test_spi_api);

int defy_radio_power_off(void) {
    zassert_equal(atomic_get(&battery_stop_count), 1);
    atomic_inc(&off_attempts);
    int err = atomic_get(&power_off_error);

    if (err != 0) {
        return err;
    }
    if (atomic_get(&cs_active)) {
        return -EBUSY;
    }
    k_sem_give(&powered_off);
    k_thread_abort(k_current_get());
    CODE_UNREACHABLE;
}

static void drain(void) {
    struct net_buf *buf;

    while ((buf = k_fifo_get(&host_queue, K_NO_WAIT)) != NULL) {
        net_buf_unref(buf);
    }
}

static void before(void *fixture) {
    ARG_UNUSED(fixture);

    k_timer_stop(&watchdog_timer);
    drain();
    power_off_requested = false;
    battery_pending = false;
    battery_stopped = false;
    memset(&battery_latest, 0, sizeof(battery_latest));
    battery_latest_at = battery_queued_at = 0;
    battery_queued_age = 0;
    power_off_deadline = 0;
    spi_result = 0;
    thread_started = false;
    atomic_clear(&cs_active);
    atomic_clear(&off_attempts);
    atomic_clear(&arm_count);
    atomic_clear(&reset_count);
    atomic_clear(&power_off_error);
    atomic_clear(&battery_stop_count);
    atomic_clear(&battery_refresh_count);
    battery_refresh_result = 0;
    k_sem_reset(&spi_done);
    k_sem_reset(&armed);
    k_sem_reset(&powered_off);
    zassert_ok(gpio_pin_configure_dt(&data_ready, GPIO_OUTPUT_INACTIVE));
    memset(&watchdog_registers, 0, sizeof(watchdog_registers));
    memset(reload_count, 0, sizeof(reload_count));
    watchdog_last_progress = 0;
    watchdog_timeout = 0;
    watchdog_expired = false;
    watchdog_registers.RUNSTATUS = 1;
    watchdog_registers.RREN = 1;
    watchdog_registers.CRV = 0x8000;
    zassert_ok(watchdog_timer_start());
}

static void after(void *fixture) {
    ARG_UNUSED(fixture);

    if (thread_started) {
        k_thread_abort(&link_thread_data);
    }
    k_timer_stop(&watchdog_timer);
    drain();
}

static unsigned int feeds(void) {
    k_spinlock_key_t key = k_spin_lock(&watchdog_lock);
    unsigned int count = reload_count[0];

    k_spin_unlock(&watchdog_lock, key);
    return count;
}

static bool expired(void) {
    k_spinlock_key_t key = k_spin_lock(&watchdog_lock);
    bool value = watchdog_expired;

    k_spin_unlock(&watchdog_lock, key);
    return value;
}

static void start_link(void) {
    thread_started = true;
    k_thread_create(&link_thread_data, link_stack, K_THREAD_STACK_SIZEOF(link_stack), link_thread,
                    NULL, NULL, NULL, LINK_THREAD_PRIORITY, 0, K_NO_WAIT);
}

static void transfer(const uint8_t *mosi, uint8_t *miso, size_t len) {
    zassert_ok(k_sem_take(&armed, K_MSEC(200)));
    zassert_true(len <= armed_rx.len);
    if (miso != NULL) {
        memset(miso, 0, len);
        memcpy(miso, armed_tx.buf, MIN(len, armed_tx.len));
    }
    memset(armed_rx.buf, 0, armed_rx.len);
    if (mosi != NULL) {
        memcpy(armed_rx.buf, mosi, len);
    }
    completion(spi_dev, len, completion_data);
}

static void send_power_off(void) {
    static const uint8_t header[DEFY_HCI_SPI_HEADER_LEN] = {DEFY_HCI_SPI_WRITE};
    static const uint8_t command[] = DEFY_HCI_SPI_COMMAND(DEFY_HCI_SPI_OP_POWER_OFF);

    transfer(header, NULL, sizeof(header));
    transfer(command, NULL, sizeof(command));
}

static size_t read_message(uint8_t *message) {
    static const uint8_t header[DEFY_HCI_SPI_HEADER_LEN] = {DEFY_HCI_SPI_READ};
    uint8_t response[DEFY_HCI_SPI_HEADER_LEN];

    transfer(header, response, sizeof(header));
    size_t len = response[DEFY_HCI_SPI_HEADER_TOREAD];

    zassert_true(len > 0);
    transfer(NULL, message, len);
    return len;
}

ZTEST(defy_radio_lifecycle, test_battery_refresh_ack_and_shutdown_rejection) {
    static const uint8_t header[DEFY_HCI_SPI_HEADER_LEN] = {DEFY_HCI_SPI_WRITE};
    static const uint8_t command[] = DEFY_HCI_SPI_COMMAND(DEFY_HCI_SPI_OP_BATTERY_REFRESH);
    uint8_t message[DEFY_HCI_SPI_MAX_MSG_LEN];
    start_link();
    for (int i = 0; i < 2; i++) {
        battery_refresh_result = i == 0 ? 0 : -ESHUTDOWN;
        transfer(header, NULL, sizeof(header));
        transfer(command, NULL, sizeof(command));
        zassert_equal(read_message(message), 7);
        zassert_true(is_command_complete(message, 7, DEFY_HCI_SPI_OP_BATTERY_REFRESH,
                                         i == 0 ? BT_HCI_ERR_SUCCESS : BT_HCI_ERR_CMD_DISALLOWED));
        zassert_equal(atomic_get(&battery_refresh_count), i + 1);
    }
}

ZTEST(defy_radio_lifecycle, test_watchdog_stays_fed_while_spi_is_idle) {
    start_link();
    zassert_ok(k_sem_take(&armed, K_MSEC(200)));
    unsigned int previous = feeds();

    k_sleep(K_MSEC(LINK_TIMEOUT_MS + 500));
    zassert_true(feeds() >= previous + 10);
    zassert_false(expired());
    zassert_equal(atomic_get(&reset_count), 0);
    zassert_equal(atomic_get(&arm_count), 1);
    zassert_equal(atomic_get(&off_attempts), 0);
}

ZTEST(defy_radio_lifecycle, test_watchdog_expires_while_link_is_suspended) {
    start_link();
    zassert_ok(k_sem_take(&armed, K_MSEC(200)));
    k_thread_suspend(&link_thread_data);
    k_sleep(K_MSEC(LINK_TIMEOUT_MS + 250));
    zassert_true(expired());
    zassert_equal(atomic_get(&reset_count), 1);
    unsigned int previous = feeds();

    k_sleep(K_MSEC(250));
    zassert_equal(feeds(), previous);
    k_thread_resume(&link_thread_data);
    k_sleep(K_MSEC(250));
    zassert_equal(feeds(), previous);
    zassert_true(expired());
    zassert_equal(atomic_get(&reset_count), 1);
    zassert_equal(atomic_get(&arm_count), 1);
    zassert_equal(atomic_get(&off_attempts), 0);
}

ZTEST(defy_radio_lifecycle, test_watchdog_expires_without_link_startup) {
    k_sleep(K_MSEC(STARTUP_GRACE_MS + 250));
    zassert_true(expired());
    zassert_equal(atomic_get(&reset_count), 1);
    unsigned int previous = feeds();

    start_link();
    zassert_ok(k_sem_take(&armed, K_MSEC(200)));
    k_sleep(K_MSEC(250));
    zassert_equal(feeds(), previous);
    zassert_true(expired());
    zassert_equal(atomic_get(&reset_count), 1);
    zassert_equal(atomic_get(&arm_count), 1);
    zassert_equal(atomic_get(&off_attempts), 0);
}

ZTEST(defy_radio_lifecycle, test_watchdog_supervises_without_hardware_watchdog) {
    k_timer_stop(&watchdog_timer);
    watchdog_registers.RUNSTATUS = 0;
    NRF_WDT_Type before = watchdog_registers;
    unsigned int previous = feeds();

    zassert_ok(watchdog_timer_start());
    start_link();
    zassert_ok(k_sem_take(&armed, K_MSEC(200)));
    k_sleep(K_MSEC(LINK_TIMEOUT_MS + 250));
    zassert_false(expired());
    zassert_equal(atomic_get(&reset_count), 0);
    k_thread_suspend(&link_thread_data);
    k_sleep(K_MSEC(LINK_TIMEOUT_MS + 250));
    zassert_true(expired());
    zassert_equal(atomic_get(&reset_count), 1);
    zassert_equal(feeds(), previous);
    zassert_mem_equal(&watchdog_registers, &before, sizeof(before));
}

ZTEST(defy_radio_lifecycle, test_shutdown_with_exhausted_controller_pool) {
    static const uint8_t ready[] = DEFY_HCI_SPI_READY_EVENT;
    static const uint8_t event[] = {BT_HCI_EVT_VENDOR, 1, 0x42};
    uint8_t message[DEFY_HCI_SPI_MAX_MSG_LEN];
    struct net_buf *buf;
    int queued = 0;

    queue_event(ready, sizeof(ready));
    while ((buf = bt_buf_get_rx(BT_BUF_EVT, K_NO_WAIT)) != NULL) {
        net_buf_add_mem(buf, event, sizeof(event));
        defy_radio_queue_for_host(buf, false);
        queued++;
        zassert_true(queued < 64);
    }
    zassert_true(queued > 0);
    start_link();
    send_power_off();
    size_t ready_len = read_message(message);

    zassert_true(defy_hci_spi_is_ready_event(message, ready_len));
    for (int i = 0; i < queued; i++) {
        zassert_equal(read_message(message), sizeof(event) + 1);
        zassert_mem_equal(&message[1], event, sizeof(event));
    }
    size_t len = read_message(message);

    zassert_true(
        is_command_complete(message, len, DEFY_HCI_SPI_OP_POWER_OFF, BT_HCI_ERR_SUCCESS));
    zassert_ok(k_sem_take(&powered_off, K_MSEC(200)));
    zassert_equal(atomic_get(&off_attempts), 1);
}

ZTEST(defy_radio_lifecycle, test_shutdown_after_ack) {
    uint8_t message[DEFY_HCI_SPI_MAX_MSG_LEN];

    start_link();
    send_power_off();
    size_t len = read_message(message);

    zassert_true(
        is_command_complete(message, len, DEFY_HCI_SPI_OP_POWER_OFF, BT_HCI_ERR_SUCCESS));
    zassert_ok(k_sem_take(&powered_off, K_MSEC(200)));
    zassert_true(k_fifo_is_empty(&host_queue));
    zassert_equal(atomic_get(&off_attempts), 1);
}

ZTEST(defy_radio_lifecycle, test_shutdown_when_host_never_reads_ack) {
    start_link();
    send_power_off();
    int64_t started = k_uptime_get();

    zassert_ok(k_sem_take(&powered_off, K_MSEC(DEFY_HCI_SPI_POWER_OFF_RADIO_TIMEOUT_MS + 200)));
    int64_t elapsed = k_uptime_get() - started;
    zassert_true(elapsed >= DEFY_HCI_SPI_POWER_OFF_RADIO_TIMEOUT_MS);
    zassert_true(elapsed <= DEFY_HCI_SPI_POWER_OFF_RADIO_TIMEOUT_MS + 2);
    zassert_false(k_fifo_is_empty(&host_queue));
    zassert_equal(gpio_emul_output_get(data_ready.port, data_ready.pin), 0);
    zassert_equal(atomic_get(&off_attempts), 1);
}

ZTEST(defy_radio_lifecycle, test_shutdown_ack_can_drain_near_fallback_deadline) {
    uint8_t message[DEFY_HCI_SPI_MAX_MSG_LEN];

    start_link();
    send_power_off();
    k_sleep(K_MSEC(DEFY_HCI_SPI_POWER_OFF_RADIO_TIMEOUT_MS - 5));
    zassert_equal(atomic_get(&off_attempts), 0);
    size_t len = read_message(message);
    zassert_true(
        is_command_complete(message, len, DEFY_HCI_SPI_OP_POWER_OFF, BT_HCI_ERR_SUCCESS));
    zassert_ok(k_sem_take(&powered_off, K_MSEC(2)));
    zassert_equal(atomic_get(&off_attempts), 1);
}

ZTEST(defy_radio_lifecycle, test_shutdown_waits_for_active_transfer) {
    start_link();
    send_power_off();
    atomic_set(&cs_active, 1);

    zassert_not_equal(k_sem_take(&powered_off, K_MSEC(DEFY_HCI_SPI_POWER_OFF_RADIO_TIMEOUT_MS + 30)), 0);
    zassert_true(atomic_get(&off_attempts) > 0);
    atomic_clear(&cs_active);
    zassert_ok(k_sem_take(&powered_off, K_MSEC(30)));
}

ZTEST(defy_radio_lifecycle, test_shutdown_retries_errors) {
    uint8_t message[DEFY_HCI_SPI_MAX_MSG_LEN];

    atomic_set(&power_off_error, -EIO);
    start_link();
    send_power_off();
    read_message(message);
    k_sleep(K_MSEC(100));

    zassert_true(atomic_get(&off_attempts) > 1);
    zassert_not_equal(k_sem_take(&powered_off, K_NO_WAIT), 0);

    atomic_clear(&power_off_error);
    zassert_ok(k_sem_take(&powered_off, K_MSEC(30)));
}

ZTEST(defy_radio_lifecycle, test_repeated_shutdown_does_not_extend_deadline) {
    request_power_off();
    int64_t deadline = power_off_deadline;

    k_sleep(K_MSEC(2));
    request_power_off();
    zassert_equal(power_off_deadline, deadline);
    struct net_buf *reply = k_fifo_get(&host_queue, K_NO_WAIT);

    zassert_not_null(reply);
    zassert_true(k_fifo_is_empty(&host_queue));
    net_buf_unref(reply);
}

ZTEST(defy_radio_lifecycle, test_controller_events_do_not_follow_shutdown_ack) {
    static const uint8_t event[] = {BT_HCI_EVT_VENDOR, 1, 0x42};
    struct net_buf *buf = bt_buf_get_rx(BT_BUF_EVT, K_NO_WAIT);

    zassert_not_null(buf);
    request_power_off();
    net_buf_add_mem(buf, event, sizeof(event));
    defy_radio_queue_for_host(buf, false);
    struct net_buf *reply = k_fifo_get(&host_queue, K_NO_WAIT);

    zassert_not_null(reply);
    zassert_true(is_command_complete(reply->data, reply->len, DEFY_HCI_SPI_OP_POWER_OFF,
                                     BT_HCI_ERR_SUCCESS));
    zassert_true(k_fifo_is_empty(&host_queue));
    net_buf_unref(reply);
}

static struct defy_battery_sample battery_sample(uint32_t sequence) {
    struct defy_battery_sample sample = {.sequence = sequence, .age_ms = 12};

    for (size_t i = 0; i < DEFY_BATTERY_WORD_COUNT; i++) {
        sample.words[i] = 0x2300 + i;
    }
    return sample;
}

ZTEST(defy_radio_lifecycle, test_battery_event_encoding) {
    struct defy_battery_sample sample = battery_sample(7);
    uint8_t message[DEFY_HCI_SPI_MAX_MSG_LEN];

    sample.status = DEFY_BATTERY_SAMPLE_IO_ERROR;
    zassert_ok(defy_radio_battery_publish(&sample));
    start_link();
    zassert_equal(read_message(message), DEFY_BATTERY_HCI_PAYLOAD_SIZE + 3);
    zassert_equal(message[0], BT_HCI_H4_EVT);
    zassert_equal(message[1], BT_HCI_EVT_VENDOR);
    zassert_equal(message[2], DEFY_BATTERY_HCI_PAYLOAD_SIZE);
    zassert_equal(sys_get_le16(&message[3]), DEFY_HCI_SPI_EVT_BATTERY);
    zassert_equal(message[5], DEFY_BATTERY_VERSION);
    zassert_equal(message[6], DEFY_BATTERY_SAMPLE_IO_ERROR);
    zassert_equal(sys_get_le32(&message[7]), 7);
    zassert_true(sys_get_le32(&message[11]) >= 12);
    for (size_t i = 0; i < DEFY_BATTERY_WORD_COUNT; i++) {
        zassert_equal(sys_get_le16(&message[15 + 2 * i]), sample.words[i]);
    }
}

ZTEST(defy_radio_lifecycle, test_battery_coalesces_without_mutating_queued_buffer) {
    struct defy_battery_sample sample = battery_sample(1);
    uint8_t saved[DEFY_BATTERY_HCI_PAYLOAD_SIZE + 3];

    zassert_ok(defy_radio_battery_publish(&sample));
    struct net_buf *first = k_fifo_peek_head(&host_queue);
    zassert_not_null(first);
    memcpy(saved, first->data, sizeof(saved));
    sample.sequence = 2;
    zassert_ok(defy_radio_battery_publish(&sample));
    sample.sequence = 3;
    sample.words[0] = 0x4321;
    zassert_ok(defy_radio_battery_publish(&sample));
    zassert_true(battery_pending);
    zassert_equal(k_fifo_peek_head(&host_queue), first);
    zassert_mem_equal(first->data, saved, sizeof(saved));
    zassert_is_null(net_buf_alloc(&battery_pool, K_NO_WAIT));
    sent();
    struct net_buf *latest = k_fifo_peek_head(&host_queue);
    zassert_not_null(latest);
    zassert_equal(sys_get_le32(&latest->data[7]), 3);
    zassert_equal(sys_get_le16(&latest->data[15]), 0x4321);
    zassert_false(battery_pending);
    sent();
    zassert_true(k_fifo_is_empty(&host_queue));
}

ZTEST(defy_radio_lifecycle, test_battery_armed_transfer_is_immutable) {
    static const uint8_t header[DEFY_HCI_SPI_HEADER_LEN] = {DEFY_HCI_SPI_READ};
    struct defy_battery_sample sample = battery_sample(1);
    uint8_t response[DEFY_HCI_SPI_HEADER_LEN];
    uint8_t message[DEFY_BATTERY_HCI_PAYLOAD_SIZE + 3];
    uint8_t saved[sizeof(message)];

    zassert_ok(defy_radio_battery_publish(&sample));
    start_link();
    transfer(header, response, sizeof(header));
    zassert_ok(k_sem_take(&armed, K_MSEC(200)));
    zassert_equal(armed_tx.len, sizeof(message));
    memcpy(saved, armed_tx.buf, sizeof(saved));
    sample.sequence = 2;
    sample.words[0] = 0x9876;
    zassert_ok(defy_radio_battery_publish(&sample));
    zassert_mem_equal(armed_tx.buf, saved, sizeof(saved));
    k_sem_give(&armed);
    transfer(NULL, message, sizeof(message));
    zassert_equal(sys_get_le32(&message[7]), 1);
    zassert_equal(read_message(message), sizeof(message));
    zassert_equal(sys_get_le32(&message[7]), 2);
    zassert_equal(sys_get_le16(&message[15]), 0x9876);
}

ZTEST(defy_radio_lifecycle, test_battery_queued_age_is_updated_only_in_spi_copy) {
    struct defy_battery_sample sample = battery_sample(1);
    uint8_t message[DEFY_HCI_SPI_MAX_MSG_LEN];

    zassert_ok(defy_radio_battery_publish(&sample));
    struct net_buf *queued = k_fifo_peek_head(&host_queue);
    uint32_t queued_age = sys_get_le32(&queued->data[11]);
    k_sleep(K_MSEC(20));
    zassert_equal(sys_get_le32(&queued->data[11]), queued_age);
    start_link();
    zassert_equal(read_message(message), DEFY_BATTERY_HCI_PAYLOAD_SIZE + 3);
    zassert_true(sys_get_le32(&message[11]) >= queued_age + 20);
}

ZTEST(defy_radio_lifecycle, test_battery_cannot_consume_shutdown_control_slot) {
    static const uint8_t ready[] = DEFY_HCI_SPI_READY_EVENT;
    struct defy_battery_sample sample = battery_sample(1);
    uint8_t message[DEFY_HCI_SPI_MAX_MSG_LEN];

    queue_event(ready, sizeof(ready));
    zassert_ok(defy_radio_battery_publish(&sample));
    sample.sequence = 2;
    zassert_ok(defy_radio_battery_publish(&sample));
    start_link();
    send_power_off();
    zassert_ok(k_sem_take(&armed, K_MSEC(200)));
    k_sem_give(&armed);
    zassert_false(battery_pending);
    zassert_equal(defy_radio_battery_publish(&sample), -ESHUTDOWN);
    size_t len = read_message(message);
    zassert_true(defy_hci_spi_is_ready_event(message, len));
    zassert_equal(read_message(message), DEFY_BATTERY_HCI_PAYLOAD_SIZE + 3);
    zassert_equal(sys_get_le32(&message[7]), 1);
    len = read_message(message);
    zassert_true(
        is_command_complete(message, len, DEFY_HCI_SPI_OP_POWER_OFF, BT_HCI_ERR_SUCCESS));
    zassert_ok(k_sem_take(&powered_off, K_MSEC(200)));
    zassert_true(k_fifo_is_empty(&host_queue));
}

ZTEST(defy_radio_lifecycle, test_battery_rejects_invalid_publication) {
    struct defy_battery_sample sample = battery_sample(0);

    zassert_equal(defy_radio_battery_publish(&sample), -EINVAL);
    sample.sequence = 1;
    sample.status = DEFY_BATTERY_SAMPLE_ANALOG_ERROR + 1;
    zassert_equal(defy_radio_battery_publish(&sample), -EINVAL);
    zassert_true(k_fifo_is_empty(&host_queue));
}

ZTEST_SUITE(defy_radio_lifecycle, NULL, NULL, before, after, NULL);
