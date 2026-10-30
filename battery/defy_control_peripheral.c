/*
 * A half's end of the Neuron's control channel: its GATT service, with the
 * battery frame characteristic, the command characteristic and the version
 * report's, as dygma/defy_control.h describes them. The battery frames the
 * cable sends come from here too, under the same session. Commands are
 * followed on the system work queue, outside Bluetooth's callbacks; a
 * disconnection drops those not yet followed. A lighting group is kept at
 * once, so that the half holds it before a state that follows it is applied.
 *
 * SPDX-License-Identifier: MIT
 */

#include <errno.h>
#include <string.h>

#include <zephyr/bluetooth/conn.h>
#include <zephyr/bluetooth/gatt.h>
#include <zephyr/kernel.h>
#include <zephyr/random/random.h>

#include <dygma/defy_battery.h>
#include <dygma/defy_battery_hci.h>
#include <dygma/defy_control.h>
#include <dygma/defy_feedback.h>
#include <dygma/defy_half.h>
#include <dygma/defy_half_leds.h>
#include <dygma/defy_half_power.h>
#include <dygma/defy_half_version.h>
#include <dygma/defy_lighting.h>

/* Guards the session, the connection epoch and every command waiting for the work queue. */
static struct k_spinlock control_lock;
/* A random number for this start, once Bluetooth can give one: the wake
 * report's boot, and the battery frames' session, which a radio restart
 * advances. */
static uint32_t session;
static bool nonce_ready;
/* Counts disconnections, so that work for an earlier connection drops its command. */
static uint32_t connection_epoch;
#if IS_ENABLED(CONFIG_DEFY_BATTERY)
static uint32_t generation;
static uint8_t read_frame[DEFY_BATTERY_FRAME_SIZE];
static bool read_available;
static bool show_pending;
static uint32_t show_epoch;
static int64_t show_deadline;
#endif
#if IS_ENABLED(CONFIG_DEFY_LIGHTING)
static struct defy_host_feedback feedback_requested;
static uint32_t feedback_epoch;
static int64_t feedback_deadline;
static bool feedback_pending;
static uint32_t wake_boot;
static struct defy_lighting_state lighting_requested;
static uint32_t lighting_epoch;
static int64_t lighting_deadline;
static bool lighting_pending;
#endif
BUILD_ASSERT(CONFIG_BT_MAX_CONN == 1, "the epoch and the pending commands are for one connection");

static void initialize_nonce(void) {
    uint32_t candidate = sys_rand32_get();
    k_spinlock_key_t key = k_spin_lock(&control_lock);
    session = candidate != 0 ? candidate : 1;
#if IS_ENABLED(CONFIG_DEFY_LIGHTING)
    wake_boot = session;
#endif
    nonce_ready = true;
    k_spin_unlock(&control_lock, key);
}

/* sys_rand32_get() asks the radio over HCI, so the nonce waits for Bluetooth in a thread. */
static void nonce_thread(void *unused1, void *unused2, void *unused3) {
    ARG_UNUSED(unused1);
    ARG_UNUSED(unused2);
    ARG_UNUSED(unused3);
    while (!bt_is_ready()) {
        k_sleep(K_MSEC(1000));
    }
    initialize_nonce();
}

K_THREAD_DEFINE(defy_control_nonce, 1024, nonce_thread, NULL, NULL, NULL, K_PRIO_PREEMPT(8), 0,
                0);

