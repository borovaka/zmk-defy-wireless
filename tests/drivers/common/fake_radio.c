/*
 * The fake nRF52820. Like an nRF52 SPIS, it arms its reply as chip select
 * falls and takes what the host sent as it rises. An unarmed or sleeping radio
 * sends 0, defy_rf's default character, and past the armed reply it sends
 * 0xff, the overread character.
 *
 * SPDX-License-Identifier: MIT
 */

#include <string.h>

#include <zephyr/bluetooth/hci_types.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/drivers/gpio/gpio_emul.h>
#include <zephyr/init.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/byteorder.h>
#include <zephyr/sys/crc.h>

#include <dygma/defy_hci_spi.h>
#include <dygma/defy_radio_factory.h>
#include <dygma/defy_radio_image.h>

#include "fake_radio.h"
#include "link.h"

#define QUEUE_SIZE 8
#define DEFAULT_CHARACTER 0x00
#define OVERREAD_CHARACTER 0xff
#define BOOT_MS 30
#define WINDOW_MS 300
#define SLOT_SIZE (DEFY_RADIO_SLOT_END - DEFY_RADIO_SLOT_START)
#define STOCK_VERSION 0x10201u
#define STOCK_LENGTH 256u

static const struct gpio_dt_spec data_ready = GPIO_DT_SPEC_GET(DT_NODELABEL(radio), irq_gpios);

static void boot_done(struct k_work *work);
static void window_expired(struct k_work *work);
static void power_off_done(struct k_work *work);
static void power_off_reply_ready(struct k_work *work);
static void power_off_expired(struct k_work *work);
static atomic_t block_next_transfer;
static K_SEM_DEFINE(transfer_blocked, 0, 1);
static K_SEM_DEFINE(transfer_release, 0, 1);

/* What the tests set, back to the defaults at every reset. */
static struct options {
    /* Whether a reset passes through rf_bootloader. */
    bool bootloader;
    int boot_ms;
    bool ignore_reboot;
    int power_off_ms;
    int power_off_reply;
    int power_off_reply_ms;
    int power_off_timeout_ms;
    int power_off_read_delay_ms;
    bool fail_next_transfer;
    int missed_transfers;
    enum fake_radio_rx_fault rx_fault;
    bool s113;
    bool foreign;
    unsigned int reject_hellos;
    uint8_t reject_command;
    uint8_t drop_receive_for;
    uint8_t ignore_receive_for;
} options;

static struct {
    struct k_mutex lock;
    enum fake_radio_state state;
    struct fake_radio_counts counts;

    /* The transfer: answering if the running firmware armed out, and so takes in. */
    bool selected;
    bool answering;
    enum fake_radio_rx_fault fault;
    uint8_t in[DEFY_HCI_SPI_MAX_MSG_LEN];
    uint8_t out[DEFY_HCI_SPI_MAX_MSG_LEN];
    size_t clocked;

    /* The controller. */
    struct defy_link link;
    uint8_t queue[QUEUE_SIZE][DEFY_HCI_SPI_MAX_MSG_LEN];
    size_t queue_len[QUEUE_SIZE];
    size_t head;
    size_t count;
    uint8_t armed[DEFY_HCI_SPI_MAX_MSG_LEN];
    size_t armed_len;
    bool off_when_sent;

    /* Dygma's framing. */
    uint8_t status;
    bool writable;
    bool reply_header_next;
    uint8_t reply[64];
    size_t reply_len;

    /* rf_bootloader's flash and session. */
    uint8_t slot[SLOT_SIZE];
    uint8_t seal[DEFY_RADIO_SEAL_SIZE];
    bool held;
    bool seal_staged;
    bool metadata_erased;
    bool seal_written;
    uint8_t staged[DEFY_RADIO_SEAL_SIZE];
    uint8_t command;
    uint8_t parser;
    uint32_t address;
    uint32_t length;

