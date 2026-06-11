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

/* -------------------------------------------------------------------------- */
/* Structures                                                                 */
/* -------------------------------------------------------------------------- */

struct autocomplete_sequence {
    const struct zmk_behavior_binding *bindings;
    uint8_t binding_len;

    uint32_t encoded[AUTOCOMPLETE_HISTORY_SIZE];
};

struct autocomplete_config {
    int32_t max_delay_ms;

    struct autocomplete_sequence **sequences;
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
/* Helpers                                                                    */
/* -------------------------------------------------------------------------- */

static inline bool is_modifier_keycode(
    uint32_t keycode
) {
    return
        keycode >= HID_USAGE_KEY_KEYBOARD_LEFTCONTROL &&
        keycode <= HID_USAGE_KEY_KEYBOARD_RIGHT_GUI;
}

static inline uint32_t encode_key(
    uint8_t mods,
    uint8_t usage_page,
    uint8_t keycode
) {
    return
        ((uint32_t)mods << 24) |
        ((uint32_t)usage_page << 16) |
        (uint32_t)keycode;
}

static inline uint32_t encoded_from_event(
    const struct zmk_keycode_state_changed *ev
) {
    uint8_t mods =
        ev->implicit_modifiers |
        ev->explicit_modifiers;

    return encode_key(
        mods,
        ev->usage_page,
        ev->keycode
    );
}

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

/* -------------------------------------------------------------------------- */
/* Binding encoding                                                           */
/* -------------------------------------------------------------------------- */

static uint32_t binding_to_encoded(
    const struct zmk_behavior_binding *binding
) {
    /*
     * Assumes &kp-compatible bindings.
     *
     * param1 layout:
     * bits 0-7   = keycode
     * bits 8-15  = modifiers
     */

    uint32_t kp =
        binding->param1;

    uint8_t keycode =
        kp & 0xFF;

    uint8_t mods =
        (kp >> 8) & 0xFF;

    return encode_key(
        mods,
        HID_USAGE_KEY,
        keycode
    );
}

/* -------------------------------------------------------------------------- */
/* Matching                                                                   */
/* -------------------------------------------------------------------------- */

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
        seq->encoded,
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

        if (pos >=
            matches[0]->binding_len) {

            return pos;
        }

        uint32_t value =
            matches[0]->encoded[pos];

