/* SPDX-License-Identifier: MIT */

#include <dt-bindings/dygma/defy_lighting.h>
#include <dt-bindings/zmk/matrix_transform.h>
#include <dygma/defy_control.h>
#include <dygma/defy_lighting.h>
#include <dygma/defy_wired_protocol.h>
#include <errno.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/random/random.h>
#include <zephyr/settings/settings.h>
#include <zephyr/sys/crc.h>
#include <zmk/events/layer_state_changed.h>
#include <zmk/events/position_state_changed.h>
#include <zmk/keymap.h>
#include <dygma/defy_profile_chooser.h>

LOG_MODULE_REGISTER(defy_lighting, CONFIG_ZMK_LOG_LEVEL);

static struct k_spinlock lighting_lock;
static struct defy_lighting_state desired;
#define LIGHTING_IDLE_MS 60000

/*
 * The looks and the first brightness, from &defy_lighting in the keymap. A look
 * gives the keys and the underglow a colour, 0xRRGGBB at full strength, which
 * the brightness scales.
 */
struct look {
    uint32_t keys;
    uint32_t underglow;
};

#if DT_HAS_COMPAT_STATUS_OKAY(dygma_defy_lighting)
#define LIGHTING_NODE DT_INST(0, dygma_defy_lighting)
#define FIRST_LEVEL DT_PROP(LIGHTING_NODE, brightness)
#define KEYMAP_LOOKS DT_CHILD_NUM_STATUS_OKAY(LIGHTING_NODE)
#else
#define FIRST_LEVEL 24
#define KEYMAP_LOOKS 0
#endif

#if KEYMAP_LOOKS
#define LOOK(node)                                                                                 \
    BUILD_ASSERT(DT_PROP(node, keys) <= 0xffffff && DT_PROP(node, underglow) <= 0xffffff,         \
                 "A look's colours are 0xRRGGBB");
DT_FOREACH_CHILD_STATUS_OKAY(LIGHTING_NODE, LOOK)
#undef LOOK
#define LOOK(node) {DT_PROP(node, keys), DT_PROP(node, underglow)},
static const struct look looks[] = {DT_FOREACH_CHILD_STATUS_OKAY(LIGHTING_NODE, LOOK)};
#else
/* Without looks in the keymap: white keys, warm white keys, both white, and white underglow. */
static const struct look looks[] = {
    {DEFY_WHITE, DEFY_OFF},
    {DEFY_WARM_WHITE, DEFY_OFF},
    {DEFY_WHITE, DEFY_WHITE},
    {DEFY_OFF, DEFY_WHITE},
};
#endif
BUILD_ASSERT(ARRAY_SIZE(looks) <= UINT8_MAX, "At most 255 looks");
BUILD_ASSERT(FIRST_LEVEL >= 1 && FIRST_LEVEL <= 255, "brightness is 1 to 255");

/* The brightness steps of DEFY_LIGHTING_BRIGHTER and DEFY_LIGHTING_DIMMER. */
static const uint8_t levels[] = {4, 8, 12, 16, 24, 32, 48, 64, 96, 128, 192, 255};

/*
 * Colours for keymap layers, from &defy_layer_lighting. A colour of -1 is left
 * out, and comes from the look.
 */
struct layer_look {
    uint8_t layer;
    int32_t keys;
    int32_t underglow;
};

#if DT_HAS_COMPAT_STATUS_OKAY(dygma_defy_layer_lighting)
#define LAYERS_NODE DT_INST(0, dygma_defy_layer_lighting)
#define LAYER_LOOKS DT_CHILD_NUM_STATUS_OKAY(LAYERS_NODE)
BUILD_ASSERT(DT_NUM_INST_STATUS_OKAY(dygma_defy_layer_lighting) == 1,
             "One &defy_layer_lighting holds every layer's colours");
#else
#define LAYER_LOOKS 0
#endif

