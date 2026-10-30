/*
 * Copyright (c) 2017 Linaro Ltd.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/*
 * Bluetooth HCI over SPI to the nRF52820 on a Defy half's RF board, which
 * runs apps/defy_radio. Based on Zephyr's drivers/bluetooth/hci/spi.c, whose
 * framing it keeps (include/dygma/defy_hci_spi.h). What changes:
 *
 * - No reset line, which the Defy does not have. Zephyr's driver resets the
 *   controller and then waits for ever for its ready event, so it hangs after
 *   any restart of the RP2040 alone. Here every start brings the controller to
 *   a fresh boot: a wake pulse on chip select for a controller in System OFF,
 *   reading out what is left from before, and a vendor reboot command for one
 *   that was already running. Its ready event ends the start.
 * - Every wait has a limit. A controller that does not answer fails
 *   bt_enable(), and ZMK starts without Bluetooth.
 * - Data ready is a level: the driver reads while the line is high, not only
 *   after a rising edge.
 * - Suspend sends the vendor power-off command, so the controller sleeps with
 *   the RP2040. A resume after that reboots the RP2040.
 * - A ready event while running means the controller restarted on its own,
 *   losing every connection the host knows of. The RP2040 reboots, or, within
 *   a minute of its start, stops Bluetooth.
 * - Every boot of the nRF52820 passes through Dygma's rf_bootloader, which
 *   listens for about 300 ms. With CONFIG_DEFY_RADIO_INSTALL the driver looks
 *   for it once per boot of the RP2040, before the rest of ZMK starts or, if
 *   the radio is silent then, when it first answers. It installs the radio
 *   firmware embedded in this build if the radio holds anything else, as
 *   Dygma's keyscanner does. Nothing resets Dygma's radio application into
 *   the bootloader over SPI, so a radio found running it fails the start.
 *
 * tests/drivers/defy_hci_spi runs it against apps/defy_radio's link code.
 */

#define DT_DRV_COMPAT dygma_defy_bt_hci_spi

#include <string.h>

#include <zephyr/bluetooth/buf.h>
#include <zephyr/bluetooth/hci.h>
#include <zephyr/drivers/bluetooth.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/drivers/spi.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/pm/device.h>
#include <zephyr/sys/atomic.h>
#include <zephyr/sys/byteorder.h>
#include <zephyr/sys/reboot.h>

#include <dygma/defy_battery_hci.h>
#include <dygma/defy_hci_diagnostics.h>
#include <dygma/defy_hci_spi.h>
#include <dygma/defy_radio_factory.h>
#include <dygma/defy_radio_image.h>
#include <dygma/defy_startup.h>

LOG_MODULE_REGISTER(defy_hci_spi, CONFIG_BT_HCI_DRIVER_LOG_LEVEL);

BUILD_ASSERT(DT_NUM_INST_STATUS_OKAY(DT_DRV_COMPAT) == 1, "One controller only");

/* How long the controller has to answer a header, or to arm a message or packet after one. */
#define ANSWER_TIMEOUT_MS 20

/*
 * How long a start attempt waits for the ready event. The nRF52820 has to
 * boot and start its 32.768 kHz crystal, which can take some hundred
 * milliseconds.
 */
#define BOOT_TIMEOUT_MS 2000
#define START_ATTEMPTS 3
#define START_POLL_MS 5

/*
 * Chip select setup and hold time around every transfer. Without it the
 * status poll does not identify rf_bootloader and the controller does not
 * start.
 */
#define CS_DELAY_US 10

/* Chip select stays low for twice this, which wakes the controller from System OFF. */
#define WAKE_PULSE_US 100

/* How long open(), the PM actions and a battery refresh wait for a lock. */
#define LOCK_TIMEOUT_MS 100

/*
 * A controller that restarts within this long of boot stops Bluetooth instead
 * of rebooting the RP2040.
 */
#define RESTART_REBOOT_MIN_UPTIME_MS 60000
/*
 * Data ready held high this long without a readable message counts as a
 * controller restart. Each failed read counts its own time, at most two answer
 * timeouts.
 */
#define RX_FAILURE_TIMEOUT_MS 1000
#define RX_FAILURE_STEP_MAX_MS (2 * ANSWER_TIMEOUT_MS + 1)

/* Per rf_bootloader operation. An erase takes up to some 90 ms. */
#define FACTORY_TIMEOUT_MS 1000
/* Sessions per look: a first session can fail to begin with -EPROTO. */
#define FACTORY_SESSIONS 3

/*
 * How long the look at power-on waits for the radio to answer, and how long
 * it keeps looking after rebooting this port's controller: rf_bootloader
 * listens for some 300 ms after every reset.
 */
#define POWER_ON_LOOK_MS 150
#define BOOTLOADER_WINDOW_MS 350

#define DATA_DELAY_US DT_INST_PROP(0, controller_data_delay_us)

