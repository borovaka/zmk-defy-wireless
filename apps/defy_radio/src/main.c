/*
 * Copyright (c) 2017 Linaro Limited
 * Copyright (c) 2016 Nordic Semiconductor ASA
 * Copyright (c) 2015-2016 Intel Corporation
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/*
 * Bluetooth controller for a Defy half's RP2040, over SPI. Based on Zephyr's
 * samples/bluetooth/hci_spi, with its two-thread transfer handshake replaced
 * by one thread around link.c, which tests/drivers/defy_hci_spi runs against
 * the RP2040's driver. Besides that:
 *
 * - The ready event is queued before the link starts, so the host never finds
 *   this radio empty before reading it.
 * - Data ready stays high while any message is unsent.
 * - It takes vendor commands of its own: reboot, power off to System OFF,
 *   from which chip select going low wakes it, and a battery refresh.
 * - It queues battery samples for the host, through battery_hci.c.
 */

#include <errno.h>

#include <zephyr/bluetooth/buf.h>
#include <zephyr/bluetooth/hci.h>
#include <zephyr/bluetooth/hci_raw.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/drivers/spi.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/reboot.h>

#include <dygma/defy_hci_spi.h>

#include "battery.h"
#include "bridge.h"
#include "link.h"
#include "power_off.h"
#include "watchdog.h"

LOG_MODULE_REGISTER(defy_radio, LOG_LEVEL_INF);

/* The link thread's heartbeat while it waits for the host to clock a transfer. */
#define LINK_WAIT_TIMEOUT_MS 100
/*
 * Cooperative, as in Zephyr's hci_spi sample: main() and the battery queue,
 * both preemptible, never interrupt it between arming a reply and starting
 * the transfer.
 */
#define LINK_THREAD_PRIORITY K_PRIO_COOP(7)
/* Command Complete with a status, the longest of the radio's own events. */
#define CONTROL_EVENT_MAX 6

static const struct device *const spi_dev = DEVICE_DT_GET(DEFY_RADIO_SPI_NODE);
static const struct spi_config spi_cfg = {
    .operation = SPI_WORD_SET(8) | SPI_OP_MODE_SLAVE,
};
static const struct gpio_dt_spec data_ready = GPIO_DT_SPEC_GET(DEFY_RADIO_HCI_NODE, irq_gpios);
const struct gpio_dt_spec defy_radio_chip_select =
    GPIO_DT_SPEC_GET(DT_PATH(zephyr_user), chip_select_gpios);

static K_FIFO_DEFINE(controller_queue);

/* Messages for the host, with their H4 packet type, oldest first. */
static K_FIFO_DEFINE(host_queue);
static struct k_spinlock host_queue_lock;

static uint8_t tx[DEFY_HCI_SPI_MAX_MSG_LEN];
static uint8_t rx[DEFY_HCI_SPI_MAX_MSG_LEN];

static K_THREAD_STACK_DEFINE(link_stack, CONFIG_BT_HCI_TX_STACK_SIZE);
static struct k_thread link_thread_data;

NET_BUF_POOL_FIXED_DEFINE(control_pool, 2, BT_BUF_RESERVE + CONTROL_EVENT_MAX,
                          sizeof(struct bt_buf_data), NULL);

static bool power_off_requested;
static int64_t power_off_deadline;
static K_SEM_DEFINE(spi_done, 0, 1);
static int spi_result;

