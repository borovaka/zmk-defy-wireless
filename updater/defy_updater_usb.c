/* SPDX-License-Identifier: MIT */

#include <errno.h>
#include <string.h>

#include <zephyr/drivers/uart.h>
#include <zephyr/init.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/atomic.h>
#include <zephyr/sys/ring_buffer.h>
#include <zmk/usb.h>
#include <hal/nrf_power.h>

#include <dygma/defy_half_bootloader.h>
#include <dygma/defy_half_version.h>
#include <dygma/defy_hybrid.h>
#include <dygma/defy_keymap_changes.h>
#include <dygma/defy_neuron_cables.h>
#include <dygma/defy_split_pairing.h>
#include <dygma/defy_updater.h>
#include <dygma/defy_update_guard.h>
#include <dygma/defy_battery_central.h>
#if IS_ENABLED(CONFIG_DEFY_NEURON_DOCK)
#include <dygma/defy_neuron_dock.h>
#endif
#if IS_ENABLED(CONFIG_DEFY_KEY_TRACE)
#include <dygma/defy_key_trace.h>
#endif

/* GPREGRET2 survives every Neuron reset but power-on and brownout, and neither the UF2 loader nor
 * n2_bootloader uses it. Zephyr's GPREGRET driver, which ZMK's boot mode needs for GPREGRET, must
 * have no device for it. */
#define HALVES_HELD_REGISTER 1
#define HALVES_HELD_MARKER 0xd5
BUILD_ASSERT(!IS_ENABLED(CONFIG_RETAINED_MEM_NRF_GPREGRET) ||
                 !DT_NODE_HAS_STATUS_OKAY(DT_NODELABEL(gpregret2)),
             "GPREGRET2 holds the halves' marker; disable the gpregret2 node");

/* After this many failed releases in a row, typing resumes without them. */
#define HALF_RELEASE_ATTEMPTS 5

/* A reply has this long to reach the UART. */
#define REPLY_TIMEOUT_MS 1000
/* A partial frame is dropped after this long without a byte. */
#define FRAME_GAP_MS 1000
/* DTR changes raise no event. The thread polls the line this often while a disconnect would end
 * a session, drop a partial frame or clear the reply cache. */
#define LINE_POLL_MS 5

static const struct device *const updater_uart =
    DEVICE_DT_GET(DT_CHOSEN(dygma_half_updater_uart));
static struct defy_half_bootloader loader;
static struct defy_half_bootloader_info loader_info[DEFY_HALF_BOOTLOADER_SIDES];
static struct defy_updater state;
static bool startup_recovery;
static unsigned int release_failures;
static struct k_thread updater_thread;
static K_THREAD_STACK_DEFINE(updater_stack, 2048);

/* The UART callback fills the RX ring and empties the TX ring; each holds a whole frame. */
static uint8_t rx_buffer[DEFY_UPDATER_FRAME];
static uint8_t tx_buffer[DEFY_UPDATER_FRAME];
static struct ring_buf rx_ring;
static struct ring_buf tx_ring;
/* Given when the callback has added to the RX ring or paused RX. */
static K_SEM_DEFINE(rx_ready, 0, 1);
/* Given when the callback has emptied the TX ring or a fill failed, with the error in tx_error. */
static K_SEM_DEFINE(tx_done, 0, 1);
static int tx_error;
/* Keeps the callback's fills apart from send_reply discarding a failed reply. */
static struct k_spinlock tx_lock;
/* Set while RX is off because the RX ring was full. */
static atomic_t rx_paused;

static bool halves_held(void) {
    return nrf_power_gpregret_get(NRF_POWER, HALVES_HELD_REGISTER) == HALVES_HELD_MARKER;
}

static void mark_halves_held(bool held) {
    nrf_power_gpregret_set(NRF_POWER, HALVES_HELD_REGISTER, held ? HALVES_HELD_MARKER : 0);
}

static int release_halves(void) {
    int err = startup_recovery ? defy_half_bootloader_recover(&loader)
                               : defy_half_bootloader_abort(&loader);
    if (!err) {
        mark_halves_held(false);
        release_failures = 0;
    } else if (++release_failures < HALF_RELEASE_ATTEMPTS) {
        return err;
    }
    startup_recovery = false;
    return 0;
}

static int close_session(void) {
    if (startup_recovery) {
        int err = defy_hybrid_central_set_maintenance(true);
        if (err) {
            return err;
        }
    }
    int err = release_halves();
    return err ? err : defy_hybrid_central_set_maintenance(false);
}

static int open_session(void) {
    int err = defy_hybrid_central_set_maintenance(true);
    if (err) {
        return err;
    }
    mark_halves_held(true);
    release_failures = 0;
    err = defy_half_bootloader_reset_all(&loader);
    for (uint8_t side = 0; !err && side < DEFY_HALF_BOOTLOADER_SIDES; side++) {
        err = defy_half_bootloader_begin(&loader, side, &loader_info[side]);
    }
    return err;
}