enum link_state {
    LINK_DOWN,
    LINK_STARTING,
    LINK_UP,
    LINK_POWERING_OFF,
    LINK_POWERED_OFF,
    LINK_FAILED,
    LINK_POWER_OFF_FAILED,
};

struct defy_hci_spi_data {
    /* Set as the RX thread starts. */
    bt_hci_recv_t recv;
    atomic_t state;
    /* Settles a power-off ack against power_off() giving up on it. */
    struct k_spinlock power_off_lock;
    atomic_t power_off_result;
    int64_t power_off_deadline;
};

static struct defy_hci_spi_data defy_hci_spi_data;

static uint8_t __noinit rxmsg[DEFY_HCI_SPI_MAX_MSG_LEN];
/* Sent while reading a message. */
static uint8_t __noinit filler[DEFY_HCI_SPI_MAX_MSG_LEN];

static const struct gpio_dt_spec irq_gpio = GPIO_DT_SPEC_INST_GET(0, irq_gpios);

static const struct spi_dt_spec bus =
    SPI_DT_SPEC_INST_GET(0, SPI_OP_MODE_MASTER | SPI_TRANSFER_MSB | SPI_WORD_SET(8), CS_DELAY_US);
static struct spi_config wake_config;

static struct gpio_callback gpio_cb;

static K_SEM_DEFINE(sem_request, 0, 1);
static K_SEM_DEFINE(sem_busy, 1, 1);
static K_SEM_DEFINE(sem_power_off, 0, 1);
/* Serialises open() and the PM actions. */
static K_MUTEX_DEFINE(lifecycle_mutex);

static K_KERNEL_STACK_DEFINE(rx_stack, CONFIG_BT_DRV_RX_STACK_SIZE);
static struct k_thread rx_thread_data;
static atomic_t diagnostic_tx;
static atomic_t diagnostic_rx;

static void record_command(uint16_t opcode) {
    if (opcode != BT_HCI_OP_HOST_NUM_COMPLETED_PACKETS) {
        atomic_set(&diagnostic_tx, opcode);
    }
}

static int transceive(const struct spi_config *config, const void *tx, size_t tx_len, void *rx,
                      size_t rx_len) {
    const struct spi_buf tx_buf = {.buf = (void *)tx, .len = tx_len};
    const struct spi_buf rx_buf = {.buf = rx, .len = rx_len};
    const struct spi_buf_set tx_set = {.buffers = &tx_buf, .count = 1};
    const struct spi_buf_set rx_set = {.buffers = &rx_buf, .count = 1};

    return spi_transceive(bus.bus, config, &tx_set, rx != NULL ? &rx_set : NULL);
}

/* Gives the controller time to arm its next reply, without holding the CPU. */
static void retry_wait(void) { k_sleep(K_TICKS(1)); }

/*
 * A header exchange, repeated until the controller answers or the time is up.
 * For a read it is also repeated while the controller has no message: a slave
 * arms its reply before the transfer, so the first answer after data ready
 * rises may predate the message.
 *
 * Returns the length of the controller's oldest unsent message, 0 for a write
 * or if it has none, or -ETIMEDOUT if it does not answer.
 */
static int exchange_header(uint8_t op, int64_t limit) {
    const uint8_t header[DEFY_HCI_SPI_HEADER_LEN] = {op};
    uint8_t answer[DEFY_HCI_SPI_HEADER_LEN];
    int64_t deadline = MIN(k_uptime_get() + ANSWER_TIMEOUT_MS, limit);
    bool answered = false;

    while (k_uptime_get() < deadline) {
        int err = transceive(&bus.config, header, sizeof(header), answer, sizeof(answer));

        if (err < 0) {
            return err;
        }

        if (answer[0] == DEFY_HCI_SPI_READY_NOW && answer[1] == DEFY_HCI_SPI_SANITY_CHECK) {
            if (op == DEFY_HCI_SPI_WRITE) {
                return 0;
            }
            if (answer[DEFY_HCI_SPI_HEADER_TOREAD] > 0) {
                return answer[DEFY_HCI_SPI_HEADER_TOREAD];
            }
            answered = true;
        }

        retry_wait();
    }

    return answered ? 0 : -ETIMEDOUT;
}

/* Reads one message into rxmsg. Returns its length, 0 if there is none, or a negative error. */
static int read_message(int64_t limit) {
    int len = exchange_header(DEFY_HCI_SPI_READ, limit);
    int64_t deadline;

    if (len <= 0) {
        return len;
    }

    k_busy_wait(DATA_DELAY_US);
    deadline = MIN(k_uptime_get() + ANSWER_TIMEOUT_MS, limit);

    while (k_uptime_get() < deadline) {
        int err = transceive(&bus.config, filler, len, rxmsg, len);

        if (err < 0) {
            return err;
        }

        /* An unarmed nRF52 SPIS sends its default character, 0. */
        if (rxmsg[0] != 0) {
            return len;
        }

        retry_wait();
    }

    return -ETIMEDOUT;
}