        for (uint8_t i = 1;
             i < count;
             i++) {

            if (pos >=
                matches[i]->binding_len) {

                return pos;
            }

            if (matches[i]->encoded[pos] !=
                value) {

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

    int64_t now =
        k_uptime_get();

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

            struct autocomplete_sequence *seq =
                &cfg->sequences[i];

            if (sequence_matches(
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

static int emit_completion(
    struct autocomplete_data *data,
    const struct autocomplete_sequence *seq,
    uint8_t start,
    uint8_t end,
    struct zmk_behavior_binding_event event
) {
    int ret;

    for (uint8_t i = start;
         i < end;
         i++) {

        const struct zmk_behavior_binding *binding =
            &seq->bindings[i];

        ret =
            zmk_behavior_invoke_binding(
                binding,
                event,
                true
            );

        if (ret < 0) {
            return ret;
        }

        ret =
            zmk_behavior_invoke_binding(
                binding,
                event,
                false
            );

        if (ret < 0) {
            return ret;
        }

        history_push(
            data,
            seq->encoded[i]
        );
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

    if (ev->usage_page !=
        HID_USAGE_KEY) {

        return ZMK_EV_EVENT_BUBBLE;
    }

    if (is_modifier_keycode(
            ev->keycode
        )) {

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
/* Init                                                                       */
/* -------------------------------------------------------------------------- */

static int autocomplete_init(
    const struct device *dev
) {
    struct autocomplete_config *cfg =
        (struct autocomplete_config *)
            dev->config;

    for (uint8_t i = 0;
         i < cfg->sequence_count;
         i++) {

        struct autocomplete_sequence *seq =
            &cfg->sequences[i];

        for (uint8_t j = 0;
             j < seq->binding_len;
             j++) {

            seq->encoded[j] =
                binding_to_encoded(
                    &seq->bindings[j]
                );
        }
    }

    return 0;
}

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

    if (shared_len <=
        history_len) {

        return 0;
    }

    data->firing = true;

    int ret =
        emit_completion(
            data,
            matches[0],
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
/* DT Helpers                                                                 */
/* -------------------------------------------------------------------------- */

#define AC_BINDING_ENTRY(node_id, idx)                 \
    {                                                  \
        .behavior_dev = DEVICE_DT_NAME(                \
            DT_PHANDLE_BY_IDX(node_id, bindings, idx)  \
        ),                                             \
                                                       \
        .param1 =                                      \
            DT_PHA_BY_IDX_OR(                          \
                node_id,                               \
                bindings,                              \
                idx,                                   \
                param1,                                \
                0                                      \
            ),                                         \
                                                       \
        .param2 =                                      \
            DT_PHA_BY_IDX_OR(                          \
                node_id,                               \
                bindings,                              \
                idx,                                   \
                param2,                                \
                0                                      \
            ),                                         \
    }

#define AC_BINDINGS_1(node_id) \
    AC_BINDING_ENTRY(node_id, 0)

#define AC_BINDINGS_2(node_id) \
    AC_BINDINGS_1(node_id),    \
    AC_BINDING_ENTRY(node_id, 1)

#define AC_BINDINGS_3(node_id) \
    AC_BINDINGS_2(node_id),    \
    AC_BINDING_ENTRY(node_id, 2)

#define AC_BINDINGS_4(node_id) \
    AC_BINDINGS_3(node_id),    \
    AC_BINDING_ENTRY(node_id, 3)

#define AC_BINDINGS_5(node_id) \
    AC_BINDINGS_4(node_id),    \
    AC_BINDING_ENTRY(node_id, 4)

#define AC_BINDINGS_6(node_id) \
    AC_BINDINGS_5(node_id),    \
    AC_BINDING_ENTRY(node_id, 5)

#define AC_BINDINGS_7(node_id) \
    AC_BINDINGS_6(node_id),    \
    AC_BINDING_ENTRY(node_id, 6)

#define AC_BINDINGS_8(node_id) \
    AC_BINDINGS_7(node_id),    \
    AC_BINDING_ENTRY(node_id, 7)

#define AC_BINDINGS_9(node_id) \
    AC_BINDINGS_8(node_id),    \
    AC_BINDING_ENTRY(node_id, 8)

#define AC_CAT(a, b) a##b
#define AC_EVAL(a, b) AC_CAT(a, b)

#define AC_DECLARE_CHILD(child)                                \
                                                                \
    static const struct zmk_behavior_binding                   \
        ac_bindings_##child[] = {                              \
            AC_EVAL(                                            \
                AC_BINDINGS_,                                   \
                DT_PROP_LEN(child, bindings)                    \
            )(child)                                            \
    };                                                          \
                                                                \
    static struct autocomplete_sequence                        \
        ac_sequence_##child = {                                \
            .bindings =                                         \
                ac_bindings_##child,                            \
                                                                \
            .binding_len =                                      \
                ARRAY_SIZE(                                     \
                    ac_bindings_##child                         \
                ),                                              \
    };

DT_FOREACH_CHILD_STATUS_OKAY(
    DT_DRV_INST(0),
    AC_DECLARE_CHILD
)

#define AC_SEQUENCE_REF(child) \
    &ac_sequence_##child

static struct autocomplete_sequence *ac_sequences_0[] = {
    DT_FOREACH_CHILD_STATUS_OKAY_SEP(
        DT_DRV_INST(0),
        AC_SEQUENCE_REF,
        (,)
    )
};

/* -------------------------------------------------------------------------- */
/* Config                                                                     */
/* -------------------------------------------------------------------------- */

static struct autocomplete_config
    ac_cfg_0 = {

    .max_delay_ms =
        DT_INST_PROP(
            0,
            max_delay_ms
        ),

    .sequences =
        (struct autocomplete_sequence *)
            ac_sequences_0,

    .sequence_count =
        ARRAY_SIZE(
            ac_sequences_0
        ),
};

static struct autocomplete_data
    ac_data_0;

/* -------------------------------------------------------------------------- */
/* Device                                                                     */
/* -------------------------------------------------------------------------- */

BEHAVIOR_DT_INST_DEFINE(
    0,
    autocomplete_init,
    NULL,
    &ac_data_0,
    &ac_cfg_0,
    APPLICATION,
    CONFIG_KERNEL_INIT_PRIORITY_DEFAULT,
    &autocomplete_driver_api
);