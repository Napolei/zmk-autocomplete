#define DT_DRV_COMPAT zmk_behavior_autocomplete

#include <string.h>

#include <zephyr/device.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

#include <drivers/behavior.h>

#include <zmk/behavior.h>
#include <zmk/event_manager.h>
#include <zmk/events/keycode_state_changed.h>
#include <zmk/hid.h>
#include <zmk/keymap.h>

LOG_MODULE_DECLARE(zmk, CONFIG_ZMK_LOG_LEVEL);

#define AUTOCOMPLETE_HISTORY_SIZE \
    CONFIG_ZMK_AUTOCOMPLETE_HISTORY_SIZE

#define AUTOCOMPLETE_MAX_MATCHES 32

struct autocomplete_sequence {
    const uint32_t *bindings;
    uint8_t binding_len;
};

struct autocomplete_config {
    int32_t max_delay_ms;

    const struct autocomplete_sequence *sequences;
    uint8_t sequence_count;
};

struct autocomplete_data {
    uint32_t history[
        AUTOCOMPLETE_HISTORY_SIZE
    ];

    uint8_t head;
    uint8_t count;

    int64_t last_press_time;

    bool firing;
    bool capturing;
};

static inline uint32_t encode_semantic(
    uint8_t mods,
    uint16_t usage_page,
    uint16_t keycode
) {
    return
        ((uint32_t)mods << 24) |
        ((uint32_t)usage_page << 16) |
        keycode;
}

static inline uint32_t encoded_from_event(
    const struct zmk_keycode_state_changed *ev
) {
    uint8_t mods =
        ev->implicit_modifiers |
        ev->explicit_modifiers;

    return encode_semantic(
        mods,
        ev->usage_page,
        ev->keycode
    );
}

static bool is_modifier_keycode(
    uint32_t keycode
) {
    return
        (keycode >=
            HID_USAGE_KEY_KEYBOARD_LEFTCONTROL &&
         keycode <=
            HID_USAGE_KEY_KEYBOARD_RIGHT_GUI);
}

static bool is_backspace(
    uint32_t keycode
) {
    return
        keycode ==
            HID_USAGE_KEY_KEYBOARD_DELETE_BACKSPACE;
}

static bool is_delete(
    uint32_t keycode
) {
    return
        keycode ==
            HID_USAGE_KEY_KEYBOARD_DELETE_FORWARD;
}

static void history_push(
    struct autocomplete_data *data,
    uint32_t encoded
) {
    data->history[data->head] = encoded;

    data->head =
        (data->head + 1) %
        AUTOCOMPLETE_HISTORY_SIZE;

    if (data->count <
        AUTOCOMPLETE_HISTORY_SIZE) {

        data->count++;
    }

    data->last_press_time =
        k_uptime_get();
}

static void history_pop(
    struct autocomplete_data *data
) {
    if (data->count == 0) {
        return;
    }

    data->head =
        (data->head +
         AUTOCOMPLETE_HISTORY_SIZE -
         1) %
        AUTOCOMPLETE_HISTORY_SIZE;

    data->count--;
}

static bool history_suffix(
    const struct autocomplete_data *data,
    uint8_t len,
    uint32_t *out
) {
    if (len == 0 ||
        len > data->count) {

        return false;
    }

    int start =
        ((int)data->head -
         (int)len +
         AUTOCOMPLETE_HISTORY_SIZE) %
        AUTOCOMPLETE_HISTORY_SIZE;

    for (uint8_t i = 0; i < len; i++) {

        out[i] =
            data->history[
                (start + i) %
                AUTOCOMPLETE_HISTORY_SIZE
            ];
    }

    return true;
}

static bool sequence_matches(
    const struct autocomplete_sequence *seq,
    const uint32_t *history,
    uint8_t history_len
) {
    if (history_len >
        seq->binding_len) {

        return false;
    }

    return memcmp(
        history,
        seq->bindings,
        history_len *
            sizeof(uint32_t)
    ) == 0;
}

static uint8_t
longest_shared_continuation(
    const struct autocomplete_sequence
        **matches,
    uint8_t match_count,
    uint8_t prefix_len
) {
    if (match_count == 0) {
        return 0;
    }

    uint8_t continuation = 0;

    while (true) {

        bool first = true;
        uint32_t expected = 0;

        for (uint8_t i = 0;
             i < match_count;
             i++) {

            const struct
                autocomplete_sequence *seq =
                    matches[i];

            uint8_t pos =
                prefix_len +
                continuation;

            if (pos >=
                seq->binding_len) {

                return continuation;
            }

            uint32_t value =
                seq->bindings[pos];

            if (first) {

                expected = value;
                first = false;

            } else if (
                value != expected
            ) {

                return continuation;
            }
        }

        continuation++;
    }
}