/* Writes one packet, with its H4 packet type. */
static int write_packet(const uint8_t *packet, size_t len, int64_t limit) {
    int64_t deadline;
    uint8_t first;
    int err;

    err = exchange_header(DEFY_HCI_SPI_WRITE, limit);
    if (err < 0) {
        return err;
    }

    k_busy_wait(DATA_DELAY_US);
    deadline = MIN(k_uptime_get() + ANSWER_TIMEOUT_MS, limit);

    while (k_uptime_get() < deadline) {
        err = transceive(&bus.config, packet, len, &first, 1);
        if (err < 0) {
            return err;
        }

        /* An armed controller sends a non-zero byte first. */
        if (first != 0) {
            return 0;
        }

        retry_wait();
    }

    return -ETIMEDOUT;
}

/* A one-byte transfer with chip select held low long enough to wake a controller in System OFF. */
static void wake_pulse(void) {
    static const uint8_t zero;

    (void)transceive(&wake_config, &zero, 1, NULL, 0);
}

static bool data_ready(void) { return gpio_pin_get_dt(&irq_gpio) > 0; }

static void data_ready_isr(const struct device *port, struct gpio_callback *cb, uint32_t pins) {
    ARG_UNUSED(port);
    ARG_UNUSED(cb);
    ARG_UNUSED(pins);

    k_sem_give(&sem_request);
}

static int rx_packet_size(const uint8_t *msg, int len) {
    int size;

    if (len >= 3 && msg[0] == BT_HCI_H4_EVT) {
        size = sizeof(struct bt_hci_evt_hdr) + msg[2];
    } else if (len >= 5 && msg[0] == BT_HCI_H4_ACL) {
        size = sizeof(struct bt_hci_acl_hdr) + sys_get_le16(&msg[3]);
    } else {
        return -EINVAL;
    }
    return len >= size + 1 ? size : -EMSGSIZE;
}

static struct net_buf *rx_buf_construct(const uint8_t *msg, size_t size) {
    struct net_buf *buf;

    if (msg[0] == BT_HCI_H4_EVT) {
        buf = bt_buf_get_evt(msg[1], false, K_FOREVER);
    } else {
        buf = bt_buf_get_rx(BT_BUF_ACL_IN, K_FOREVER);
    }

    if (size > net_buf_tailroom(buf)) {
        LOG_ERR("Packet of %zu bytes is too long", size);
        net_buf_unref(buf);
        return NULL;
    }

    net_buf_add_mem(buf, &msg[1], size);
    return buf;
}

/*
 * The controller restarted on its own, and every connection the host knows
 * of went with it. Rebooting the RP2040 starts both over. Not within a minute
 * of starting, though: a controller that keeps restarting would keep the
 * keyboard rebooting, USB included, so Bluetooth stops instead.
 */
static void controller_restarted(struct defy_hci_spi_data *data) {
    if (!atomic_cas(&data->state, LINK_UP, LINK_FAILED)) {
        return;
    }
    if (IS_ENABLED(CONFIG_DEFY_BATTERY_HCI)) {
        defy_battery_hci_reset();
    }
    if (k_uptime_get() >= RESTART_REBOOT_MIN_UPTIME_MS) {
        LOG_ERR("Bluetooth controller unavailable, rebooting");
        sys_reboot(SYS_REBOOT_COLD);
    }

    LOG_ERR("Bluetooth controller unavailable soon after start, stopping Bluetooth");
}

/*
 * Passes a message to the host. While powering off, it settles the power-off
 * ack and drops anything else. Returns false for a message the link cannot
 * take.
 */
static bool handle_message(const struct device *dev, const uint8_t *msg, int len) {
    struct defy_hci_spi_data *data = dev->data;
    struct defy_hci_spi_reply reply;
    struct net_buf *buf;
    k_spinlock_key_t key = k_spin_lock(&data->power_off_lock);
    atomic_val_t state = atomic_get(&data->state);

    if (state == LINK_POWERING_OFF) {
        bool acknowledged = defy_hci_spi_parse_reply(msg, len, &reply) &&
                            reply.event == BT_HCI_EVT_CMD_COMPLETE &&
                            reply.opcode == DEFY_HCI_SPI_OP_POWER_OFF;

        if (acknowledged) {
            bool success = reply.status == BT_HCI_ERR_SUCCESS;

            atomic_set(&data->power_off_result, success ? 0 : -EIO);
            atomic_set(&data->state, success ? LINK_POWERED_OFF : LINK_POWER_OFF_FAILED);
        }
        k_spin_unlock(&data->power_off_lock, key);
        if (acknowledged) {
            k_sem_give(&sem_power_off);
        }
        return true;
    }
    k_spin_unlock(&data->power_off_lock, key);

    if (state != LINK_UP) {
        return false;
    }

    int size = rx_packet_size(msg, len);

    if (size < 0) {
        return false;
    }

    if (defy_hci_spi_is_ready_event(msg, len)) {
        controller_restarted(data);
        return true;
    }

    if (defy_hci_spi_parse_reply(msg, len, &reply)) {
        atomic_set(&diagnostic_rx, reply.opcode);
        /*
         * Drop the reply to defy_hci_spi_battery_refresh(): the host did not
         * send it. A radio without the command answers with Command Status.
         */
        if (reply.opcode == DEFY_HCI_SPI_OP_BATTERY_REFRESH) {
            return true;
        }
    }

    buf = rx_buf_construct(msg, size);
    if (buf == NULL) {
        return true;
    }
    /* Suspend may have begun during the wait for a buffer. */
    if (atomic_get(&data->state) != LINK_UP) {
        net_buf_unref(buf);
        return true;
    }
    data->recv(dev, buf);
    return true;
}

