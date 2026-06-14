#define DT_DRV_COMPAT zmk_behavior_autocomplete

#include <zephyr/device.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

#include <drivers/behavior.h>

#include <zmk/behavior.h>
#include <zmk/event_manager.h>
#include <zmk/events/keycode_state_changed.h>
#include <zmk/hid.h>
#include <zmk/behavior.h>

LOG_MODULE_DECLARE(zmk, CONFIG_ZMK_LOG_LEVEL);

#define AUTOCOMPLETE_HISTORY_SIZE 64

struct autocomplete_sequence {
    const uint32_t *bindings;
    uint8_t binding_len;

    uint32_t semantic[32];
};

struct autocomplete_config {
    int32_t max_delay_ms;

    struct autocomplete_sequence *sequences;
    uint8_t sequence_count;
};

struct autocomplete_data {
    uint32_t history[AUTOCOMPLETE_HISTORY_SIZE];

    uint8_t head;
    uint8_t count;

    int64_t last_press_time;

    bool firing;
};

static struct autocomplete_data global_data;

static uint32_t encode(uint32_t keycode) {
    return keycode;
}

static bool is_modifier(uint32_t keycode) {
    return keycode >= HID_USAGE_KEY_KEYBOARD_LEFTCONTROL &&
           keycode <= HID_USAGE_KEY_KEYBOARD_RIGHT_GUI;
}

static bool is_backspace(uint32_t keycode) {
    return keycode ==
        HID_USAGE_KEY_KEYBOARD_DELETE_BACKSPACE;
}

static void history_push(uint32_t value) {
    global_data.history[global_data.head] = value;

    global_data.head =
        (global_data.head + 1) %
        AUTOCOMPLETE_HISTORY_SIZE;

    if (global_data.count <
        AUTOCOMPLETE_HISTORY_SIZE) {

        global_data.count++;
    }

    global_data.last_press_time =
        k_uptime_get();
}

static void history_pop(void) {
    if (global_data.count == 0) {
        return;
    }

    global_data.head =
        (global_data.head +
         AUTOCOMPLETE_HISTORY_SIZE - 1) %
        AUTOCOMPLETE_HISTORY_SIZE;

    global_data.count--;
}

static bool history_suffix_matches(
    const uint32_t *suffix,
    uint8_t len
) {
    if (len > global_data.count) {
        return false;
    }

    uint8_t start =
        (global_data.head +
         AUTOCOMPLETE_HISTORY_SIZE -
         len) %
        AUTOCOMPLETE_HISTORY_SIZE;

    for (uint8_t i = 0; i < len; i++) {

        uint32_t v =
            global_data.history[
                (start + i) %
                AUTOCOMPLETE_HISTORY_SIZE
            ];

        if (v != suffix[i]) {
            return false;
        }
    }

    return true;
}

static int emit_keycode(
    uint32_t keycode,
    struct zmk_behavior_binding_event event
) {
    struct zmk_behavior_binding kp = {
        .behavior_dev = "kp",
        .param1 = keycode,
        .param2 = 0,
    };

    return zmk_behavior_invoke_binding(&kp, event, true);
}

static int autocomplete_listener(
    const zmk_event_t *eh
) {
    const struct zmk_keycode_state_changed *ev =
        as_zmk_keycode_state_changed(eh);

    if (!ev) {
        return ZMK_EV_EVENT_BUBBLE;
    }

    if (ev->state) {
        return ZMK_EV_EVENT_BUBBLE;
    }

    if (global_data.firing) {
        return ZMK_EV_EVENT_BUBBLE;
    }

    if (ev->usage_page != HID_USAGE_KEY) {
        return ZMK_EV_EVENT_BUBBLE;
    }

    if (is_modifier(ev->keycode)) {
        return ZMK_EV_EVENT_BUBBLE;
    }

    if (is_backspace(ev->keycode)) {
        history_pop();
        return ZMK_EV_EVENT_BUBBLE;
    }

    history_push(encode(ev->keycode));
    return ZMK_EV_EVENT_BUBBLE;
}

ZMK_LISTENER(
    autocomplete,
    autocomplete_listener
);

ZMK_SUBSCRIPTION(
    autocomplete,
    zmk_keycode_state_changed
);