#if DT_HAS_COMPAT_STATUS_OKAY(dygma_defy_key_lighting)
#define KEYS_NODE DT_INST(0, dygma_defy_key_lighting)
#define KEY_GROUPS DT_CHILD_NUM_STATUS_OKAY(KEYS_NODE)
BUILD_ASSERT(DT_NUM_INST_STATUS_OKAY(dygma_defy_key_lighting) == 1,
             "One &defy_key_lighting holds every key colour");
#else
#define KEY_GROUPS 0
#endif

#if LAYER_LOOKS || KEY_GROUPS
/*
 * ZMK_KEYMAP_LAYERS_LEN expands DT_FOREACH_CHILD, which cannot nest in the
 * DT_FOREACH_CHILD below.
 */
enum { KEYMAP_LAYERS = ZMK_KEYMAP_LAYERS_LEN };
#endif

#if LAYER_LOOKS
#define LAYER_LOOK(node)                                                                           \
    BUILD_ASSERT(DT_PROP(node, layer) < KEYMAP_LAYERS, "No such layer");                          \
    BUILD_ASSERT(DT_PROP_OR(node, keys, 0) <= 0xffffff &&                                          \
                     DT_PROP_OR(node, underglow, 0) <= 0xffffff,                                   \
                 "A layer's colours are 0xRRGGBB");
DT_FOREACH_CHILD_STATUS_OKAY(LAYERS_NODE, LAYER_LOOK)
#undef LAYER_LOOK
#define LAYER_LOOK(node)                                                                           \
    {DT_PROP(node, layer), DT_PROP_OR(node, keys, -1), DT_PROP_OR(node, underglow, -1)},
static const struct layer_look layer_looks[] = {
    DT_FOREACH_CHILD_STATUS_OKAY(LAYERS_NODE, LAYER_LOOK)};
#endif

/*
 * Colours for single keys of a layer, from &defy_key_lighting: a group each,
 * numbered in the keymap's order. The halves keep them and draw those a
 * state names.
 */
struct key_group {
    uint8_t layer;
    uint32_t colour;
    const uint8_t *positions;
    uint8_t count;
};

#if KEY_GROUPS
BUILD_ASSERT(KEY_GROUPS <= DEFY_LIGHTING_GROUPS, "At most 16 key colours");
#define KEY_POSITION(node, prop, index)                                                            \
    BUILD_ASSERT(DT_PROP_BY_IDX(node, prop, index) < DEFY_WIRED_KEY_COUNT, "No such key");
#define KEY_GROUP(node)                                                                            \
    BUILD_ASSERT(DT_PROP(node, layer) < KEYMAP_LAYERS, "No such layer");                          \
    BUILD_ASSERT(DT_PROP(node, colour) <= 0xffffff, "A key colour is 0xRRGGBB");                  \
    DT_FOREACH_PROP_ELEM(node, key_positions, KEY_POSITION)
DT_FOREACH_CHILD_STATUS_OKAY(KEYS_NODE, KEY_GROUP)
#undef KEY_GROUP
#define KEY_GROUP(node)                                                                            \
    {DT_PROP(node, layer), DT_PROP(node, colour), (const uint8_t[])DT_PROP(node, key_positions), \
     DT_PROP_LEN(node, key_positions)},
static const struct key_group key_groups[] = {DT_FOREACH_CHILD_STATUS_OKAY(KEYS_NODE, KEY_GROUP)};

/*
 * The matrix row and column of each key position, from the board's transform.
 * Columns 8 to 15 are the right half's.
 */
#define TRANSFORM DT_NODELABEL(default_transform)
static const uint16_t key_matrix[] = DT_PROP(TRANSFORM, map);
BUILD_ASSERT(ARRAY_SIZE(key_matrix) == DEFY_WIRED_KEY_COUNT, "The transform maps every key");

static const uint8_t row_leds[] = DEFY_LIGHTING_ROW_LEDS;