static bool reading(struct defy_hci_spi_data *data) {
    atomic_val_t state = atomic_get(&data->state);

    return state == LINK_UP || state == LINK_POWERING_OFF;
}

static void rx_thread(void *p1, void *p2, void *p3) {
    const struct device *dev = p1;
    struct defy_hci_spi_data *data = dev->data;

    ARG_UNUSED(p2);
    ARG_UNUSED(p3);

    while (true) {
        k_sem_take(&sem_request, K_FOREVER);
        uint32_t failure_ms = 0;

        while (reading(data) && data_ready()) {
            k_sem_take(&sem_busy, K_FOREVER);
            if (!reading(data)) {
                k_sem_give(&sem_busy);
                break;
            }
            int64_t started = k_uptime_get();
            int64_t limit = atomic_get(&data->state) == LINK_POWERING_OFF
                                ? data->power_off_deadline
                                : INT64_MAX;
            int len = read_message(limit);
            uint32_t elapsed = MIN(k_uptime_get() - started + 1, RX_FAILURE_STEP_MAX_MS);
            /*
             * A power-off ack is settled before the bus is given back, so that
             * power_off() cannot time out between reading and handling it.
             */
            bool settled = len > 0 && atomic_get(&data->state) == LINK_POWERING_OFF &&
                           handle_message(dev, rxmsg, len);
            k_sem_give(&sem_busy);

            /* rxmsg needs no lock here: once the link is up, only this thread reads into it. */
            if (settled || (len > 0 && handle_message(dev, rxmsg, len))) {
                failure_ms = 0;
            } else {
                if (atomic_get(&data->state) == LINK_UP && data_ready()) {
                    failure_ms += elapsed;
                    if (failure_ms >= RX_FAILURE_TIMEOUT_MS) {
                        controller_restarted(data);
                        failure_ms = 0;
                    }
                } else {
                    failure_ms = 0;
                }
                k_sleep(K_MSEC(1));
            }
        }
    }
}

/*
 * A status poll of Dygma's protocol: two zero bytes. rf_bootloader and the
 * stock application answer 2 and a state from 0x80 to 0x85. This controller,
 * between transfers, answers with the start of its header, READY_NOW and
 * SANITY_CHECK, and otherwise ignores so short a transfer.
 */
enum radio_answer {
    RADIO_SILENT,
    RADIO_CONTROLLER,
    RADIO_DYGMA,
};

static enum radio_answer classify(const uint8_t answer[2]) {
    if (answer[0] == DEFY_HCI_SPI_READY_NOW && answer[1] == DEFY_HCI_SPI_SANITY_CHECK) {
        return RADIO_CONTROLLER;
    }
    if (defy_radio_factory_is_status(answer)) {
        return RADIO_DYGMA;
    }
    return RADIO_SILENT;
}

static enum radio_answer probe(void) {
    static const uint8_t poll[2];
    static uint8_t previous[2] = {0x5a, 0x5a};
    uint8_t answer[2];

    if (transceive(&bus.config, poll, sizeof(poll), answer, sizeof(answer)) < 0) {
        return RADIO_SILENT;
    }
    /* Log only changes, so that the start's log shows each answer once. */
    if (memcmp(answer, previous, sizeof(answer)) != 0) {
        LOG_INF("Radio answers %02x %02x", answer[0], answer[1]);
        defy_startup_radio_answer(answer[0], answer[1]);
        memcpy(previous, answer, sizeof(previous));
    }
    return classify(answer);
}

#ifdef CONFIG_DEFY_RADIO_INSTALL
/* rf_bootloader's protocol holds chip select across a header and its payload. */
static struct spi_config factory_config;

static int factory_exchange(void *context, const uint8_t *tx, uint8_t *rx, size_t length) {
    ARG_UNUSED(context);
    return transceive(&factory_config, tx, length, rx, length);
}