    struct k_work_delayable boot_work;
    struct k_work_delayable window_work;
    struct k_work_delayable power_off_work;
    struct k_work_delayable power_off_reply_work;
    struct k_work_delayable power_off_timeout_work;
} radio = {
    .lock = Z_MUTEX_INITIALIZER(radio.lock),
    .boot_work = Z_WORK_DELAYABLE_INITIALIZER(boot_done),
    .window_work = Z_WORK_DELAYABLE_INITIALIZER(window_expired),
    .power_off_work = Z_WORK_DELAYABLE_INITIALIZER(power_off_done),
    .power_off_reply_work = Z_WORK_DELAYABLE_INITIALIZER(power_off_reply_ready),
    .power_off_timeout_work = Z_WORK_DELAYABLE_INITIALIZER(power_off_expired),
};

static bool up(void) {
    return radio.state == FAKE_RADIO_BOOTED || radio.state == FAKE_RADIO_RUNNING;
}

static void update_data_ready(void) {
    gpio_emul_input_set(data_ready.port, data_ready.pin,
                        options.rx_fault != FAKE_RX_NONE || radio.state == FAKE_RADIO_STOPPING ||
                            (up() && radio.count > 0));
}

static void push(const uint8_t *msg, size_t len) {
    size_t tail = (radio.head + radio.count) % QUEUE_SIZE;

    __ASSERT(radio.count < QUEUE_SIZE, "fake radio queue full");
    memcpy(radio.queue[tail], msg, len);
    radio.queue_len[tail] = len;
    radio.count++;
}

static void pop(void) {
    radio.head = (radio.head + 1) % QUEUE_SIZE;
    radio.count--;
}

static void arm(void) {
    radio.armed_len = defy_link_arm(&radio.link, radio.count > 0 ? radio.queue[radio.head] : NULL,
                                    radio.count > 0 ? radio.queue_len[radio.head] : 0, radio.armed);
}

static void clear(void) {
    radio.head = 0;
    radio.count = 0;
    radio.off_when_sent = false;
    defy_link_init(&radio.link);
}

/*
 * Up, as at the end of a boot: the ready event queued before the link starts,
 * as the radio app does.
 */
static void come_up(enum fake_radio_state state) {
    static const uint8_t ready[] = DEFY_HCI_SPI_READY_EVENT;
    uint8_t msg[1 + sizeof(ready)] = {BT_HCI_H4_EVT};

    clear();
    radio.state = state;
    if (state == FAKE_RADIO_BOOTED) {
        memcpy(&msg[1], ready, sizeof(ready));
        push(msg, sizeof(msg));
    }
    arm();
    update_data_ready();
}

static void boot(void) {
    radio.state = FAKE_RADIO_BOOTING;
    k_work_reschedule(&radio.boot_work, K_MSEC(options.boot_ms));
}

static void boot_done(struct k_work *work) {
    ARG_UNUSED(work);

    k_mutex_lock(&radio.lock, K_FOREVER);
    if (radio.state == FAKE_RADIO_BOOTING) {
        radio.counts.boots++;
        come_up(FAKE_RADIO_BOOTED);
    }
    k_mutex_unlock(&radio.lock);
}

static void reset_framing(void) {
    radio.status = 0x82;
    radio.writable = false;
    radio.reply_header_next = false;
    radio.reply_len = 0;
}

static void enter_bootloader(void) {
    k_work_cancel_delayable(&radio.boot_work);
    radio.state = FAKE_RADIO_BOOTLOADER;
    radio.held = radio.seal_staged = radio.metadata_erased = radio.seal_written = false;
    radio.parser = 0;
    reset_framing();
    radio.counts.bootloader_entries++;
    k_work_reschedule(&radio.window_work, K_MSEC(WINDOW_MS));
}

/* A reset: through rf_bootloader if the radio has it, and otherwise straight to booting. */
static void restart(void) {
    clear();
    if (options.bootloader) {
        enter_bootloader();
    } else {
        boot();
    }
    update_data_ready();
}

static bool seal_valid(const uint8_t *seal) {
    uint8_t copy[DEFY_RADIO_SEAL_SIZE];

    memcpy(copy, seal, sizeof(copy));
    memset(copy + 8, 0, 4);
    return sys_get_le32(seal) == 1 && sys_get_le32(seal + 4) == DEFY_RADIO_SEAL_SIZE &&
           crc32_ieee(copy, sizeof(copy)) == sys_get_le32(seal + 8);
}