/*
 * The per-key LED of a key position on the half on side, or -1 when the
 * other half has it. Each half numbers its LEDs row by row, from the outer
 * end of each row: on the left the LED is the column, on the right 7 minus
 * the half's own column.
 */
static int key_led(uint8_t position, uint8_t side) {
    uint8_t row = KT_ROW(key_matrix[position]);
    uint8_t column = KT_COL(key_matrix[position]);
    bool right = side == DEFY_WIRED_RIGHT;
    if (row >= ARRAY_SIZE(row_leds) || (column >= 8) != right) {
        return -1;
    }
    int led = right ? 15 - column : column;
    if (led >= row_leds[row]) {
        return -1;
    }
    for (uint8_t r = 0; r < row; r++) {
        led += row_leds[r];
    }
    return led;
}
#endif

/*
 * "defy/look": the look and brightness, with a CRC of the looks and first
 * brightness they belong to.
 */
struct look_setting {
    uint32_t config;
    uint8_t look;
    uint8_t level;
} __packed;

static bool settings_ready;
static bool preferred_enabled = true;
static bool idle;
static int64_t last_activity_ms;
static uint32_t wake_boot[DEFY_WIRED_RIGHT + 1];
static uint8_t look_index;
static uint8_t level = FIRST_LEVEL;
static bool saved_enabled = true;
static uint8_t saved_look;
static uint8_t saved_level = FIRST_LEVEL;
static unsigned int save_attempts;

static void publish_setting(void) {
    defy_wired_lighting_changed();
    defy_control_ble_lighting_changed();
}

bool defy_lighting_central_get(struct defy_lighting_state *state) {
    if (!state) {
        return false;
    }
    k_spinlock_key_t key = k_spin_lock(&lighting_lock);
    bool available = settings_ready;
    if (available) {
        *state = desired;
    }
    k_spin_unlock(&lighting_lock, key);
    return available;
}

/*
 * What the saved look and brightness depend on: the keymap's looks and first
 * brightness. A build whose looks differ does not take them.
 */
static uint32_t looks_checksum(void) {
    const uint8_t first = FIRST_LEVEL;
    return crc32_ieee_update(crc32_ieee((const uint8_t *)looks, sizeof(looks)), &first, 1);
}

/* A layer's colours: its look, if it has one, and its key groups. */
struct layer_colours {
    const struct layer_look *look;
    uint16_t groups;
};

#if LAYER_LOOKS || KEY_GROUPS
/* What layer gives: whether it has colours of either kind. */
static bool layer_colours(zmk_keymap_layer_id_t layer, struct layer_colours *colours) {
    *colours = (struct layer_colours){0};
#if LAYER_LOOKS
    for (size_t i = 0; i < ARRAY_SIZE(layer_looks) && !colours->look; i++) {
        if (layer_looks[i].layer == layer) {
            colours->look = &layer_looks[i];
        }
    }
#endif
#if KEY_GROUPS
    for (size_t i = 0; i < ARRAY_SIZE(key_groups); i++) {
        if (key_groups[i].layer == layer) {
            colours->groups |= BIT(i);
        }
    }
#endif
    return colours->look || colours->groups;
}
#endif

/* The colours of the highest active layer, in the keymap's order, that has any. */
static struct layer_colours active_layer_colours(void) {
    struct layer_colours colours = {0};
#if LAYER_LOOKS || KEY_GROUPS
    for (int index = ZMK_KEYMAP_LAYERS_LEN - 1; index >= 0; index--) {
        zmk_keymap_layer_id_t layer = zmk_keymap_layer_index_to_id(index);
        if (layer != ZMK_KEYMAP_LAYER_ID_INVAL && zmk_keymap_layer_active(layer) &&
            layer_colours(layer, &colours)) {
            break;
        }
    }
#endif
    return colours;
}

static struct defy_lighting_colour colour24(uint32_t colour) {
    return (struct defy_lighting_colour){colour >> 16 & 0xff, colour >> 8 & 0xff, colour & 0xff};
}