static void copy_seal(struct defy_updater_info *info, const struct defy_half_seal *seal) {
    info->valid = true;
    info->version = seal->program_version;
    info->crc = seal->program_crc;
}

static int probe_half(uint8_t side, struct defy_updater_info *info) {
    info->hardware = loader_info[side].hardware_version;
    info->flash_start = loader_info[side].flash_start;
    struct defy_half_seal seal;
    int err = defy_half_bootloader_read_seal(&loader, side, &seal);
    if (err == -EBADMSG) {
        info->valid = false;
        return 0;
    }
    if (!err) {
        copy_seal(info, &seal);
    }
    return err;
}

static int write_page(uint8_t side, uint32_t address, const uint8_t *data) {
    int err = 0;
    if (address % DEFY_HALF_SECTOR_SIZE == 0) {
        err = defy_half_bootloader_erase_sector(&loader, side, address);
    }
    return err ? err : defy_half_bootloader_write_page(&loader, side, address, data);
}

static int validate_half(uint8_t side, struct defy_updater_info *info) {
    struct defy_half_seal seal;
    int err = defy_half_bootloader_validate(&loader, side, &seal);
    if (!err) {
        copy_seal(info, &seal);
    }
    return err;
}

/* The Neuron's side first, so that a failure partway leaves the Neuron unpaired, which PAIRING
 * then reports. Then each half's settings, left first. */
static int reset_pairing(void) {
    int err = defy_split_pairing_clear();
    for (uint8_t side = 0; !err && side < DEFY_HALF_BOOTLOADER_SIDES; side++) {
        err = defy_half_bootloader_erase_settings(&loader, side);
    }
    return err;
}

static const struct defy_updater_ops operations = {
    .open = open_session,
    .probe = probe_half,
    .page = write_page,
    .finish = validate_half,
    .close = close_session,
    .diagnostics = defy_hybrid_ble_diagnostics,
    .pairing_status = defy_split_pairing_status,
    .reset_pairing = reset_pairing,
#if IS_ENABLED(CONFIG_DEFY_HALF_VERSION)
    .half_versions = defy_half_versions,
#endif
#if IS_ENABLED(CONFIG_ZMK_KEYMAP_SETTINGS_STORAGE)
    .clear_keymap_changes = defy_keymap_changes_clear,
#endif
#if IS_ENABLED(CONFIG_DEFY_BATTERY)
    .battery_status = defy_battery_central_snapshot,
#endif
#if IS_ENABLED(CONFIG_DEFY_NEURON_DOCK)
    .dock_status = defy_neuron_dock_status,
#endif
#if IS_ENABLED(CONFIG_DEFY_KEY_TRACE)
    .key_trace = defy_key_trace_read,
#endif
};

static bool connected(void) {
    uint32_t dtr = 0;
    return zmk_usb_is_hid_ready() && zmk_usb_get_status() != USB_DC_SUSPEND &&
           uart_line_ctrl_get(updater_uart, UART_LINE_CTRL_DTR, &dtr) == 0 && dtr != 0;
}

static void receive(const struct device *dev) {
    uint8_t *data;
    uint32_t space = ring_buf_put_claim(&rx_ring, &data, sizeof(rx_buffer));
    if (space) {
        int count = uart_fifo_read(dev, data, space);
        ring_buf_put_finish(&rx_ring, MAX(count, 0));
    } else {
        /* The thread turns RX back on once it has read the ring empty. */
        uart_irq_rx_disable(dev);
        atomic_set(&rx_paused, 1);
    }
    k_sem_give(&rx_ready);
}

static void transmit(const struct device *dev) {
    k_spinlock_key_t key = k_spin_lock(&tx_lock);
    uint8_t *data;
    uint32_t length = ring_buf_get_claim(&tx_ring, &data, sizeof(tx_buffer));
    int sent = length ? uart_fifo_fill(dev, data, length) : 0;
    ring_buf_get_finish(&tx_ring, MAX(sent, 0));
    if (sent < 0 || ring_buf_is_empty(&tx_ring)) {
        tx_error = MIN(sent, 0);
        uart_irq_tx_disable(dev);
        k_sem_give(&tx_done);
    }
    k_spin_unlock(&tx_lock, key);
}

/* The UART's interrupt callback, the only place uart.h allows the FIFO calls. */
static void serial_callback(const struct device *dev, void *user_data) {
    ARG_UNUSED(user_data);
    while (uart_irq_update(dev) && uart_irq_is_pending(dev)) {
        if (uart_irq_rx_ready(dev) > 0) {
            receive(dev);
        }
        if (uart_irq_tx_ready(dev) > 0) {
            transmit(dev);
        }
    }
}

static int serial_init(void) {
    ring_buf_init(&rx_ring, sizeof(rx_buffer), rx_buffer);
    ring_buf_init(&tx_ring, sizeof(tx_buffer), tx_buffer);
    int err = uart_irq_callback_set(updater_uart, serial_callback);
    if (!err) {
        uart_irq_rx_enable(updater_uart);
    }
    return err;
}