/* The image a seal describes is in the slot. */
static bool program_valid(const uint8_t *seal) {
    return seal_valid(seal) && sys_get_le32(seal + 12) == DEFY_RADIO_SLOT_START &&
           sys_get_le32(seal + 16) <= SLOT_SIZE &&
           crc32_ieee(radio.slot, sys_get_le32(seal + 16)) == sys_get_le32(seal + 20);
}

/* Through S113 to the slot: this firmware boots, Dygma's runs, and anything else sleeps. */
static void hand_off(void) {
    k_work_cancel_delayable(&radio.window_work);
    radio.state = FAKE_RADIO_SILENT;
    if (options.s113 && program_valid(radio.seal)) {
        if (sys_get_le32(radio.seal + 24) == STOCK_VERSION) {
            radio.state = FAKE_RADIO_STOCK;
            reset_framing();
        } else {
            boot();
        }
    }
    update_data_ready();
}

static void window_expired(struct k_work *work) {
    ARG_UNUSED(work);

    k_mutex_lock(&radio.lock, K_FOREVER);
    if (radio.state == FAKE_RADIO_BOOTLOADER && !radio.held) {
        radio.counts.windows_expired++;
        hand_off();
    }
    k_mutex_unlock(&radio.lock);
}

static void power_off_done(struct k_work *work) {
    ARG_UNUSED(work);

    k_mutex_lock(&radio.lock, K_FOREVER);
    if (radio.state == FAKE_RADIO_STOPPING) {
        radio.state = FAKE_RADIO_OFF;
        update_data_ready();
    }
    k_mutex_unlock(&radio.lock);
}

/* The stub controller completes every command. */
static void controller(const uint8_t *packet) {
    uint16_t opcode;

    if (packet[0] != BT_HCI_H4_CMD) {
        return;
    }

    opcode = sys_get_le16(&packet[1]);
    radio.counts.commands++;
    radio.counts.last_opcode = opcode;

    const uint8_t complete[] = {BT_HCI_H4_EVT, BT_HCI_EVT_CMD_COMPLETE, 4, 1, opcode & 0xff,
                                opcode >> 8, BT_HCI_ERR_SUCCESS};
    push(complete, sizeof(complete));
}

static void queue_power_off_reply(void) {
    const uint8_t reply[] =
        DEFY_HCI_SPI_COMMAND_COMPLETE(DEFY_HCI_SPI_OP_POWER_OFF, (uint8_t)options.power_off_reply);
    uint8_t msg[1 + sizeof(reply)] = {BT_HCI_H4_EVT};

    if (options.power_off_reply == -1) {
        return;
    }
    memcpy(&msg[1], reply, sizeof(reply));
    if (options.power_off_reply == -2) {
        /* Without its status. */
        msg[2] = 3;
        push(msg, sizeof(msg) - 1);
    } else {
        push(msg, sizeof(msg));
    }
    radio.off_when_sent = options.power_off_reply == 0;
}

static void power_off_reply_ready(struct k_work *work) {
    ARG_UNUSED(work);
    k_mutex_lock(&radio.lock, K_FOREVER);
    if (up()) {
        queue_power_off_reply();
        update_data_ready();
    }
    k_mutex_unlock(&radio.lock);
}

static void power_off_expired(struct k_work *work) {
    ARG_UNUSED(work);
    k_mutex_lock(&radio.lock, K_FOREVER);
    clear();
    radio.state = FAKE_RADIO_OFF;
    options.rx_fault = FAKE_RX_NONE;
    update_data_ready();
    k_mutex_unlock(&radio.lock);
}

static void power_off_requested(void) {
    if (options.power_off_timeout_ms > 0) {
        k_work_reschedule(&radio.power_off_timeout_work, K_MSEC(options.power_off_timeout_ms));
        if (options.power_off_reply == -1) {
            options.rx_fault = FAKE_RX_EMPTY;
        }
    }
    if (options.power_off_reply_ms > 0) {
        k_work_reschedule(&radio.power_off_reply_work, K_MSEC(options.power_off_reply_ms));
    } else {
        queue_power_off_reply();
    }
}