static int factory_release(void *context) {
    ARG_UNUSED(context);
    return spi_release(bus.bus, &factory_config);
}

static uint64_t factory_now(void *context) {
    ARG_UNUSED(context);
    return k_uptime_get();
}

static void factory_wait(void *context, uint32_t milliseconds) {
    ARG_UNUSED(context);
    k_sleep(K_MSEC(milliseconds));
}

static void factory_progress(void *context, size_t written, size_t size) {
    ARG_UNUSED(context);
    defy_startup_progress(written, size);
}

static const struct defy_radio_factory_bus factory_bus = {
    .exchange = factory_exchange,
    .release = factory_release,
    .now_ms = factory_now,
    .wait_ms = factory_wait,
    .progress = factory_progress,
};

/* factory_session() ignores init()'s result: with factory_bus, only a bad timeout fails it. */
BUILD_ASSERT(FACTORY_TIMEOUT_MS >= DEFY_RADIO_FACTORY_MIN_TIMEOUT_MS &&
             FACTORY_TIMEOUT_MS <= DEFY_RADIO_FACTORY_MAX_TIMEOUT_MS);

/*
 * The look's result: 0 once the radio runs this build's firmware, a negative
 * error otherwise, FACTORY_NOT_YET until a look has met the radio. It stands
 * until the half restarts, so a failed install is neither retried nor allowed
 * to erase the radio again.
 */
#define FACTORY_NOT_YET 1
static int factory_result = FACTORY_NOT_YET;

/*
 * One rf_bootloader session: installs this build's radio firmware if the
 * radio holds anything else, and starts the firmware either way. Dygma's
 * radio application answers the status poll but not B, so its session fails
 * to begin. *begun is set once B and INFO succeed; nothing is written before
 * that. Returns 0 once the bootloader has started this build's firmware.
 */
static int factory_session(bool *begun) {
    struct defy_radio_factory factory;
    bool installed = false;
    int err;

    (void)defy_radio_factory_init(&factory, &factory_bus, FACTORY_TIMEOUT_MS);
    /* After probe()'s poll, the 1 ms margin the factory keeps between transfers. */
    k_sleep(K_MSEC(1));
    err = defy_radio_factory_begin(&factory, NULL);
    defy_startup_radio_session(err);
    *begun = err == 0;
    if (err == -ENOTSUP) {
        LOG_ERR("The radio's bootloader is not Dygma's");
        return err;
    }
    if (err != 0) {
        LOG_ERR("No radio bootloader session (%d); Dygma's radio application may be running",
                err);
        return err;
    }

    defy_startup_report(DEFY_STARTUP_RADIO_BOOTLOADER);
    LOG_INF("Radio bootloader holds version 0x%08x", factory.info.program_version);
    err = defy_radio_factory_install(&factory, defy_radio_image, defy_radio_image_size,
                                     defy_radio_image_seal, &installed);
    defy_startup_radio_install(&factory, err, installed);
    if (err == -ENOTSUP) {
        LOG_ERR("The radio lacks Dygma's S113, so its firmware is left alone");
        (void)defy_radio_factory_leave(&factory);
    } else if (err != 0) {
        LOG_ERR("Radio firmware %s failed (%d)", installed ? "install" : "check", err);
    } else if (installed) {
        LOG_INF("Radio firmware 0x%08x installed over 0x%08x", defy_radio_image_version,
                factory.info.program_version);
    }
    return err;
}

/*
 * The look: a session with rf_bootloader as soon as it answers, after
 * rebooting this port's controller into it if the controller answers instead.
 * factory_result stays unset only while the radio is silent. A session that
 * fails to begin has written nothing, so the look tries again, up to
 * FACTORY_SESSIONS within its deadline, unless the bootloader is not Dygma's.
 * A begun session's result stands.
 */
static void catch_bootloader_locked(void) {
    static const uint8_t reboot[] = DEFY_HCI_SPI_COMMAND(DEFY_HCI_SPI_OP_REBOOT);
    int64_t deadline = k_uptime_get() + POWER_ON_LOOK_MS;
    bool reboot_sent = false;
    int sessions = 0;
    int failure = 0;

    if (factory_result != FACTORY_NOT_YET) {
        return;
    }

    while (k_uptime_get() < deadline) {
        enum radio_answer answer = probe();

        if (answer == RADIO_DYGMA) {
            bool begun;
            int err = factory_session(&begun);

            sessions++;
            if (begun || err == -ENOTSUP || sessions == FACTORY_SESSIONS) {
                factory_result = err;
                return;
            }
            failure = err;
            /*
             * A begin can fail long after the look's window, on a timeout or
             * as the bootloader hands over to the controller. Give the radio
             * a fresh window to answer again, as after a reboot.
             */
            deadline = MAX(deadline, k_uptime_get() + BOOTLOADER_WINDOW_MS);
            continue;
        }
        if (answer == RADIO_CONTROLLER && !reboot_sent) {
            int err = write_packet(reboot, sizeof(reboot), INT64_MAX);

            if (err != 0) {
                factory_result = err;
                return;
            }
            reboot_sent = true;
            deadline = k_uptime_get() + BOOTLOADER_WINDOW_MS;
        }
        k_sleep(K_MSEC(1));
    }
    /* The last session's failure to begin, or the bootloader never came after the reboot. */
    if (sessions > 0) {
        factory_result = failure;
    } else if (reboot_sent) {
        factory_result = -ETIMEDOUT;
    }
}

