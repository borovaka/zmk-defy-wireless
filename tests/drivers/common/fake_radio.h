/*
 * The nRF52820 behind the test SPI controller in test_bus.c, with data ready on
 * an emulated GPIO. It runs this port's controller, through apps/defy_radio's
 * link code with a stub controller and the radio's power states, or Dygma's
 * rf_bootloader, which listens for 300 ms after every reset unless B holds it,
 * or Dygma's radio application.
 *
 * fake_radio_reset() starts it as the controller alone: a reset boots the
 * controller at once. fake_radio_reset_stock() and fake_radio_reset_custom()
 * start it with firmware in rf_bootloader's slot, and every reset passes
 * through rf_bootloader, as on the keyboard.
 *
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

enum fake_radio_state {
    /* Never answers: no radio, or S113 asleep over erased vectors until power-on. */
    FAKE_RADIO_SILENT,
    /* In System OFF. Any transfer wakes it, as chip select does. */
    FAKE_RADIO_OFF,
    /* Booting the controller: not answering until it is up. */
    FAKE_RADIO_BOOTING,
    /* Up, with its ready event not read yet. */
    FAKE_RADIO_BOOTED,
    /* Up long ago, its ready event read by an earlier start. */
    FAKE_RADIO_RUNNING,
    FAKE_RADIO_STOPPING,
    FAKE_RADIO_BOOTLOADER,
    /* Dygma's radio application. */
    FAKE_RADIO_STOCK,
};

enum fake_radio_rx_fault {
    FAKE_RX_NONE,
    FAKE_RX_SILENT,
    FAKE_RX_EMPTY,
    FAKE_RX_INVALID,
    FAKE_RX_ERROR,
};

struct fake_radio_counts {
    unsigned int transfers;
    /* The controller. */
    unsigned int wakes;
    unsigned int boots;
    unsigned int reboot_commands;
    unsigned int power_off_commands;
    unsigned int refresh_commands;
    unsigned int commands;
    unsigned int transfers_after_power_off_reply;
    uint16_t last_opcode;
    /* rf_bootloader and Dygma's framing. */
    unsigned int bootloader_entries;
    unsigned int windows_expired;
    unsigned int erases;
    unsigned int writes;
    unsigned int seals;
    unsigned int finishes;
    unsigned int begins;
    unsigned int infos;
    unsigned int short_frames;
    unsigned int truncated_replies;
    unsigned int rejected_commands;
    unsigned int rejected_hellos;
    unsigned int dropped_receive_requests;
    unsigned int ignored_receive_requests;
};

/* The controller alone, in state, with no messages, the default options and zero counts. */
void fake_radio_reset(enum fake_radio_state state);

/* Dygma's application in the slot, its seal stored, and running. */
void fake_radio_reset_stock(void);

#ifdef CONFIG_DEFY_RADIO_INSTALL
/* This build's firmware in the slot, its seal stored, and running, its ready event long read. */
void fake_radio_reset_installed(void);

/* This build's firmware sealed as version, running, with its ready event queued or read. */
void fake_radio_reset_custom(uint32_t version, bool ready);

/* Whether the slot and seal hold exactly this build's radio firmware. */
bool fake_radio_holds_image(void);
#endif

/* Power-on: rf_bootloader starts listening. bootloader_entries counts only later entries. */
void fake_radio_power_on(void);

/* How long booting the controller takes. 30 ms by default. */
void fake_radio_set_boot_ms(int ms);
void fake_radio_set_power_off_ms(int ms);
void fake_radio_set_power_off_reply(int status);
void fake_radio_set_power_off_timing(int reply_ms, int timeout_ms, int read_delay_ms);
void fake_radio_fail_next_transfer(void);
void fake_radio_block_next_transfer(void);
int fake_radio_wait_blocked(void);
void fake_radio_unblock_transfer(void);
void fake_radio_rx_fault(enum fake_radio_rx_fault fault);

/* Whether the controller ignores the reboot command. */
void fake_radio_ignore_reboot(bool ignore);

/* Whether a reset, such as the controller's reboot command, skips rf_bootloader. */
void fake_radio_skip_bootloader(bool skip);

/* The next n transfers find the controller unarmed, as when it is slow to arm. */
void fake_radio_miss_transfers(int n);

/* Queues a message for the host, with its H4 packet type, and raises data ready. */
void fake_radio_send(const uint8_t *msg, size_t len);

/* Puts it part-way through a read: the host's header taken, the oldest message armed. */
void fake_radio_start_read(void);

/* As if it has just taken the power-off command: the reply queued, System OFF once it is read. */
void fake_radio_power_off_pending(void);

/* Stops it answering, as a crash would. */
void fake_radio_kill(void);

/* rf_bootloader answers the next count hellos with 0x81, so a session fails to begin. */
void fake_radio_reject_hellos(unsigned int count);
void fake_radio_reject_command(uint8_t command);
void fake_radio_drop_receive_request(uint8_t command);

/* Ignores the command's reply requests until the next session's B, so this session times out. */
void fake_radio_ignore_until_next_begin(uint8_t command);

/* A bootloader whose INFO is not Dygma's: the driver must leave it alone. */
void fake_radio_foreign_bootloader(bool foreign);

/* Without S113's identification, the bootloader reports an application start of 0x1000. */
void fake_radio_remove_s113(void);

/* The seal page erased, as an install interrupted after its first erase leaves it. */
void fake_radio_erase_seal(void);

enum fake_radio_state fake_radio_state(void);
struct fake_radio_counts fake_radio_counts(void);

/* For test_bus.c: chip select falls or rises. */
void fake_radio_select(bool selected);

/* For test_bus.c: clocks length bytes with chip select low. Returns -EIO for a failed transfer. */
int fake_radio_clock(const uint8_t *tx, uint8_t *rx, size_t length);