/* The controller takes a transfer. */
static void take(const uint8_t *mosi, size_t len) {
    switch (defy_link_done(&radio.link, mosi, len)) {
    case DEFY_LINK_SENT:
        pop();
        if (radio.off_when_sent && radio.count == 0) {
            clear();
            if (options.power_off_ms > 0) {
                radio.state = FAKE_RADIO_STOPPING;
                k_work_reschedule(&radio.power_off_work, K_MSEC(options.power_off_ms));
            } else {
                radio.state = FAKE_RADIO_OFF;
            }
        }
        break;
    case DEFY_LINK_PACKET:
        controller(mosi);
        break;
    case DEFY_LINK_REBOOT:
        radio.counts.reboot_commands++;
        if (!options.ignore_reboot) {
            restart();
        }
        break;
    case DEFY_LINK_POWER_OFF:
        radio.counts.power_off_commands++;
        power_off_requested();
        break;
    case DEFY_LINK_BATTERY_REFRESH: {
        static const uint8_t reply[] =
            DEFY_HCI_SPI_COMMAND_COMPLETE(DEFY_HCI_SPI_OP_BATTERY_REFRESH, BT_HCI_ERR_SUCCESS);
        uint8_t msg[1 + sizeof(reply)] = {BT_HCI_H4_EVT};

        radio.counts.refresh_commands++;
        memcpy(&msg[1], reply, sizeof(reply));
        push(msg, sizeof(msg));
        break;
    }
    case DEFY_LINK_NONE:
        break;
    }
}

static void answer(const uint8_t *data, size_t len) {
    memcpy(radio.reply, data, len);
    radio.reply_len = len;
}

/* The first erase or write of a session erases the seal page. */
static void erase_seal_page(void) {
    if (!radio.metadata_erased) {
        memset(radio.seal, 0xff, sizeof(radio.seal));
        radio.metadata_erased = true;
    }
}

static void bootloader_packet(const uint8_t *data, size_t len) {
    uint8_t response[44] = {0};

    if (radio.parser == 0) {
        if (len != 1) {
            return;
        }
        radio.command = data[0];
        if (data[0] == options.reject_command) {
            radio.counts.rejected_commands++;
            radio.status = 0x81;
            return;
        }
        switch (data[0]) {
        case 'B':
            if (radio.counts.ignored_receive_requests > 0) {
                options.ignore_receive_for = 0;
            }
            radio.counts.begins++;
            radio.held = true;
            k_work_cancel_delayable(&radio.window_work);
            break;
        case 'I': {
            radio.counts.infos++;
            uint32_t version = seal_valid(radio.seal) ? sys_get_le32(radio.seal + 24) : UINT32_MAX;
            uint32_t id = options.foreign ? 0x12345678 : 0x57474652;
            uint32_t values[] = {id, 1, 1, version, 0x1000,
                                 options.s113 ? DEFY_RADIO_SLOT_START : 0x1000,
                                 DEFY_RADIO_SEAL_ADDRESS, 0x38000, 4096, 4, 128};

            for (size_t i = 0; i < ARRAY_SIZE(values); i++) {
                sys_put_le32(values[i], &response[i * 4]);
            }
            answer(response, sizeof(response));
            break;
        }
        case 'S':
        case 'E':
        case 'W':
        case 'R':
            radio.parser = data[0];
            break;
        case 'V':
            response[0] = radio.seal_staged && program_valid(radio.staged);
            answer(response, 1);
            break;
        case 'F':
            radio.counts.finishes++;
            if (radio.seal_staged && !radio.seal_written && program_valid(radio.staged)) {
                memcpy(radio.seal, radio.staged, sizeof(radio.seal));
                radio.seal_written = true;
                radio.counts.seals++;
            }
            hand_off();
            break;
        }
        return;
    }

    if (radio.parser == 'S') {
        radio.parser = 0;
        radio.seal_staged = len == DEFY_RADIO_SEAL_SIZE && seal_valid(data);
        if (radio.seal_staged) {
            memcpy(radio.staged, data, len);
        }
        response[0] = radio.seal_staged;
        answer(response, 1);
        return;
    }

    if (radio.parser == 0xff) {
        radio.parser = 0;
        uint32_t offset = radio.address - DEFY_RADIO_SLOT_START;

        __ASSERT(len == radio.length && offset + len + 4 <= SLOT_SIZE, "write outside the slot");
        if (radio.seal_staged) {
            erase_seal_page();
            for (size_t i = 0; i < len; i++) {
                radio.slot[offset + i] &= data[i];
            }
            radio.counts.writes++;
            sys_put_le32(crc32_ieee(&radio.slot[offset], len), response);
        }
        answer(response, 4);
        return;
    }

    __ASSERT(len == 8, "parameters are 8 bytes");
    radio.address = sys_get_le32(data);
    radio.length = sys_get_le32(data + 4);
    if (radio.parser == 'W') {
        radio.parser = 0xff;
        return;
    }
    if (radio.parser == 'R') {
        radio.parser = 0;
        if (radio.address >= DEFY_RADIO_SEAL_ADDRESS) {
            answer(&radio.seal[radio.address - DEFY_RADIO_SEAL_ADDRESS], radio.length);
        } else {
            answer(&radio.slot[radio.address - DEFY_RADIO_SLOT_START], radio.length);
        }
        return;
    }
    radio.parser = 0;
    __ASSERT(radio.address >= DEFY_RADIO_SLOT_START && radio.address % 4096 == 0 &&
                 radio.length == 4096 && radio.address + 4096 <= DEFY_RADIO_SLOT_END,
             "erase outside the slot");
    if (radio.seal_staged) {
        erase_seal_page();
        memset(&radio.slot[radio.address - DEFY_RADIO_SLOT_START], 0xff, 4096);
        radio.counts.erases++;
    }
    response[0] = radio.seal_staged;
    answer(response, 1);
}