static struct defy_lighting_colour scaled(uint32_t colour) {
    return defy_lighting_scale(colour24(colour), level);
}

/*
 * Brings desired up to date with the preference, idleness, look, brightness and
 * layers, under the next revision; whether it changed. While the lights are
 * off, the colours wait, so that layer changes send nothing.
 */
static bool update_locked(void) {
    struct defy_lighting_state next = desired;
    next.enabled = preferred_enabled && !idle;
    if (next.enabled) {
        const struct look *look = &looks[look_index];
        struct layer_colours colours = active_layer_colours();
        const struct layer_look *layer = colours.look;
        next.keys = scaled(layer && layer->keys >= 0 ? layer->keys : look->keys);
        next.underglow =
            scaled(layer && layer->underglow >= 0 ? layer->underglow : look->underglow);
        next.groups = colours.groups;
        next.level = level;
    }
    if (defy_lighting_equal(&next, &desired)) {
        return false;
    }
    if (++next.revision == 0) {
        next.revision = 1;
    }
    desired = next;
    return true;
}

int defy_lighting_central_group(uint8_t side, uint8_t index, struct defy_lighting_group *group) {
    if (side > DEFY_WIRED_RIGHT || !group) {
        return -EINVAL;
    }
#if KEY_GROUPS
    if (index >= ARRAY_SIZE(key_groups)) {
        return -ENOENT;
    }
    k_spinlock_key_t key = k_spin_lock(&lighting_lock);
    bool ready = settings_ready;
    uint32_t boot = desired.boot;
    k_spin_unlock(&lighting_lock, key);
    if (!ready) {
        return -EAGAIN;
    }
    const struct key_group *keys = &key_groups[index];
    *group = (struct defy_lighting_group){
        .boot = boot, .index = index, .colour = colour24(keys->colour)};
    for (size_t i = 0; i < keys->count; i++) {
        int led = key_led(keys->positions[i], side);
        if (led >= 0) {
            group->leds |= BIT64(led);
        }
    }
    return 0;
#else
    ARG_UNUSED(index);
    return -ENOENT;
#endif
}

static void idle_expired(struct k_work *work);
static K_WORK_DELAYABLE_DEFINE(idle_work, idle_expired);

static void idle_expired(struct k_work *work) {
    ARG_UNUSED(work);
    k_spinlock_key_t key = k_spin_lock(&lighting_lock);
    int64_t remaining = last_activity_ms + LIGHTING_IDLE_MS - k_uptime_get();
    bool changed = false;
    if (settings_ready && remaining <= 0) {
        idle = true;
        changed = update_locked();
    }
    k_spin_unlock(&lighting_lock, key);
    if (remaining > 0) {
        k_work_reschedule(&idle_work, K_MSEC(remaining));
    }
    if (changed) {
        publish_setting();
    }
}

static void note_activity(void) {
    k_spinlock_key_t key = k_spin_lock(&lighting_lock);
    bool ready = settings_ready;
    bool changed = false;
    if (ready) {
        last_activity_ms = k_uptime_get();
        idle = false;
        changed = update_locked();
    }
    k_spin_unlock(&lighting_lock, key);
    if (ready) {
        k_work_reschedule(&idle_work, K_MSEC(LIGHTING_IDLE_MS));
    }
    if (changed) {
        publish_setting();
    }
}

void defy_lighting_central_wake(const struct defy_lighting_wake *wake) {
    if (!wake || !wake->from_sleep || wake->side > DEFY_WIRED_RIGHT || !wake->boot ||
        wake->age_ms >= LIGHTING_IDLE_MS) {
        return;
    }
    k_spinlock_key_t key = k_spin_lock(&lighting_lock);
    int64_t now = k_uptime_get();
    int64_t activity = now - wake->age_ms;
    int64_t remaining = 0;
    bool changed = false;
    if (settings_ready && wake_boot[wake->side] != wake->boot) {
        wake_boot[wake->side] = wake->boot;
        if (activity > last_activity_ms) {
            last_activity_ms = activity;
            idle = false;
            changed = update_locked();
            remaining = LIGHTING_IDLE_MS - wake->age_ms;
        }
    }
    k_spin_unlock(&lighting_lock, key);
    if (remaining > 0) {
        k_work_reschedule(&idle_work, K_MSEC(remaining));
    }
    if (changed) {
        publish_setting();
    }
}