void defy_radio_queue_for_host(struct net_buf *buf, bool control) {
    k_spinlock_key_t key;

    switch (bt_buf_get_type(buf)) {
    case BT_BUF_ACL_IN:
        net_buf_push_u8(buf, BT_HCI_H4_ACL);
        break;
    case BT_BUF_EVT:
        net_buf_push_u8(buf, BT_HCI_H4_EVT);
        break;
    default:
        LOG_ERR("Unknown buffer type %u", bt_buf_get_type(buf));
        net_buf_unref(buf);
        return;
    }

    if (buf->len > DEFY_HCI_SPI_MAX_MSG_LEN) {
        LOG_ERR("Message of %u bytes is too long", buf->len);
        net_buf_unref(buf);
        return;
    }

    key = k_spin_lock(&host_queue_lock);
    if (power_off_requested && !control) {
        k_spin_unlock(&host_queue_lock, key);
        net_buf_unref(buf);
        return;
    }
    k_fifo_put(&host_queue, buf);
    gpio_pin_set_dt(&data_ready, 1);
    k_spin_unlock(&host_queue_lock, key);
}

/* Queues an HCI event of the radio's own, without its H4 packet type. */
static void queue_event(const uint8_t *event, size_t len) {
    struct net_buf *buf = net_buf_alloc(&control_pool, K_NO_WAIT);

    if (buf == NULL) {
        LOG_ERR("No buffer for a control event");
        return;
    }
    net_buf_reserve(buf, BT_BUF_RESERVE);
    bt_buf_set_type(buf, BT_BUF_EVT);
    net_buf_add_mem(buf, event, len);
    defy_radio_queue_for_host(buf, true);
}

/* Drops the oldest message, which the host has read. */
static void sent(void) {
    k_spinlock_key_t key = k_spin_lock(&host_queue_lock);
    struct net_buf *buf = k_fifo_get(&host_queue, K_NO_WAIT);

    if (k_fifo_is_empty(&host_queue)) {
        gpio_pin_set_dt(&data_ready, 0);
    }
    k_spin_unlock(&host_queue_lock, key);

    net_buf_unref(buf);
    defy_radio_battery_hci_sent();
}

static void to_controller(const uint8_t *packet, size_t len) {
    enum bt_buf_type type = packet[0] == BT_HCI_H4_CMD ? BT_BUF_CMD : BT_BUF_ACL_OUT;
    struct net_buf *buf = bt_buf_get_tx(type, K_NO_WAIT, &packet[1], len - 1);
    int err;

    if (buf == NULL) {
        LOG_ERR("No buffer for a packet from the host");
        return;
    }

    err = bt_send(buf);
    if (err < 0) {
        LOG_ERR("Controller refused a packet (%d)", err);
        net_buf_unref(buf);
    }
}

static void request_power_off(void) {
    static const uint8_t reply[] =
        DEFY_HCI_SPI_COMMAND_COMPLETE(DEFY_HCI_SPI_OP_POWER_OFF, BT_HCI_ERR_SUCCESS);
    k_spinlock_key_t key = k_spin_lock(&host_queue_lock);

    if (power_off_requested) {
        k_spin_unlock(&host_queue_lock, key);
        return;
    }
    defy_radio_battery_hci_power_off();
    power_off_deadline = k_uptime_get() + DEFY_HCI_SPI_POWER_OFF_RADIO_TIMEOUT_MS;
    power_off_requested = true;
    k_spin_unlock(&host_queue_lock, key);
    queue_event(reply, sizeof(reply));
}

static void spi_complete(const struct device *dev, int result, void *userdata) {
    ARG_UNUSED(dev);
    ARG_UNUSED(userdata);

    spi_result = result;
    k_sem_give(&spi_done);
}

static FUNC_NORETURN void finish_power_off(void) {
    defy_radio_battery_stop();
    gpio_pin_set_dt(&data_ready, 0);
    while (true) {
        /* -EBUSY until the RP2040 lets go of chip select. */
        (void)defy_radio_power_off();
        k_sleep(K_MSEC(1));
    }
}