/* rf_bootloader or Dygma's application takes a transfer. */
static void dygma_complete(const uint8_t *mosi, size_t len) {
    if (radio.reply_header_next) {
        if (len < radio.reply_len + 2) {
            radio.counts.truncated_replies++;
        }
        radio.reply_len = 0;
        radio.reply_header_next = false;
    }
    if (len < 2) {
        radio.counts.short_frames++;
        radio.status = 0x81;
        radio.writable = false;
        return;
    }
    if (mosi[1] == 1) {
        bool reject = radio.state == FAKE_RADIO_BOOTLOADER && options.reject_hellos > 0;

        if (reject) {
            options.reject_hellos--;
            radio.counts.rejected_hellos++;
        }
        radio.writable = mosi[0] == 2 && !reject;
        radio.status = radio.writable ? 0x80 : 0x81;
    } else if (mosi[1] == 3) {
        bool valid = radio.writable && mosi[0] >= 2 && mosi[0] <= len;

        radio.writable = false;
        radio.status = valid ? 0x80 : 0x81;
        if (!valid) {
            radio.counts.short_frames++;
        } else if (radio.state == FAKE_RADIO_BOOTLOADER) {
            bootloader_packet(mosi + 2, mosi[0] - 2);
        }
    } else if (mosi[0] == 2 && mosi[1] == 2) {
        if (options.drop_receive_for != 0 && radio.command == options.drop_receive_for) {
            options.drop_receive_for = 0;
            radio.counts.dropped_receive_requests++;
            return;
        }
        if (options.ignore_receive_for != 0 && radio.command == options.ignore_receive_for) {
            radio.counts.ignored_receive_requests++;
            return;
        }
        radio.reply_header_next = true;
        radio.writable = false;
    } else {
        radio.status = radio.writable ? 0x80 : radio.reply_len > 0 ? 0x85 : 0x82;
    }
}