/* Hands the reply to the UART. Fails with -ETIMEDOUT if the host disconnects or the time runs out
 * first, or with the UART's error. A failed reply leaves nothing queued. */
static int send_reply(const uint8_t *reply, size_t length) {
    int64_t deadline = k_uptime_get() + REPLY_TIMEOUT_MS;
    int err;
    tx_error = 0;
    k_sem_reset(&tx_done);
    while (true) {
        if (!connected() || k_uptime_get() >= deadline) {
            err = -ETIMEDOUT;
            break;
        }
        if (length) {
            uint32_t queued = ring_buf_put(&tx_ring, reply, length);
            reply += queued;
            length -= queued;
            uart_irq_tx_enable(updater_uart);
        }
        if (k_sem_take(&tx_done, K_MSEC(1)) == 0 && (tx_error || !length)) {
            err = tx_error;
            break;
        }
    }
    if (err) {
        k_spinlock_key_t key = k_spin_lock(&tx_lock);
        uart_irq_tx_disable(updater_uart);
        ring_buf_reset(&tx_ring);
        k_spin_unlock(&tx_lock, key);
    }
    return err;
}

/* How long the thread can wait for input. The line poll also times the idle and partial-frame
 * limits. A failed release is retried at its own time, which the next defy_updater_expire sets
 * when retry_at is negative. With nothing pending the thread sleeps until input arrives. */
static k_timeout_t input_timeout(int64_t now, bool live, bool partial) {
    if (live && (state.active || partial || state.cached_reply_length)) {
        return K_MSEC(LINE_POLL_MS);
    }
    if (state.closing) {
        return state.retry_at < 0 ? K_NO_WAIT : K_MSEC(state.retry_at - now);
    }
    return K_FOREVER;
}

static void run_updater(void *p1, void *p2, void *p3) {
    ARG_UNUSED(p1);
    ARG_UNUSED(p2);
    ARG_UNUSED(p3);
    if (halves_held()) {
        while (!defy_update_claim(DEFY_UPDATE_HALVES)) {
            k_sleep(K_MSEC(100));
        }
        startup_recovery = true;
        defy_updater_recover(&state);
    }
    static uint8_t request[DEFY_UPDATER_FRAME];
    static uint8_t reply[DEFY_UPDATER_FRAME];
    size_t used = 0;
    size_t wanted = 0;
    int64_t last_byte = 0;
    while (true) {
        int64_t now = k_uptime_get();
        bool live = connected();
        defy_updater_expire(&state, now, live);
        if (!live || (used && now - last_byte >= FRAME_GAP_MS)) {
            used = 0;
            wanted = 0;
        }
        uint8_t byte;
        if (ring_buf_get(&rx_ring, &byte, 1) != 1) {
            if (atomic_clear(&rx_paused)) {
                /*
                 * While RX was off, the rest of the input stayed in the port's own
                 * buffer, which a USB reset leaves as it is. With the host gone, it
                 * belongs to no session: drop it. RX is still off, so only this
                 * thread reads the port.
                 */
                while (!live && uart_poll_in(updater_uart, &byte) == 0) {
                }
                uart_irq_rx_enable(updater_uart);
            }
            k_sem_take(&rx_ready, input_timeout(now, live, used));
            continue;
        }
        if (!live) {
            continue;
        }
        last_byte = now;
        if (used == 0 && byte != 'D') {
            continue;
        }
        if (used == 1 && byte != 'U') {
            used = byte == 'D' ? 1 : 0;
            continue;
        }
        request[used++] = byte;
        if (used == DEFY_UPDATER_HEADER) {
            wanted = defy_updater_frame_length(request);
            if (!wanted) {
                used = 0;
                continue;
            }
        }
        if (wanted && used == wanted) {
            size_t length = defy_updater_process(&state, request, used, reply, now);
            used = 0;
            wanted = 0;
            if (length && send_reply(reply, length)) {
                defy_updater_expire(&state, k_uptime_get(), false);
            }
        }
    }
}

static int updater_init(void) {
    if (!device_is_ready(updater_uart)) {
        return -ENODEV;
    }
    const struct defy_half_bootloader_config config = {
        .bus = DEVICE_DT_GET(DT_PHANDLE(DEFY_NEURON_CABLES_NODE, i2c_bus)),
        .reset = DEFY_NEURON_RUN_LINES,
    };
    int err = defy_half_bootloader_init(&loader, &config);
    if (err) {
        return err;
    }
    defy_updater_init(&state, &operations);
    err = serial_init();
    if (err) {
        return err;
    }
    k_thread_create(&updater_thread, updater_stack, K_THREAD_STACK_SIZEOF(updater_stack),
                    run_updater, NULL, NULL, NULL, K_PRIO_PREEMPT(10), 0, K_NO_WAIT);
    k_thread_name_set(&updater_thread, "defy_updater");
    return 0;
}

/* After the split transports have started, since a restart's recovery pauses them. */
SYS_INIT(updater_init, APPLICATION, 99);