#if IS_ENABLED(CONFIG_DEFY_BATTERY)
int defy_battery_local_frame(struct defy_battery_frame *frame) {
    if (frame == NULL) {
        return -EINVAL;
    }
    struct defy_battery_sample sample;
    uint32_t current_generation;
    int err = defy_battery_hci_get_generation(&sample, &current_generation);
    if (err < 0) {
        return err;
    }
    k_spinlock_key_t key = k_spin_lock(&control_lock);
    if (!nonce_ready ||
        (generation != 0 && (int32_t)(current_generation - generation) < 0)) {
        k_spin_unlock(&control_lock, key);
        return -EAGAIN;
    }
    if (generation != current_generation) {
        if (generation != 0 && ++session == 0) {
            session = 1;
        }
        generation = current_generation;
    }
    *frame = (struct defy_battery_frame){
        .side = DEFY_HALF_ID, .session = session, .sample = sample,
    };
    k_spin_unlock(&control_lock, key);
    return 0;
}

static ssize_t read_battery(struct bt_conn *conn, const struct bt_gatt_attr *attr,
                             void *buf, uint16_t len, uint16_t offset) {
    uint8_t data[DEFY_BATTERY_FRAME_SIZE];
    k_spinlock_key_t key = k_spin_lock(&control_lock);
    uint32_t expected_epoch = connection_epoch;
    k_spin_unlock(&control_lock, key);
    if (offset == 0) {
        struct defy_battery_frame frame;
        if (defy_battery_local_frame(&frame) < 0 ||
            defy_battery_frame_encode(&frame, data) < 0) {
            key = k_spin_lock(&control_lock);
            if (connection_epoch == expected_epoch) {
                read_available = false;
            }
            k_spin_unlock(&control_lock, key);
            return BT_GATT_ERR(BT_ATT_ERR_UNLIKELY);
        }
    }
    key = k_spin_lock(&control_lock);
    if (connection_epoch != expected_epoch) {
        k_spin_unlock(&control_lock, key);
        return BT_GATT_ERR(BT_ATT_ERR_UNLIKELY);
    }
    if (offset == 0) {
        memcpy(read_frame, data, sizeof(data));
        read_available = true;
    }
    if (!read_available) {
        k_spin_unlock(&control_lock, key);
        return BT_GATT_ERR(BT_ATT_ERR_UNLIKELY);
    }
    memcpy(data, read_frame, sizeof(data));
    k_spin_unlock(&control_lock, key);
    return bt_gatt_attr_read(conn, attr, buf, len, offset, data, sizeof(data));
}

static void show_battery_work(struct k_work *work) {
    ARG_UNUSED(work);
    k_spinlock_key_t key = k_spin_lock(&control_lock);
    bool show = show_pending && show_epoch == connection_epoch && k_uptime_get() < show_deadline;
    show_pending = false;
    k_spin_unlock(&control_lock, key);
    if (show) {
        defy_half_leds_battery_show();
    }
}

static K_WORK_DEFINE(show_battery, show_battery_work);

/* The battery display: the header alone. */
static ssize_t write_battery_show(const uint8_t *command, uint16_t len) {
    if (len != DEFY_CONTROL_BATTERY_SHOW_SIZE) {
        return BT_GATT_ERR(BT_ATT_ERR_INVALID_ATTRIBUTE_LEN);
    }
    if (command[0] != DEFY_CONTROL_BATTERY_SHOW_VERSION) {
        return BT_GATT_ERR(BT_ATT_ERR_VALUE_NOT_ALLOWED);
    }
    k_spinlock_key_t key = k_spin_lock(&control_lock);
    show_pending = true;
    show_epoch = connection_epoch;
    show_deadline = k_uptime_get() + DEFY_CONTROL_TIMEOUT_MS;
    k_spin_unlock(&control_lock, key);
    k_work_submit(&show_battery);
    return len;
}

#define BATTERY_CHARACTERISTIC                                                                     \
    BT_GATT_CHARACTERISTIC(BT_UUID_DECLARE_128(DEFY_CONTROL_BATTERY_UUID), BT_GATT_CHRC_READ,     \
                           BT_GATT_PERM_READ_ENCRYPT, read_battery, NULL, NULL),
#else
#define BATTERY_CHARACTERISTIC
#endif