/* Chip select falls: a failed or faulty transfer, a wake, or the firmware arms its reply. */
static void begin_transfer(void) {
    radio.clocked = 0;
    radio.answering = false;
    radio.fault = FAKE_RX_NONE;
    memset(radio.out, DEFAULT_CHARACTER, sizeof(radio.out));
    radio.counts.transfers++;

    if (options.fail_next_transfer) {
        options.fail_next_transfer = false;
        radio.fault = FAKE_RX_ERROR;
        return;
    }
    if (options.rx_fault != FAKE_RX_NONE && radio.count == 0) {
        radio.fault = options.rx_fault;
        return;
    }

    switch (radio.state) {
    case FAKE_RADIO_OFF:
        radio.counts.wakes++;
        restart();
        break;
    case FAKE_RADIO_STOPPING:
        radio.counts.transfers_after_power_off_reply++;
        break;
    case FAKE_RADIO_BOOTED:
    case FAKE_RADIO_RUNNING:
        if (options.missed_transfers > 0) {
            options.missed_transfers--;
            break;
        }
        radio.answering = true;
        memset(radio.out, OVERREAD_CHARACTER, sizeof(radio.out));
        memcpy(radio.out, radio.armed, radio.armed_len);
        break;
    case FAKE_RADIO_BOOTLOADER:
    case FAKE_RADIO_STOCK:
        radio.answering = true;
        memset(radio.out, OVERREAD_CHARACTER, sizeof(radio.out));
        if (radio.reply_header_next) {
            radio.out[0] = radio.reply_len + 2;
            radio.out[1] = 3;
            memcpy(&radio.out[2], radio.reply, radio.reply_len);
        } else {
            radio.out[0] = 2;
            radio.out[1] = radio.status;
        }
        break;
    default:
        break;
    }
}

/* Chip select rises. Returns how long the host's transfer stalls. */
static int end_transfer(void) {
    int delay_ms = 0;

    if (radio.counts.power_off_commands > 0 && radio.clocked == DEFY_HCI_SPI_HEADER_LEN &&
        radio.in[0] == DEFY_HCI_SPI_READ) {
        delay_ms = options.power_off_read_delay_ms;
        options.power_off_read_delay_ms = 0;
    }
    if (radio.answering && up()) {
        take(radio.in, radio.clocked);
        if (up()) {
            arm();
        }
    } else if (radio.answering) {
        /* Armed by rf_bootloader or Dygma's application. */
        dygma_complete(radio.in, radio.clocked);
    }
    update_data_ready();
    return delay_ms;
}

void fake_radio_select(bool selected) {
    int delay_ms = 0;

    if (selected && atomic_cas(&block_next_transfer, 1, 0)) {
        k_sem_give(&transfer_blocked);
        k_sem_take(&transfer_release, K_FOREVER);
    }
    k_mutex_lock(&radio.lock, K_FOREVER);
    __ASSERT(radio.selected != selected, "missing chip-select edge");
    radio.selected = selected;
    if (selected) {
        begin_transfer();
    } else {
        delay_ms = end_transfer();
    }
    k_mutex_unlock(&radio.lock);
    if (delay_ms > 0) {
        k_sleep(K_MSEC(delay_ms));
    }
}

int fake_radio_clock(const uint8_t *tx, uint8_t *rx, size_t length) {
    enum fake_radio_rx_fault fault;

    k_mutex_lock(&radio.lock, K_FOREVER);
    __ASSERT(radio.selected, "clocked with chip select inactive");
    __ASSERT(radio.clocked + length <= sizeof(radio.in), "transfer too long");
    memcpy(&radio.in[radio.clocked], tx, length);
    memcpy(rx, &radio.out[radio.clocked], length);
    radio.clocked += length;
    fault = radio.fault;
    k_mutex_unlock(&radio.lock);

    if ((fault == FAKE_RX_EMPTY || fault == FAKE_RX_INVALID) && length > 0) {
        if (length == DEFY_HCI_SPI_HEADER_LEN) {
            rx[0] = DEFY_HCI_SPI_READY_NOW;
            rx[1] = DEFY_HCI_SPI_SANITY_CHECK;
            rx[DEFY_HCI_SPI_HEADER_TOREAD] = fault == FAKE_RX_INVALID ? 1 : 0;
        } else {
            rx[0] = BT_HCI_H4_EVT;
        }
    }
    return fault == FAKE_RX_ERROR ? -EIO : 0;
}