static void layers_changed(void) {
    k_spinlock_key_t key = k_spin_lock(&lighting_lock);
    bool changed = settings_ready && update_locked();
    k_spin_unlock(&lighting_lock, key);
    if (changed) {
        publish_setting();
    }
}

static int lighting_event(const zmk_event_t *eh) {
    if (as_zmk_layer_state_changed(eh)) {
        layers_changed();
        return ZMK_EV_EVENT_BUBBLE;
    }
    const struct zmk_position_state_changed *event = as_zmk_position_state_changed(eh);
    if (!event) {
        return ZMK_EV_EVENT_BUBBLE;
    }
    if (event->state && event->source <= DEFY_WIRED_RIGHT &&
        event->position < DEFY_WIRED_KEY_COUNT) {
        note_activity();
    }
    if (defy_profile_chooser_process(event)) {
        return ZMK_EV_EVENT_HANDLED;
    }
    return ZMK_EV_EVENT_BUBBLE;
}

/*
 * ZMK calls listeners in link order, and module sources link before ZMK's own,
 * so this runs before hold-tap, combos and the keymap. Presses they hold back
 * still count as activity, and a key the chooser takes never reaches them.
 * ZMK's combos re-raise keys from the start of the list, so with combos this
 * would see those keys again; the Neuron keymaps define none. A layer change
 * reaches it after the keymap has changed the layers.
 */
ZMK_LISTENER(defy_lighting_central, lighting_event);
ZMK_SUBSCRIPTION(defy_lighting_central, zmk_position_state_changed);
ZMK_SUBSCRIPTION(defy_lighting_central, zmk_layer_state_changed);

static bool save_needed_locked(void) {
    return preferred_enabled != saved_enabled || look_index != saved_look || level != saved_level;
}

static void save_setting(struct k_work *work);
static K_WORK_DELAYABLE_DEFINE(save_work, save_setting);

/* Saves the preference as "defy/lighting", and the look and brightness as "defy/look". */
static void save_setting(struct k_work *work) {
    ARG_UNUSED(work);
    k_spinlock_key_t key = k_spin_lock(&lighting_lock);
    bool enabled = preferred_enabled;
    uint8_t look = look_index;
    uint8_t look_level = level;
    bool save_enabled = settings_ready && enabled != saved_enabled;
    bool save_look = settings_ready && (look != saved_look || look_level != saved_level);
    k_spin_unlock(&lighting_lock, key);
    if (!save_enabled && !save_look) {
        return;
    }
    int err = 0;
    if (save_enabled) {
        uint8_t value = enabled;
        err = settings_save_one("defy/lighting", &value, sizeof(value));
    }
    bool enabled_saved = save_enabled && !err;
    if (save_look && !err) {
        struct look_setting value = {
            .config = looks_checksum(), .look = look, .level = look_level};
        err = settings_save_one("defy/look", &value, sizeof(value));
    }
    key = k_spin_lock(&lighting_lock);
    if (enabled_saved) {
        saved_enabled = enabled;
    }
    bool retry = false;
    if (err == 0) {
        if (save_look) {
            saved_look = look;
            saved_level = look_level;
        }
        save_attempts = 0;
    } else {
        retry = ++save_attempts < 3;
    }
    retry |= preferred_enabled != enabled || look_index != look || level != look_level;
    k_spin_unlock(&lighting_lock, key);
    if (err) {
        LOG_WRN("Lighting setting save failed: %d", err);
    }
    if (retry) {
        k_work_reschedule(&save_work, K_SECONDS(1));
    }
}

