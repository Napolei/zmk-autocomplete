/*
 * SPDX-License-Identifier: MIT
 */

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

struct autocomplete_sequence {
    const uint32_t *sequence;
    uint8_t sequence_len;
};

struct autocomplete_config {
    int32_t max_delay_ms;

    const struct autocomplete_sequence *sequences;
    uint8_t sequence_count;
};

struct autocomplete_data {
    uint32_t history[AUTOCOMPLETE_HISTORY_SIZE];

    uint8_t head;
    uint8_t count;

    int64_t last_press_time;

    bool firing;
};

/* -------------------------------------------------------------------------- */
/* History                                                                    */
/* -------------------------------------------------------------------------- */

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

static bool history_tail(
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

static inline uint32_t encoded_from_event(
    const struct zmk_keycode_state_changed *ev
) {
    uint8_t mods =
        ev->implicit_modifiers |
        ev->explicit_modifiers;

    return
        ((uint32_t)mods << 24) |
        ((uint32_t)ev->usage_page << 16) |
        (uint32_t)ev->keycode;
}

static bool is_modifier_keycode(
    uint32_t keycode
) {
    return
        keycode >= HID_USAGE_KEY_KEYBOARD_LEFTCONTROL &&
        keycode <= HID_USAGE_KEY_KEYBOARD_RIGHT_GUI;
}

/* -------------------------------------------------------------------------- */
/* Matching                                                                   */
/* -------------------------------------------------------------------------- */

static bool sequence_matches_history(
    const struct autocomplete_sequence *seq,
    const uint32_t *history,
    uint8_t history_len
) {
    if (history_len > seq->sequence_len) {
        return false;
    }

    return memcmp(
        seq->sequence,
        history,
        history_len * sizeof(uint32_t)
    ) == 0;
}

static uint8_t longest_shared_prefix(
    const struct autocomplete_sequence **matches,
    uint8_t count
) {
    if (count == 0) {
        return 0;
    }

    uint8_t pos = 0;

    while (1) {

        if (pos >= matches[0]->sequence_len) {
            return pos;
        }

        uint32_t value =
            matches[0]->sequence[pos];

        for (uint8_t i = 1; i < count; i++) {

            if (pos >= matches[i]->sequence_len) {
                return pos;
            }

            if (matches[i]->sequence[pos] != value) {
                return pos;
            }
        }

        pos++;
    }
}

static int collect_matches(
    const struct autocomplete_config *cfg,
    struct autocomplete_data *data,
    const struct autocomplete_sequence **matches,
    uint8_t *match_count,
    uint8_t *history_len_out
) {
    uint32_t window[
        AUTOCOMPLETE_HISTORY_SIZE
    ];

    int64_t now = k_uptime_get();

    if (cfg->max_delay_ms > 0 &&
        (now - data->last_press_time) >
            cfg->max_delay_ms) {

        return -1;
    }

    for (uint8_t len = data->count;
         len > 0;
         len--) {

        if (!history_tail(
                data,
                len,
                window
            )) {

            continue;
        }

        uint8_t count = 0;

        for (uint8_t i = 0;
             i < cfg->sequence_count;
             i++) {

            const struct autocomplete_sequence *seq =
                &cfg->sequences[i];

            if (sequence_matches_history(
                    seq,
                    window,
                    len
                )) {

                matches[count++] = seq;
            }
        }

        if (count > 0) {

            *match_count = count;
            *history_len_out = len;

            return 0;
        }
    }

    return -1;
}

/* -------------------------------------------------------------------------- */
/* Emission                                                                   */
/* -------------------------------------------------------------------------- */

static int emit_sequence(
    struct autocomplete_data *data,
    const uint32_t *sequence,
    uint8_t start,
    uint8_t end,
    struct zmk_behavior_binding_event event
) {
    int ret;

    for (uint8_t i = start;
         i < end;
         i++) {

        uint32_t encoded =
            sequence[i];

        uint8_t usage_page =
            (encoded >> 16) & 0xFF;

        uint8_t keycode =
            encoded & 0xFF;

        struct zmk_behavior_binding binding = {
            .behavior_dev = "KEY_PRESS",
            .param1 = keycode,
            .param2 = 0,
        };

        ret = zmk_behavior_invoke_binding(
            &binding,
            event,
            true
        );

        if (ret < 0) {
            return ret;
        }

        ret = zmk_behavior_invoke_binding(
            &binding,
            event,
            false
        );

        if (ret < 0) {
            return ret;
        }

        history_push(data, encoded);
    }

    return 0;
}

/* -------------------------------------------------------------------------- */
/* Device declarations                                                        */
/* -------------------------------------------------------------------------- */

#define AUTOCOMPLETE_DECLARE(n) \
    static struct autocomplete_data ac_data_##n;

DT_INST_FOREACH_STATUS_OKAY(
    AUTOCOMPLETE_DECLARE
)

/* -------------------------------------------------------------------------- */
/* Listener                                                                   */
/* -------------------------------------------------------------------------- */

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

    if (ev->usage_page != HID_USAGE_KEY) {
        return ZMK_EV_EVENT_BUBBLE;
    }

    if (is_modifier_keycode(ev->keycode)) {
        return ZMK_EV_EVENT_BUBBLE;
    }

    uint32_t encoded =
        encoded_from_event(ev);

#define AUTOCOMPLETE_PUSH(n)            \
    do {                                \
        if (!ac_data_##n.firing) {      \
            history_push(               \
                &ac_data_##n,           \
                encoded                 \
            );                          \
        }                               \
    } while (0)

    DT_INST_FOREACH_STATUS_OKAY(
        AUTOCOMPLETE_PUSH
    );

#undef AUTOCOMPLETE_PUSH

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

    const struct autocomplete_sequence *matches[32];

    uint8_t match_count = 0;
    uint8_t history_len = 0;

    if (collect_matches(
            cfg,
            data,
            matches,
            &match_count,
            &history_len
        ) < 0) {

        return 0;
    }

    uint8_t shared_len =
        longest_shared_prefix(
            matches,
            match_count
        );

    if (shared_len <= history_len) {
        return 0;
    }

    data->firing = true;

    int ret =
        emit_sequence(
            data,
            matches[0]->sequence,
            history_len,
            shared_len,
            event
        );

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
/* DT helpers                                                                 */
/* -------------------------------------------------------------------------- */

#define AC_CHILD_DECL(child)                    \
    static const uint32_t                      \
        ac_sequence_##child[] =                \
            DT_PROP(child, sequence);

#define AC_SEQ_INIT(child)                     \
    {                                          \
        .sequence =                            \
            ac_sequence_##child,               \
                                               \
        .sequence_len =                        \
            ARRAY_SIZE(                        \
                ac_sequence_##child            \
            ),                                 \
    },

/* -------------------------------------------------------------------------- */
/* Instantiation                                                              */
/* -------------------------------------------------------------------------- */

#define AUTOCOMPLETE_INST(n)                   \
                                               \
    DT_FOREACH_CHILD(                          \
        DT_DRV_INST(n),                        \
        AC_CHILD_DECL                          \
    )                                          \
                                               \
    static const                               \
        struct autocomplete_sequence           \
        ac_sequences_##n[] = {                 \
                                               \
            DT_FOREACH_CHILD(                  \
                DT_DRV_INST(n),                \
                AC_SEQ_INIT                    \
            )                                  \
    };                                         \
                                               \
    static const                               \
        struct autocomplete_config             \
        ac_cfg_##n = {                         \
                                               \
            .max_delay_ms =                    \
                DT_INST_PROP(                  \
                    n,                         \
                    max_delay_ms               \
                ),                             \
                                               \
            .sequences =                       \
                ac_sequences_##n,              \
                                               \
            .sequence_count =                  \
                ARRAY_SIZE(                    \
                    ac_sequences_##n           \
                ),                             \
    };                                         \
                                               \
    static struct autocomplete_data            \
        ac_data_##n;                           \
                                               \
    BEHAVIOR_DT_INST_DEFINE(                   \
        n,                                     \
        NULL,                                  \
        NULL,                                  \
        &ac_data_##n,                          \
        &ac_cfg_##n,                           \
        APPLICATION,                           \
        CONFIG_KERNEL_INIT_PRIORITY_DEFAULT,   \
        &autocomplete_driver_api               \
    );

DT_INST_FOREACH_STATUS_OKAY(
    AUTOCOMPLETE_INST
)