/* Back to the default options and zero counts, in state, with nothing queued. */
static void reset(enum fake_radio_state state, bool bootloader) {
    k_work_cancel_delayable(&radio.boot_work);
    k_work_cancel_delayable(&radio.window_work);
    k_work_cancel_delayable(&radio.power_off_work);
    k_work_cancel_delayable(&radio.power_off_reply_work);
    k_work_cancel_delayable(&radio.power_off_timeout_work);

    /* The RP2040's pins come out of reset as inputs, before the driver configures them. */
    (void)gpio_pin_configure_dt(&data_ready, GPIO_INPUT);

    k_mutex_lock(&radio.lock, K_FOREVER);
    memset(&radio.counts, 0, sizeof(radio.counts));
    options = (struct options){.bootloader = bootloader, .boot_ms = BOOT_MS, .s113 = true};
    clear();
    reset_framing();
    switch (state) {
    case FAKE_RADIO_BOOTING:
        boot();
        break;
    case FAKE_RADIO_BOOTED:
    case FAKE_RADIO_RUNNING:
        come_up(state);
        break;
    default:
        radio.state = state;
        break;
    }
    update_data_ready();
    k_mutex_unlock(&radio.lock);
}

/* A program in the slot, sealed as version. */
static void store(const uint8_t *program, size_t len, uint32_t version) {
    uint8_t seal[DEFY_RADIO_SEAL_SIZE];

    sys_put_le32(1, seal);
    sys_put_le32(DEFY_RADIO_SEAL_SIZE, seal + 4);
    sys_put_le32(0, seal + 8);
    sys_put_le32(DEFY_RADIO_SLOT_START, seal + 12);
    sys_put_le32(len, seal + 16);
    sys_put_le32(crc32_ieee(program, len), seal + 20);
    sys_put_le32(version, seal + 24);
    sys_put_le32(crc32_ieee(seal, sizeof(seal)), seal + 8);

    k_mutex_lock(&radio.lock, K_FOREVER);
    memset(radio.slot, 0xff, sizeof(radio.slot));
    memcpy(radio.slot, program, len);
    memcpy(radio.seal, seal, sizeof(seal));
    k_mutex_unlock(&radio.lock);
}

void fake_radio_reset(enum fake_radio_state state) { reset(state, false); }

void fake_radio_reset_stock(void) {
    uint8_t program[STOCK_LENGTH];

    for (size_t i = 0; i < sizeof(program); i++) {
        program[i] = (uint8_t)(i * 7u + 3u);
    }
    reset(FAKE_RADIO_STOCK, true);
    store(program, sizeof(program), STOCK_VERSION);
}

/* Only a build that installs the radio firmware has its image. */
#ifdef CONFIG_DEFY_RADIO_INSTALL
void fake_radio_reset_installed(void) { fake_radio_reset_custom(defy_radio_image_version, false); }

void fake_radio_reset_custom(uint32_t version, bool ready) {
    reset(ready ? FAKE_RADIO_BOOTED : FAKE_RADIO_RUNNING, true);
    store(defy_radio_image, defy_radio_image_size, version);
}

bool fake_radio_holds_image(void) {
    k_mutex_lock(&radio.lock, K_FOREVER);
    bool holds = memcmp(radio.slot, defy_radio_image, defy_radio_image_size) == 0 &&
                 memcmp(radio.seal, defy_radio_image_seal, DEFY_RADIO_SEAL_SIZE) == 0;
    k_mutex_unlock(&radio.lock);
    return holds;
}
#endif

void fake_radio_power_on(void) {
    k_mutex_lock(&radio.lock, K_FOREVER);
    restart();
    radio.counts.bootloader_entries = 0;
    k_mutex_unlock(&radio.lock);
}

void fake_radio_set_boot_ms(int ms) {
    k_mutex_lock(&radio.lock, K_FOREVER);
    options.boot_ms = ms;
    if (radio.state == FAKE_RADIO_BOOTING) {
        k_work_reschedule(&radio.boot_work, K_MSEC(ms));
    }
    k_mutex_unlock(&radio.lock);
}