void defy_hci_spi_catch_bootloader(void) {
    k_sem_take(&sem_busy, K_FOREVER);
    catch_bootloader_locked();
    k_sem_give(&sem_busy);
}

static int catch_bootloader_at_start(void) {
    if (device_is_ready(DEVICE_DT_INST_GET(0))) {
        defy_hci_spi_catch_bootloader();
    }
    return 0;
}

/*
 * At power-on both chips start together. rf_bootloader listens for about
 * 300 ms, and the RP2040's own bootloader uses about 200 of them, so the look
 * runs before the rest of ZMK, some of which, such as settings erasing flash,
 * takes longer. It is the only way to install over Dygma's radio application,
 * and the only way back for a radio whose install was cut off: S113 then
 * sleeps, and only power-on brings the bootloader back.
 *
 * APPLICATION 1: after defy_startup_init, APPLICATION 0 in
 * lighting/defy_startup_leds.c, which drives the LEDs that show the look.
 */
SYS_INIT(catch_bootloader_at_start, APPLICATION, 1);
#endif

/* One attempt to bring the controller to a fresh boot and read its ready event. */
static int start_attempt(void) {
    static const uint8_t reboot[] = DEFY_HCI_SPI_COMMAND(DEFY_HCI_SPI_OP_REBOOT);
    int64_t deadline = k_uptime_get() + BOOT_TIMEOUT_MS;
    bool rebooting = false;

#ifdef CONFIG_DEFY_RADIO_INSTALL
    if (factory_result != FACTORY_NOT_YET && factory_result != 0) {
        return factory_result;
    }
#endif

    while (k_uptime_get() < deadline) {
        enum radio_answer answer = probe();

#ifdef CONFIG_DEFY_RADIO_INSTALL
        /* The first answer since the driver started: the look comes first. */
        if (answer != RADIO_SILENT && factory_result == FACTORY_NOT_YET) {
            catch_bootloader_locked();
            if (factory_result != 0) {
                return factory_result == FACTORY_NOT_YET ? -EIO : factory_result;
            }
            /* An install takes far longer than a boot: time the boot from here. */
            deadline = k_uptime_get() + BOOT_TIMEOUT_MS;
            rebooting = true;
            continue;
        }
#endif

        if (answer == RADIO_CONTROLLER) {
            int len = read_message(INT64_MAX);

            if (len > 0) {
                if (defy_hci_spi_is_ready_event(rxmsg, len)) {
                    if (IS_ENABLED(CONFIG_DEFY_BATTERY_HCI)) {
                        defy_battery_hci_reset();
                    }
                    return 0;
                }
                LOG_DBG("Dropped a message from before the start");
                continue;
            }

            if (len == 0 && !rebooting) {
                /* Running, with nothing left from before. */
                rebooting = write_packet(reboot, sizeof(reboot), INT64_MAX) == 0;
                continue;
            }
        }

        if (answer == RADIO_SILENT) {
            /* Asleep, booting or not there. If asleep, this wakes it. */
            wake_pulse();
        }

        k_sleep(K_MSEC(START_POLL_MS));
    }

    return -ETIMEDOUT;
}

static int start_link(const struct device *dev, bt_hci_recv_t recv) {
    struct defy_hci_spi_data *data = dev->data;
    int err;

    err = gpio_pin_configure_dt(&irq_gpio, GPIO_INPUT);
    if (err < 0) {
        return err;
    }

    err = gpio_add_callback(irq_gpio.port, &gpio_cb);
    if (err < 0) {
        return err;
    }

    err = gpio_pin_interrupt_configure_dt(&irq_gpio, GPIO_INT_EDGE_TO_ACTIVE);
    if (err < 0) {
        return err;
    }

    k_sem_take(&sem_busy, K_FOREVER);
    for (int attempt = 1; attempt <= START_ATTEMPTS; attempt++) {
        err = start_attempt();
        if (err == 0) {
            break;
        }
        LOG_WRN("No ready event from the Bluetooth controller (attempt %d)", attempt);
    }
    k_sem_give(&sem_busy);

    if (err < 0) {
        LOG_ERR("Bluetooth controller not answering");
        defy_startup_report(DEFY_STARTUP_RADIO_FAILED);
        return -EIO;
    }

    data->recv = recv;
    atomic_set(&data->state, LINK_UP);
    defy_startup_report(DEFY_STARTUP_RADIO_READY);

    k_thread_create(&rx_thread_data, rx_stack, K_KERNEL_STACK_SIZEOF(rx_stack), rx_thread,
                    (void *)dev, NULL, NULL, K_PRIO_COOP(CONFIG_BT_DRIVER_RX_HIGH_PRIO), 0,
                    K_NO_WAIT);
    k_thread_name_set(&rx_thread_data, "defy_hci_spi_rx");

    /* Data ready may already be high, with no edge to come. */
    k_sem_give(&sem_request);

    return 0;
}