#if IS_ENABLED(CONFIG_DEFY_LIGHTING)
/* The lighting's Bluetooth epoch for a connection: never 0, which means no link. */
static uint32_t lighting_epoch_of(uint32_t connection) {
    return connection + 1 ? connection + 1 : 1;
}

static void show_feedback_work(struct k_work *work) {
    ARG_UNUSED(work);
    k_spinlock_key_t key = k_spin_lock(&control_lock);
    int64_t remaining = feedback_deadline - k_uptime_get();
    bool apply = feedback_pending && feedback_epoch == connection_epoch && remaining > 0;
    struct defy_host_feedback state = feedback_requested;
    feedback_pending = false;
    k_spin_unlock(&control_lock, key);
    if (apply) {
        state.remaining_ms = MIN(remaining, DEFY_HOST_FEEDBACK_DURATION_MS);
        defy_half_leds_host_show(&state);
    }
}

static K_WORK_DEFINE(show_feedback, show_feedback_work);

static ssize_t write_feedback(const uint8_t *command, uint16_t len) {
    struct defy_host_feedback state;
    if (len != DEFY_HOST_FEEDBACK_SIZE) {
        return BT_GATT_ERR(BT_ATT_ERR_INVALID_ATTRIBUTE_LEN);
    }
    if (defy_host_feedback_decode(command, len, &state)) {
        return BT_GATT_ERR(BT_ATT_ERR_VALUE_NOT_ALLOWED);
    }
    k_spinlock_key_t key = k_spin_lock(&control_lock);
    if (feedback_pending && feedback_epoch == connection_epoch &&
        feedback_requested.boot == state.boot &&
        (int32_t)(state.sequence - feedback_requested.sequence) <= 0) {
        k_spin_unlock(&control_lock, key);
        return len;
    }
    feedback_requested = state;
    feedback_epoch = connection_epoch;
    feedback_deadline = k_uptime_get() + state.remaining_ms;
    feedback_pending = true;
    k_spin_unlock(&control_lock, key);
    k_work_submit(&show_feedback);
    return len;
}

static void apply_lighting_work(struct k_work *work) {
    ARG_UNUSED(work);
    k_spinlock_key_t key = k_spin_lock(&control_lock);
    bool apply = lighting_pending && lighting_epoch == connection_epoch &&
                 k_uptime_get() < lighting_deadline;
    struct defy_lighting_state state = lighting_requested;
    uint32_t epoch = lighting_epoch_of(lighting_epoch);
    lighting_pending = false;
    k_spin_unlock(&control_lock, key);
    /* A state the half refuses, such as one naming a group it lacks, leaves the
     * read returning the state before, so the Neuron sends it again. */
    if (apply) {
        defy_lighting_peripheral_apply(&state, DEFY_LINK_BLE, epoch);
    }
}

static K_WORK_DEFINE(apply_lighting, apply_lighting_work);

/*
 * A lighting state older than the one pending is dropped. One as new replaces
 * it and renews its deadline, where the cable keeps the pending one; see
 * receive_commands() in drivers/split/defy_wired_peripheral.c.
 */
static ssize_t write_lighting(const uint8_t *command, uint16_t len) {
    struct defy_lighting_state state;
    if (len != DEFY_LIGHTING_CONTROL_SIZE) {
        return BT_GATT_ERR(BT_ATT_ERR_INVALID_ATTRIBUTE_LEN);
    }
    if (defy_lighting_control_decode(command, len, DEFY_CONTROL_LIGHTING_SET, &state) < 0) {
        return BT_GATT_ERR(BT_ATT_ERR_VALUE_NOT_ALLOWED);
    }
    k_spinlock_key_t key = k_spin_lock(&control_lock);
    if (lighting_pending && lighting_epoch == connection_epoch &&
        lighting_requested.boot == state.boot &&
        (int32_t)(state.revision - lighting_requested.revision) < 0) {
        k_spin_unlock(&control_lock, key);
        return len;
    }
    lighting_requested = state;
    lighting_epoch = connection_epoch;
    lighting_deadline = k_uptime_get() + DEFY_CONTROL_TIMEOUT_MS;
    lighting_pending = true;
    k_spin_unlock(&control_lock, key);
    k_work_submit(&apply_lighting);
    return len;
}