static int emit_keycode(
    uint32_t usage,
    struct zmk_behavior_binding_event event
) {
    struct zmk_behavior_binding binding = {
        .behavior_dev = "KEY_PRESS",
        .param1 = usage,
        .param2 = 0,
    };

    int ret;

    ret =
        zmk_behavior_invoke_binding(
            &binding,
            event,
            true
        );

    if (ret < 0) {
        return ret;
    }

    return
        zmk_behavior_invoke_binding(
            &binding,
            event,
            false
        );
}

static int emit_continuation(
    const struct autocomplete_sequence *seq,
    uint8_t start,
    uint8_t len,
    struct zmk_behavior_binding_event event
) {
    int ret;

    for (uint8_t i = start;
         i < start + len;
         i++) {

        ret =
            emit_keycode(
                seq->bindings[i],
                event
            );

        if (ret < 0) {
            return ret;
        }
    }

    return 0;
}

static int find_matches(
    const struct autocomplete_config *cfg,
    struct autocomplete_data *data,
    const struct autocomplete_sequence
        **matches,
    uint8_t *out_match_count,
    uint8_t *out_prefix_len
) {
    int64_t now =
        k_uptime_get();

    if (cfg->max_delay_ms > 0 &&
        (now - data->last_press_time) >
            cfg->max_delay_ms) {

        return -ENOENT;
    }

    uint8_t best_prefix = 0;
    uint8_t best_match_count = 0;

    uint32_t buffer[
        AUTOCOMPLETE_HISTORY_SIZE
    ];

    for (uint8_t len = data->count;
         len >= 1;
         len--) {

        if (!history_suffix(
                data,
                len,
                buffer
            )) {

            continue;
        }

        uint8_t count = 0;

        for (uint8_t i = 0;
             i < cfg->sequence_count;
             i++) {

            const struct
                autocomplete_sequence *seq =
                    &cfg->sequences[i];

            if (sequence_matches(
                    seq,
                    buffer,
                    len
                )) {

                matches[count++] = seq;
            }
        }

        if (count > 0) {

            best_prefix = len;
            best_match_count = count;
            break;
        }

        if (len == 1) {
            break;
        }
    }

    if (best_match_count == 0) {
        return -ENOENT;
    }

    *out_match_count =
        best_match_count;

    *out_prefix_len =
        best_prefix;

    return 0;
}

/* -------------------------------------------------------------------------- */
/* Listener                                                                   */
/* -------------------------------------------------------------------------- */

#define AUTOCOMPLETE_DECLARE(n) \
    static struct autocomplete_data autocomplete_data_##n;

DT_INST_FOREACH_STATUS_OKAY(
    AUTOCOMPLETE_DECLARE
)

static int autocomplete_keycode_listener(
    const zmk_event_t *eh
) {
    const struct
        zmk_keycode_state_changed *ev =
            as_zmk_keycode_state_changed(
                eh
            );

    if (!ev) {
        return ZMK_EV_EVENT_BUBBLE;
    }

    if (ev->state) {
        return ZMK_EV_EVENT_BUBBLE;
    }

    if (ev->usage_page !=
        HID_USAGE_KEY) {

        return ZMK_EV_EVENT_BUBBLE;
    }

    uint32_t keycode =
        ev->keycode;

    if (is_modifier_keycode(
            keycode
        )) {

        return ZMK_EV_EVENT_BUBBLE;
    }

    uint32_t encoded =
        encoded_from_event(ev);

    if (is_backspace(keycode) ||
        is_delete(keycode)) {

#define AUTOCOMPLETE_POP(n)               \
    if (!autocomplete_data_##n.firing) { \
        history_pop(                      \
            &autocomplete_data_##n        \
        );                                \
    }

        DT_INST_FOREACH_STATUS_OKAY(
            AUTOCOMPLETE_POP
        )

#undef AUTOCOMPLETE_POP

        return ZMK_EV_EVENT_BUBBLE;
    }

#define AUTOCOMPLETE_PUSH(n)                       \
    if (!autocomplete_data_##n.firing ||          \
        autocomplete_data_##n.capturing) {        \
                                                   \
        history_push(                              \
            &autocomplete_data_##n,                \
            encoded                                \
        );                                         \
    }

    DT_INST_FOREACH_STATUS_OKAY(
        AUTOCOMPLETE_PUSH
    )

#undef AUTOCOMPLETE_PUSH

    return ZMK_EV_EVENT_BUBBLE;
}