/* The host opens the link once, as bt_enable() does, and never closes it. */
static int defy_hci_spi_open(const struct device *dev, bt_hci_recv_t recv) {
    struct defy_hci_spi_data *data = dev->data;
    int err;

    if (k_mutex_lock(&lifecycle_mutex, K_MSEC(LOCK_TIMEOUT_MS)) < 0) {
        return -EBUSY;
    }
    if (!atomic_cas(&data->state, LINK_DOWN, LINK_STARTING)) {
        k_mutex_unlock(&lifecycle_mutex);
        return -EALREADY;
    }
    err = start_link(dev, recv);
    if (err < 0) {
        atomic_cas(&data->state, LINK_STARTING, LINK_FAILED);
    }
    k_mutex_unlock(&lifecycle_mutex);
    return err;
}

static int defy_hci_spi_send(const struct device *dev, struct net_buf *buf) {
    struct defy_hci_spi_data *data = dev->data;
    uint8_t type;
    int err;

    switch (bt_buf_get_type(buf)) {
    case BT_BUF_ACL_OUT:
        type = BT_HCI_H4_ACL;
        break;
    case BT_BUF_CMD:
        type = BT_HCI_H4_CMD;
        if (buf->len >= sizeof(struct bt_hci_cmd_hdr)) {
            record_command(sys_get_le16(buf->data));
        }
        break;
    default:
        LOG_ERR("Unsupported buffer type");
        return -EINVAL;
    }

    if (buf->len + 1 > DEFY_HCI_SPI_MAX_MSG_LEN) {
        LOG_ERR("Packet of %d bytes is too long", buf->len);
        return -EINVAL;
    }

    if (atomic_get(&data->state) != LINK_UP) {
        return -EIO;
    }

    net_buf_push_u8(buf, type);

    k_sem_take(&sem_busy, K_FOREVER);
    err = atomic_get(&data->state) == LINK_UP
              ? write_packet(buf->data, buf->len, INT64_MAX)
              : -EIO;
    k_sem_give(&sem_busy);

    if (err < 0) {
        /* The caller frees the buffer when sending fails. */
        net_buf_pull_u8(buf);
        LOG_ERR("Sending failed (%d)", err);
        return err;
    }

    net_buf_unref(buf);
    return 0;
}

static DEVICE_API(bt_hci, defy_hci_spi_api) = {
    .open = defy_hci_spi_open,
    .send = defy_hci_spi_send,
};

#ifdef CONFIG_PM_DEVICE
static int power_off(struct defy_hci_spi_data *data) {
    static const uint8_t command[] = DEFY_HCI_SPI_COMMAND(DEFY_HCI_SPI_OP_POWER_OFF);
    int err;

    k_sem_reset(&sem_power_off);
    atomic_set(&data->power_off_result, -EINPROGRESS);

    if (k_sem_take(&sem_busy, K_MSEC(LOCK_TIMEOUT_MS)) < 0) {
        atomic_set(&data->state, LINK_POWER_OFF_FAILED);
        return -ETIMEDOUT;
    }
    data->power_off_deadline = k_uptime_get() + DEFY_HCI_SPI_POWER_OFF_HOST_TIMEOUT_MS;
    err = write_packet(command, sizeof(command), data->power_off_deadline);
    k_sem_give(&sem_busy);

    if (err == 0) {
        err = k_sem_take(&sem_power_off, K_TIMEOUT_ABS_MS(data->power_off_deadline)) < 0
                  ? -ETIMEDOUT
                  : atomic_get(&data->power_off_result);
    }

    /*
     * A read still in progress at the deadline ends within one transfer and
     * settles any ack before giving the bus back.
     */
    bool bus_free = k_sem_take(&sem_busy, K_MSEC(LOCK_TIMEOUT_MS)) == 0;
    if (err < 0) {
        k_spinlock_key_t key = k_spin_lock(&data->power_off_lock);
        int result = atomic_get(&data->power_off_result);
        if (result != -EINPROGRESS) {
            err = result;
        } else {
            atomic_cas(&data->state, LINK_POWERING_OFF, LINK_POWER_OFF_FAILED);
        }
        k_spin_unlock(&data->power_off_lock, key);
        if (err < 0) {
            LOG_WRN("Bluetooth controller power off failed (%d)", err);
        }
    }
    if (!bus_free) {
        return -ETIMEDOUT;
    }
    k_sem_give(&sem_busy);
    return err;
}