static uint8_t brighter(uint8_t current) {
    for (size_t i = 0; i < ARRAY_SIZE(levels); i++) {
        if (levels[i] > current) {
            return levels[i];
        }
    }
    return current;
}

static uint8_t dimmer(uint8_t current) {
    for (size_t i = ARRAY_SIZE(levels); i > 0; i--) {
        if (levels[i - 1] < current) {
            return levels[i - 1];
        }
    }
    return current;
}

/* Sends a change at once and saves it a second later. */
int defy_lighting_command(uint32_t command) {
    if (command > DEFY_LIGHTING_NEXT) {
        return -EINVAL;
    }
    k_spinlock_key_t key = k_spin_lock(&lighting_lock);
    if (!settings_ready) {
        k_spin_unlock(&lighting_lock, key);
        return -EAGAIN;
    }
    switch (command) {
    case DEFY_LIGHTING_TOGGLE:
        preferred_enabled = !preferred_enabled;
        break;
    case DEFY_LIGHTING_BRIGHTER:
        level = brighter(level);
        break;
    case DEFY_LIGHTING_DIMMER:
        level = dimmer(level);
        break;
    default:
        look_index = (look_index + 1) % ARRAY_SIZE(looks);
        break;
    }
    /* A change of look or brightness turns the lights on, so that it shows. */
    preferred_enabled |= command != DEFY_LIGHTING_TOGGLE;
    bool changed = update_locked();
    save_attempts = 0;
    bool save = save_needed_locked();
    k_spin_unlock(&lighting_lock, key);
    if (changed) {
        publish_setting();
    }
    if (save) {
        k_work_reschedule(&save_work, K_SECONDS(1));
    }
    return 0;
}

/* The preference saved as "defy/lighting", one byte. */
static int read_preference(size_t length, settings_read_cb read_cb, void *argument) {
    if (length != 1) {
        return -EINVAL;
    }
    uint8_t value;
    int err = read_cb(argument, &value, sizeof(value));
    if (err < 0) {
        return err;
    }
    if (err != 1 || value > 1) {
        return -EINVAL;
    }
    preferred_enabled = saved_enabled = value;
    return 0;
}

/* The look and brightness saved as "defy/look", kept only when the looks are the same. */
static int read_look(size_t length, settings_read_cb read_cb, void *argument) {
    struct look_setting value;
    if (length != sizeof(value)) {
        return -EINVAL;
    }
    int err = read_cb(argument, &value, sizeof(value));
    if (err < 0) {
        return err;
    }
    if (err != sizeof(value)) {
        return -EINVAL;
    }
    if (value.config != looks_checksum() || value.look >= ARRAY_SIZE(looks) || !value.level) {
        return 0;
    }
    look_index = saved_look = value.look;
    level = saved_level = value.level;
    return 0;
}

static int lighting_setting(const char *name, size_t length, settings_read_cb read_cb,
                             void *argument) {
    const char *next;
    if (settings_name_steq(name, "lighting", &next) && !next) {
        return read_preference(length, read_cb, argument);
    }
    if (settings_name_steq(name, "look", &next) && !next) {
        return read_look(length, read_cb, argument);
    }
    return -ENOENT;
}

static int lighting_settings_commit(void) {
    uint32_t boot = sys_rand32_get();
    k_spinlock_key_t key = k_spin_lock(&lighting_lock);
    if (settings_ready) {
        k_spin_unlock(&lighting_lock, key);
        return 0;
    }
    desired.boot = boot ? boot : 1;
    last_activity_ms = k_uptime_get();
    idle = false;
    update_locked();
    desired.revision = 1;
    settings_ready = true;
    k_spin_unlock(&lighting_lock, key);
    k_work_reschedule(&idle_work, K_MSEC(LIGHTING_IDLE_MS));
    publish_setting();
    return 0;
}

SETTINGS_STATIC_HANDLER_DEFINE(defy_lighting, "defy", NULL, lighting_setting,
                               lighting_settings_commit, NULL);