/* A group, kept at once for the states that follow it. Nothing answers it. */
static ssize_t write_lighting_group(const uint8_t *command, uint16_t len) {
    struct defy_lighting_group group;
    if (len != DEFY_LIGHTING_GROUP_SIZE) {
        return BT_GATT_ERR(BT_ATT_ERR_INVALID_ATTRIBUTE_LEN);
    }
    if (defy_lighting_group_decode(command, len, &group) < 0) {
        return BT_GATT_ERR(BT_ATT_ERR_VALUE_NOT_ALLOWED);
    }
    defy_lighting_peripheral_group(&group);
    return len;
}

/* The lighting state as applied, or the wake report before there is one. */
static ssize_t read_lighting(struct bt_conn *conn, const struct bt_gatt_attr *attr,
                              void *buf, uint16_t len, uint16_t offset) {
    struct defy_lighting_state state;
    uint8_t data[DEFY_LIGHTING_READ_SIZE];
    size_t size = DEFY_LIGHTING_CONTROL_SIZE;
    if (defy_lighting_peripheral_get(&state)) {
        if (defy_lighting_control_encode(&state, DEFY_CONTROL_LIGHTING_ACK, data) < 0) {
            return BT_GATT_ERR(BT_ATT_ERR_UNLIKELY);
        }
    } else {
        k_spinlock_key_t key = k_spin_lock(&control_lock);
        bool ready = nonce_ready;
        uint32_t boot = wake_boot;
        k_spin_unlock(&control_lock, key);
        struct defy_lighting_wake wake = {
            .side = DEFY_HALF_ID,
            .from_sleep = defy_half_power_woke_from_key(),
            .boot = boot,
            .age_ms = MIN((uint64_t)UINT32_MAX, (uint64_t)k_uptime_get()),
        };
        if (!ready || defy_lighting_wake_encode(&wake, data) < 0) {
            return BT_GATT_ERR(BT_ATT_ERR_UNLIKELY);
        }
        size = DEFY_LIGHTING_WAKE_SIZE;
    }
    return bt_gatt_attr_read(conn, attr, buf, len, offset, data, size);
}
#endif

/* Every command starts with its version and opcode; each takes its own length. */
static ssize_t write_control(struct bt_conn *conn, const struct bt_gatt_attr *attr,
                              const void *buf, uint16_t len, uint16_t offset, uint8_t flags) {
    ARG_UNUSED(conn);
    ARG_UNUSED(attr);
    if (offset) {
        return BT_GATT_ERR(BT_ATT_ERR_INVALID_OFFSET);
    }
    if (flags != BT_GATT_WRITE_FLAG_CMD) {
        return BT_GATT_ERR(BT_ATT_ERR_NOT_SUPPORTED);
    }
    if (len < DEFY_CONTROL_HEADER_SIZE) {
        return BT_GATT_ERR(BT_ATT_ERR_INVALID_ATTRIBUTE_LEN);
    }
    const uint8_t *command = buf;
    switch (command[1]) {
#if IS_ENABLED(CONFIG_DEFY_BATTERY)
    case DEFY_CONTROL_BATTERY_SHOW:
        return write_battery_show(command, len);
#endif
#if IS_ENABLED(CONFIG_DEFY_LIGHTING)
    case DEFY_CONTROL_LIGHTING_SET:
        return write_lighting(command, len);
    case DEFY_CONTROL_LIGHTING_GROUP:
        return write_lighting_group(command, len);
    case DEFY_CONTROL_HOST_FEEDBACK:
        return write_feedback(command, len);
#endif
    default:
        return BT_GATT_ERR(BT_ATT_ERR_VALUE_NOT_ALLOWED);
    }
}