static int defy_hci_spi_pm_action(const struct device *dev, enum pm_device_action action) {
    struct defy_hci_spi_data *data = dev->data;
    int err = 0;
    bool reboot = false;

    if (k_mutex_lock(&lifecycle_mutex, K_MSEC(LOCK_TIMEOUT_MS)) < 0) {
        return -EBUSY;
    }
    switch (action) {
    case PM_DEVICE_ACTION_SUSPEND:
        if (atomic_cas(&data->state, LINK_UP, LINK_POWERING_OFF)) {
            err = power_off(data);
            break;
        }
        /* power_off() never leaves LINK_POWERING_OFF behind. */
        switch (atomic_get(&data->state)) {
        case LINK_POWERED_OFF:
            break;
        case LINK_POWER_OFF_FAILED:
            err = -EIO;
            break;
        default:
            err = -ENOTCONN;
            break;
        }
        break;
    case PM_DEVICE_ACTION_RESUME: {
        atomic_val_t state = atomic_get(&data->state);

        reboot = state == LINK_POWERED_OFF || state == LINK_POWER_OFF_FAILED;
        break;
    }
    default:
        err = -ENOTSUP;
        break;
    }
    k_mutex_unlock(&lifecycle_mutex);
    if (reboot) {
        sys_reboot(SYS_REBOOT_COLD);
    }
    return err;
}

#endif /* CONFIG_PM_DEVICE */

static int defy_hci_spi_init(const struct device *dev) {
    ARG_UNUSED(dev);

    if (!spi_is_ready_dt(&bus)) {
        LOG_ERR("SPI bus not ready");
        return -ENODEV;
    }

    if (!gpio_is_ready_dt(&irq_gpio)) {
        LOG_ERR("Data ready GPIO not ready");
        return -ENODEV;
    }

    gpio_init_callback(&gpio_cb, data_ready_isr, BIT(irq_gpio.pin));
    wake_config = bus.config;
    wake_config.cs.delay = WAKE_PULSE_US;
#ifdef CONFIG_DEFY_RADIO_INSTALL
    factory_config = bus.config;
    factory_config.operation |= SPI_HOLD_ON_CS | SPI_LOCK_ON;
#endif
    memset(filler, 0xff, sizeof(filler));

    return 0;
}

int defy_hci_spi_battery_refresh(void) {
    static const uint8_t command[] = DEFY_HCI_SPI_COMMAND(DEFY_HCI_SPI_OP_BATTERY_REFRESH);
    struct defy_hci_spi_data *data = &defy_hci_spi_data;
    int err;

    if (atomic_get(&data->state) != LINK_UP) {
        return -EAGAIN;
    }
    if (k_sem_take(&sem_busy, K_MSEC(LOCK_TIMEOUT_MS)) < 0) {
        return -EBUSY;
    }
    record_command(DEFY_HCI_SPI_OP_BATTERY_REFRESH);
    err = atomic_get(&data->state) == LINK_UP ? write_packet(command, sizeof(command), INT64_MAX)
                                               : -EAGAIN;
    k_sem_give(&sem_busy);
    return err;
}

void defy_hci_diagnostics_get(struct defy_hci_diagnostics *result) {
    *result = (struct defy_hci_diagnostics){
        .tx_opcode = atomic_get(&diagnostic_tx),
        .rx_opcode = atomic_get(&diagnostic_rx),
    };
}

#ifdef CONFIG_ZTEST
void defy_hci_spi_test_reset(void) {
    struct defy_hci_spi_data *data = &defy_hci_spi_data;

    k_sem_take(&sem_busy, K_FOREVER);
    if (data->recv != NULL) {
        k_thread_abort(&rx_thread_data);
        data->recv = NULL;
    }
    (void)gpio_pin_interrupt_configure_dt(&irq_gpio, GPIO_INT_DISABLE);
    (void)gpio_remove_callback(irq_gpio.port, &gpio_cb);
    atomic_set(&data->state, LINK_DOWN);
#ifdef CONFIG_DEFY_RADIO_INSTALL
    factory_result = FACTORY_NOT_YET;
#endif
    k_sem_reset(&sem_request);
    k_sem_give(&sem_busy);
}
#endif

PM_DEVICE_DT_INST_DEFINE(0, defy_hci_spi_pm_action);

DEVICE_DT_INST_DEFINE(0, defy_hci_spi_init, PM_DEVICE_DT_INST_GET(0), &defy_hci_spi_data, NULL,
                      POST_KERNEL, CONFIG_BT_SPI_INIT_PRIORITY, &defy_hci_spi_api);