ZMK_LISTENER(
    autocomplete,
    autocomplete_keycode_listener
);

ZMK_SUBSCRIPTION(
    autocomplete,
    zmk_keycode_state_changed
);

/* -------------------------------------------------------------------------- */
/* Behavior                                                                   */
/* -------------------------------------------------------------------------- */

static int autocomplete_binding_pressed(
    struct zmk_behavior_binding *binding,
    struct zmk_behavior_binding_event event
) {
    return 0;
}

static int autocomplete_binding_released(
    struct zmk_behavior_binding *binding,
    struct zmk_behavior_binding_event event
) {
    const struct device *dev =
        device_get_binding(
            binding->behavior_dev
        );

    const struct autocomplete_config *cfg =
        dev->config;

    struct autocomplete_data *data =
        dev->data;

    const struct autocomplete_sequence
        *matches[
            AUTOCOMPLETE_MAX_MATCHES
        ];

    uint8_t match_count = 0;
    uint8_t prefix_len = 0;

    int ret =
        find_matches(
            cfg,
            data,
            matches,
            &match_count,
            &prefix_len
        );

    if (ret < 0) {
        return ret;
    }

    uint8_t continuation =
        longest_shared_continuation(
            matches,
            match_count,
            prefix_len
        );

    if (continuation == 0) {
        return 0;
    }

    const struct autocomplete_sequence *seq =
        matches[0];

    data->firing = true;
    data->capturing = true;

    ret =
        emit_continuation(
            seq,
            prefix_len,
            continuation,
            event
        );

    data->capturing = false;
    data->firing = false;

    return ret;
}

static const struct behavior_driver_api
    autocomplete_driver_api = {

        .binding_pressed =
            autocomplete_binding_pressed,

        .binding_released =
            autocomplete_binding_released,
};

/* -------------------------------------------------------------------------- */
/* DT Helpers                                                                 */
/* -------------------------------------------------------------------------- */

#define AUTOCOMPLETE_CHILD_DECL(child)              \
    static const uint32_t                           \
        autocomplete_bindings_##child[] =           \
            DT_PROP(child, bindings);

#define AUTOCOMPLETE_SEQ_INIT(child)                \
    {                                               \
        .bindings =                                 \
            autocomplete_bindings_##child,          \
                                                    \
        .binding_len =                              \
            ARRAY_SIZE(                             \
                autocomplete_bindings_##child       \
            ),                                      \
    },

/* -------------------------------------------------------------------------- */
/* DT Instantiation                                                           */
/* -------------------------------------------------------------------------- */

#define AUTOCOMPLETE_INST(n)                        \
                                                     \
    DT_FOREACH_CHILD(                               \
        DT_DRV_INST(n),                             \
        AUTOCOMPLETE_CHILD_DECL                     \
    )                                               \
                                                     \
    static const                                    \
        struct autocomplete_sequence                \
        autocomplete_sequences_##n[] = {            \
                                                     \
            DT_FOREACH_CHILD(                       \
                DT_DRV_INST(n),                     \
                AUTOCOMPLETE_SEQ_INIT               \
            )                                       \
    };                                              \
                                                     \
    static const                                    \
        struct autocomplete_config                  \
        autocomplete_cfg_##n = {                    \
                                                     \
            .max_delay_ms =                         \
                DT_INST_PROP(                       \
                    n,                              \
                    max_delay_ms                    \
                ),                                  \
                                                     \
            .sequences =                            \
                autocomplete_sequences_##n,         \
                                                     \
            .sequence_count =                       \
                ARRAY_SIZE(                         \
                    autocomplete_sequences_##n      \
                ),                                  \
    };                                              \
                                                     \
    BEHAVIOR_DT_INST_DEFINE(                        \
        n,                                          \
        NULL,                                       \
        NULL,                                       \
        &autocomplete_data_##n,                     \
        &autocomplete_cfg_##n,                      \
        APPLICATION,                                \
        CONFIG_KERNEL_INIT_PRIORITY_DEFAULT,        \
        &autocomplete_driver_api                    \
    );

DT_INST_FOREACH_STATUS_OKAY(
    AUTOCOMPLETE_INST
);