void fake_radio_set_power_off_ms(int ms) { options.power_off_ms = ms; }

void fake_radio_set_power_off_reply(int status) { options.power_off_reply = status; }

void fake_radio_set_power_off_timing(int reply_ms, int timeout_ms, int read_delay_ms) {
    options.power_off_reply_ms = reply_ms;
    options.power_off_timeout_ms = timeout_ms;
    options.power_off_read_delay_ms = read_delay_ms;
}

void fake_radio_fail_next_transfer(void) { options.fail_next_transfer = true; }

void fake_radio_block_next_transfer(void) {
    k_sem_reset(&transfer_blocked);
    k_sem_reset(&transfer_release);
    atomic_set(&block_next_transfer, 1);
}

int fake_radio_wait_blocked(void) { return k_sem_take(&transfer_blocked, K_MSEC(100)); }

void fake_radio_unblock_transfer(void) { k_sem_give(&transfer_release); }

void fake_radio_rx_fault(enum fake_radio_rx_fault fault) {
    k_mutex_lock(&radio.lock, K_FOREVER);
    options.rx_fault = fault;
    update_data_ready();
    k_mutex_unlock(&radio.lock);
}

void fake_radio_ignore_reboot(bool ignore) { options.ignore_reboot = ignore; }

void fake_radio_skip_bootloader(bool skip) { options.bootloader = !skip; }

void fake_radio_miss_transfers(int n) { options.missed_transfers = n; }

void fake_radio_send(const uint8_t *msg, size_t len) {
    k_mutex_lock(&radio.lock, K_FOREVER);
    push(msg, len);
    /* The armed header stays as it was: the host finds the message on a later header. */
    update_data_ready();
    k_mutex_unlock(&radio.lock);
}

void fake_radio_start_read(void) {
    static const uint8_t read[DEFY_HCI_SPI_HEADER_LEN] = {DEFY_HCI_SPI_READ};

    k_mutex_lock(&radio.lock, K_FOREVER);
    arm();
    take(read, sizeof(read));
    arm();
    k_mutex_unlock(&radio.lock);
}

void fake_radio_power_off_pending(void) {
    k_mutex_lock(&radio.lock, K_FOREVER);
    power_off_requested();
    arm();
    update_data_ready();
    k_mutex_unlock(&radio.lock);
}

void fake_radio_kill(void) {
    k_mutex_lock(&radio.lock, K_FOREVER);
    clear();
    radio.state = FAKE_RADIO_SILENT;
    update_data_ready();
    k_mutex_unlock(&radio.lock);
}

void fake_radio_reject_hellos(unsigned int count) { options.reject_hellos = count; }

void fake_radio_reject_command(uint8_t command) { options.reject_command = command; }

void fake_radio_drop_receive_request(uint8_t command) { options.drop_receive_for = command; }

void fake_radio_ignore_until_next_begin(uint8_t command) { options.ignore_receive_for = command; }

void fake_radio_foreign_bootloader(bool foreign) { options.foreign = foreign; }

void fake_radio_remove_s113(void) { options.s113 = false; }

void fake_radio_erase_seal(void) {
    k_mutex_lock(&radio.lock, K_FOREVER);
    memset(radio.seal, 0xff, sizeof(radio.seal));
    memset(radio.slot, 0xff, 64);
    k_mutex_unlock(&radio.lock);
}

enum fake_radio_state fake_radio_state(void) {
    k_mutex_lock(&radio.lock, K_FOREVER);
    enum fake_radio_state state = radio.state;
    k_mutex_unlock(&radio.lock);
    return state;
}

struct fake_radio_counts fake_radio_counts(void) {
    k_mutex_lock(&radio.lock, K_FOREVER);
    struct fake_radio_counts counts = radio.counts;
    k_mutex_unlock(&radio.lock);
    return counts;
}

/* Before the driver's look at power-on, which runs at APPLICATION 1. */
static int init_data_ready(void) { return gpio_pin_configure_dt(&data_ready, GPIO_INPUT); }

SYS_INIT(init_data_ready, APPLICATION, 0);