static void link_thread(void *p1, void *p2, void *p3) {
    struct defy_link link;

    ARG_UNUSED(p1);
    ARG_UNUSED(p2);
    ARG_UNUSED(p3);

    defy_link_init(&link);

    while (true) {
        defy_radio_watchdog_heartbeat();
        if (power_off_requested &&
            (k_fifo_is_empty(&host_queue) || k_uptime_get() >= power_off_deadline)) {
            finish_power_off();
        }

        struct net_buf *oldest = k_fifo_peek_head(&host_queue);
        const struct spi_buf tx_buf = {
            .buf = tx,
            .len = defy_link_arm(&link, oldest != NULL ? oldest->data : NULL,
                                 oldest != NULL ? oldest->len : 0, tx),
        };
        const struct spi_buf rx_buf = {.buf = rx, .len = sizeof(rx)};
        const struct spi_buf_set tx_set = {.buffers = &tx_buf, .count = 1};
        const struct spi_buf_set rx_set = {.buffers = &rx_buf, .count = 1};

        if (link.phase == DEFY_LINK_SEND && oldest != NULL) {
            defy_radio_battery_hci_arm(oldest, tx);
        }
        int err = spi_transceive_cb(spi_dev, &spi_cfg, &tx_set, &rx_set, spi_complete, NULL);

        if (err < 0) {
            LOG_ERR("SPI transfer failed (%d)", err);
            k_sleep(K_MSEC(1));
            continue;
        }

        while (true) {
            int64_t wait_deadline = k_uptime_get() + LINK_WAIT_TIMEOUT_MS;

            if (power_off_requested && power_off_deadline < wait_deadline) {
                wait_deadline = power_off_deadline;
            }
            if (k_sem_take(&spi_done, K_TIMEOUT_ABS_MS(wait_deadline)) == 0) {
                break;
            }
            defy_radio_watchdog_heartbeat();
            if (power_off_requested && k_uptime_get() >= power_off_deadline) {
                finish_power_off();
            }
        }
        if (spi_result < 0) {
            LOG_ERR("SPI transfer failed (%d)", spi_result);
            continue;
        }

        switch (defy_link_done(&link, rx, spi_result)) {
        case DEFY_LINK_SENT:
            sent();
            break;
        case DEFY_LINK_PACKET:
            to_controller(rx, link.packet_len);
            break;
        case DEFY_LINK_REBOOT:
            sys_reboot(SYS_REBOOT_COLD);
            break;
        case DEFY_LINK_POWER_OFF:
            request_power_off();
            break;
        case DEFY_LINK_BATTERY_REFRESH: {
            const uint8_t reply[] = DEFY_HCI_SPI_COMMAND_COMPLETE(
                DEFY_HCI_SPI_OP_BATTERY_REFRESH, defy_radio_battery_hci_refresh());

            queue_event(reply, sizeof(reply));
            break;
        }
        case DEFY_LINK_NONE:
            break;
        }
    }
}

int main(void) {
    static const uint8_t ready[] = DEFY_HCI_SPI_READY_EVENT;
    int err;

    if (!device_is_ready(spi_dev) || !gpio_is_ready_dt(&data_ready) ||
        !gpio_is_ready_dt(&defy_radio_chip_select)) {
        LOG_ERR("SPI or GPIO not ready");
        return 0;
    }

    gpio_pin_configure_dt(&data_ready, GPIO_OUTPUT_INACTIVE);

    err = bt_enable_raw(&controller_queue);
    if (err < 0) {
        LOG_ERR("bt_enable_raw failed (%d)", err);
        return 0;
    }

    /* Before the link starts, so the first header the host reads from this boot has it. */
    queue_event(ready, sizeof(ready));
    defy_radio_battery_start();

    k_thread_create(&link_thread_data, link_stack, K_THREAD_STACK_SIZEOF(link_stack), link_thread,
                    NULL, NULL, NULL, LINK_THREAD_PRIORITY, 0, K_NO_WAIT);
    k_thread_name_set(&link_thread_data, "link");

    while (true) {
        defy_radio_queue_for_host(k_fifo_get(&controller_queue, K_FOREVER), false);
    }

    return 0;
}