static int binding_pressed(
    struct zmk_behavior_binding *binding,
    struct zmk_behavior_binding_event event
) {
    return 0;
}

static int binding_released(
    struct zmk_behavior_binding *binding,
    struct zmk_behavior_binding_event event
) {
    const struct device *dev =
        device_get_binding(binding->behavior_dev);

    const struct autocomplete_config *cfg =
        dev->config;

    global_data.firing = true;

    uint8_t best_len = 0;
    struct autocomplete_sequence *best = NULL;

    for (uint8_t i = 0;
         i < cfg->sequence_count;
         i++) {

        struct autocomplete_sequence *s =
            &cfg->sequences[i];

        for (uint8_t prefix = 1;
             prefix <= s->binding_len;
             prefix++) {

            if (!history_suffix_matches(
                    s->semantic,
                    prefix
                )) {
                continue;
            }

            if (prefix > best_len) {
                best_len = prefix;
                best = s;
            }
        }
    }

    if (!best) {
        global_data.firing = false;
        return 0;
    }

    uint8_t shared = best_len;

    for (;;) {

        if (shared >= best->binding_len) {
            break;
        }

        uint32_t next =
            best->semantic[shared];

        bool all_match = true;

        for (uint8_t i = 0;
             i < cfg->sequence_count;
             i++) {

            struct autocomplete_sequence *s =
                &cfg->sequences[i];

            if (s->binding_len <= shared) {
                continue;
            }

            if (!history_suffix_matches(
                    s->semantic,
                    best_len
                )) {
                continue;
            }

            if (s->semantic[shared] != next) {
                all_match = false;
                break;
            }
        }

        if (!all_match) {
            break;
        }

        emit_keycode(
            best->bindings[shared],
            event
        );

        history_push(
            best->semantic[shared]
        );

        shared++;
    }

    global_data.firing = false;

    return 0;
}

static const struct behavior_driver_api api = {
    .binding_pressed = binding_pressed,
    .binding_released = binding_released,
};

static int autocomplete_init(
    const struct device *dev
) {
    struct autocomplete_config *cfg =
        (struct autocomplete_config *)dev->config;

    for (uint8_t i = 0;
         i < cfg->sequence_count;
         i++) {

        struct autocomplete_sequence *seq =
            &cfg->sequences[i];

        for (uint8_t j = 0;
             j < seq->binding_len;
             j++) {

            seq->semantic[j] = encode(seq->bindings[j]);
        }
    }

    return 0;
}

#define AUTOCOMPLETE_CHILD_DECL(child)            \
    static const uint32_t                         \
        bindings_##child[] =                      \
            DT_PROP(child, bindings);

#define AUTOCOMPLETE_SEQ_INIT(child)              \
    {                                             \
        .bindings = bindings_##child,             \
        .binding_len =                            \
            ARRAY_SIZE(bindings_##child),         \
    },

#define AUTOCOMPLETE_INST(n)                      \
                                                    \
    DT_FOREACH_CHILD(                             \
        DT_DRV_INST(n),                           \
        AUTOCOMPLETE_CHILD_DECL                   \
    )                                             \
                                                    \
    static struct autocomplete_sequence           \
        sequences_##n[] = {                       \
            DT_FOREACH_CHILD(                     \
                DT_DRV_INST(n),                   \
                AUTOCOMPLETE_SEQ_INIT             \
            )                                     \
    };                                            \
                                                    \
    static struct autocomplete_config             \
        cfg_##n = {                               \
            .max_delay_ms =                       \
                DT_INST_PROP(n, max_delay_ms),    \
                                                    \
            .sequences = sequences_##n,           \
                                                    \
            .sequence_count =                     \
                ARRAY_SIZE(sequences_##n),        \
    };                                            \
                                                    \
    static struct autocomplete_data               \
        autocomplete_data_##n;                    \
                                                    \
    BEHAVIOR_DT_INST_DEFINE(                      \
        n,                                        \
        autocomplete_init,                        \
        NULL,                                     \
        &autocomplete_data_##n,                   \
        &cfg_##n,                                 \
        APPLICATION,                              \
        CONFIG_KERNEL_INIT_PRIORITY_DEFAULT,      \
        &api                                      \
    );

DT_INST_FOREACH_STATUS_OKAY(
    AUTOCOMPLETE_INST
)