static void disconnected(struct bt_conn *conn, uint8_t reason) {
    ARG_UNUSED(conn);
    ARG_UNUSED(reason);
    k_spinlock_key_t key = k_spin_lock(&control_lock);
    connection_epoch++;
#if IS_ENABLED(CONFIG_DEFY_BATTERY)
    read_available = false;
    show_pending = false;
#endif
#if IS_ENABLED(CONFIG_DEFY_LIGHTING)
    feedback_pending = false;
    lighting_pending = false;
#endif
    k_spin_unlock(&control_lock, key);
#if IS_ENABLED(CONFIG_DEFY_LIGHTING)
    defy_lighting_peripheral_ble_epoch(0);
#endif
}

#if IS_ENABLED(CONFIG_DEFY_LIGHTING)
static void lighting_connected(struct bt_conn *conn, uint8_t error) {
    ARG_UNUSED(conn);
    if (!error) {
        k_spinlock_key_t key = k_spin_lock(&control_lock);
        uint32_t epoch = lighting_epoch_of(connection_epoch);
        k_spin_unlock(&control_lock, key);
        defy_lighting_peripheral_ble_epoch(epoch);
    }
}
#endif

BT_CONN_CB_DEFINE(control_connection) = {
    .disconnected = disconnected,
#if IS_ENABLED(CONFIG_DEFY_LIGHTING)
    .connected = lighting_connected,
#endif
};

#if IS_ENABLED(CONFIG_DEFY_LIGHTING)
#define COMMAND_CHARACTERISTIC                                                                     \
    BT_GATT_CHARACTERISTIC(BT_UUID_DECLARE_128(DEFY_CONTROL_COMMAND_UUID),                        \
                           BT_GATT_CHRC_WRITE_WITHOUT_RESP | BT_GATT_CHRC_READ,                   \
                           BT_GATT_PERM_WRITE_ENCRYPT | BT_GATT_PERM_READ_ENCRYPT,                \
                           read_lighting, write_control, NULL),
#else
#define COMMAND_CHARACTERISTIC                                                                     \
    BT_GATT_CHARACTERISTIC(BT_UUID_DECLARE_128(DEFY_CONTROL_COMMAND_UUID),                        \
                           BT_GATT_CHRC_WRITE_WITHOUT_RESP, BT_GATT_PERM_WRITE_ENCRYPT, NULL,     \
                           write_control, NULL),
#endif

#if IS_ENABLED(CONFIG_DEFY_HALF_VERSION)
/* The half's version report. It stays the same while the half runs, so a long
 * read needs no snapshot. */
static ssize_t read_version(struct bt_conn *conn, const struct bt_gatt_attr *attr,
                            void *buf, uint16_t len, uint16_t offset) {
    struct defy_half_version version;
    uint8_t record[DEFY_HALF_VERSION_SIZE];

    defy_half_version_local(&version);
    defy_half_version_encode(&version, record);
    return bt_gatt_attr_read(conn, attr, buf, len, offset, record, sizeof(record));
}

/* It goes right after the command's characteristic, where the Neuron's discovery looks. */
#define VERSION_CHARACTERISTIC                                                                     \
    BT_GATT_CHARACTERISTIC(BT_UUID_DECLARE_128(DEFY_HALF_VERSION_UUID), BT_GATT_CHRC_READ,        \
                           BT_GATT_PERM_READ_ENCRYPT, read_version, NULL, NULL),
#else
#define VERSION_CHARACTERISTIC
#endif

BT_GATT_SERVICE_DEFINE(defy_control_service,
    BT_GATT_PRIMARY_SERVICE(BT_UUID_DECLARE_128(DEFY_CONTROL_SERVICE_UUID)),
    BATTERY_CHARACTERISTIC
    COMMAND_CHARACTERISTIC
    VERSION_CHARACTERISTIC);
