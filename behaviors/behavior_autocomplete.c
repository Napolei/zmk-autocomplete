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

LOG_MODULE_DECLARE(zmk, CONFIG_ZMK_LOG_LEVEL);

#define AUTOCOMPLETE_HISTORY_SIZE \
    CONFIG_ZMK_AUTOCOMPLETE_HISTORY_SIZE

/* -------------------------------------------------------------------------- */
/* DATA                                                                       */
/* -------------------------------------------------------------------------- */

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
    uint32_t history[AUTOCOMPLETE_HISTORY_SIZE];

    uint8_t head;
    uint8_t count;

    int64_t last_press_time;

    bool firing;
};

/* -------------------------------------------------------------------------- */
/* HISTORY                                                                    */
/* -------------------------------------------------------------------------- */

static void history_push(
    struct autocomplete_data *data,
    uint32_t keycode
) {
    data->history[data->head] = keycode;

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
         AUTOCOMPLETE_HISTORY_SIZE - 1) %
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

/* -------------------------------------------------------------------------- */
/* HELPERS                                                                    */
/* -------------------------------------------------------------------------- */

static bool is_backspace(uint32_t keycode) {
    return keycode ==
        HID_USAGE_KEY_KEYBOARD_DELETE_BACKSPACE;
}

static bool seq_matches(
    const struct autocomplete_sequence *seq,
    const uint32_t *history,
    uint8_t len
) {
    for (uint8_t i = 0; i < len; i++) {

        if (seq->bindings[i] != history[i]) {
            return false;
        }
    }

    return true;
}

/* -------------------------------------------------------------------------- */
/* INIT                                                                       */
/* -------------------------------------------------------------------------- */

static int autocomplete_init(
    const struct device *dev
) {
    ARG_UNUSED(dev);
    return 0;
}

/* -------------------------------------------------------------------------- */
/* DEVICE DATA                                                                */
/* -------------------------------------------------------------------------- */

#define AUTOCOMPLETE_DATA(n) \
    static struct autocomplete_data autocomplete_data_##n;

DT_INST_FOREACH_STATUS_OKAY(
    AUTOCOMPLETE_DATA
)

/* -------------------------------------------------------------------------- */
/* LISTENER                                                                   */
/* -------------------------------------------------------------------------- */

static struct autocomplete_data *active_data;

static int autocomplete_listener(
    const zmk_event_t *eh
) {
    const struct zmk_keycode_state_changed *ev =
        as_zmk_keycode_state_changed(eh);

    if (!ev) {
        return ZMK_EV_EVENT_BUBBLE;
    }

    /* only releases */
    if (ev->state) {
        return ZMK_EV_EVENT_BUBBLE;
    }

    if (ev->usage_page != HID_USAGE_KEY) {
        return ZMK_EV_EVENT_BUBBLE;
    }

    if (!active_data ||
        active_data->firing) {

        return ZMK_EV_EVENT_BUBBLE;
    }

    if (is_backspace(ev->keycode)) {

        history_pop(active_data);

        return ZMK_EV_EVENT_BUBBLE;
    }

    history_push(
        active_data,
        ev->keycode
    );

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
/* EMIT                                                                       */
/* -------------------------------------------------------------------------- */

static int emit_keycode(
    uint32_t keycode,
    struct zmk_behavior_binding_event event
) {
    struct zmk_behavior_binding binding = {
        .behavior_dev = "KEY_PRESS",
        .param1 = keycode,
        .param2 = 0,
    };

    int ret =
        zmk_behavior_invoke_binding(
            &binding,
            event,
            true
        );

    if (ret < 0) {
        return ret;
    }

    return zmk_behavior_invoke_binding(
        &binding,
        event,
        false
    );
}

/* -------------------------------------------------------------------------- */
/* BEHAVIOR                                                                   */
/* -------------------------------------------------------------------------- */

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
        device_get_binding(
            binding->behavior_dev
        );

    const struct autocomplete_config *cfg =
        dev->config;

    struct autocomplete_data *data =
        dev->data;

    active_data = data;

    uint32_t window[
        AUTOCOMPLETE_HISTORY_SIZE
    ];

    const struct autocomplete_sequence *matches[16];

    int match_count = 0;
    int best_len = 0;

    for (int len = data->count;
         len >= 1;
         len--) {

        if (!history_suffix(
                data,
                len,
                window
            )) {

            continue;
        }

        match_count = 0;

        for (int i = 0;
             i < cfg->sequence_count;
             i++) {

            const struct autocomplete_sequence *seq =
                &cfg->sequences[i];

            if (seq->binding_len < len) {
                continue;
            }

            if (seq_matches(
                    seq,
                    window,
                    len
                )) {

                matches[match_count++] = seq;
            }
        }

        if (match_count > 0) {
            best_len = len;
            break;
        }
    }

    if (match_count == 0) {
        return 0;
    }

    int continuation = 0;

    while (1) {

        bool first = true;
        uint32_t expected = 0;

        for (int i = 0;
             i < match_count;
             i++) {

            const struct autocomplete_sequence *s =
                matches[i];

            if ((best_len + continuation) >=
                s->binding_len) {

                goto done;
            }

            uint32_t v =
                s->bindings[
                    best_len + continuation
                ];

            if (first) {

                expected = v;
                first = false;

            } else if (v != expected) {

                goto done;
            }
        }

        continuation++;
    }

done:

    if (continuation == 0) {
        return 0;
    }

    data->firing = true;

    const struct autocomplete_sequence *s =
        matches[0];

    for (int i = 0;
         i < continuation;
         i++) {

        uint32_t keycode =
            s->bindings[
                best_len + i
            ];

        emit_keycode(
            keycode,
            event
        );

        history_push(
            data,
            keycode
        );
    }

    data->firing = false;

    return 0;
}

/* -------------------------------------------------------------------------- */
/* DRIVER API                                                                 */
/* -------------------------------------------------------------------------- */

static const struct behavior_driver_api api = {
    .binding_pressed = binding_pressed,
    .binding_released = binding_released,
};

/* -------------------------------------------------------------------------- */
/* DT                                                                         */
/* -------------------------------------------------------------------------- */

#define AUTOCOMPLETE_CHILD_DECL(child) \
    static const uint32_t              \
        autocomplete_bindings_##child[] = \
            DT_PROP(child, bindings);

#define AUTOCOMPLETE_SEQ_INIT(child) \
    {                                \
        .bindings =                  \
            autocomplete_bindings_##child, \
        .binding_len =               \
            ARRAY_SIZE(              \
                autocomplete_bindings_##child \
            ),                       \
    },

#define AUTOCOMPLETE_INST(n)                      \
                                                    \
    DT_FOREACH_CHILD(                              \
        DT_DRV_INST(n),                            \
        AUTOCOMPLETE_CHILD_DECL                    \
    )                                              \
                                                    \
    static const                                   \
        struct autocomplete_sequence               \
        seqs_##n[] = {                             \
                                                    \
            DT_FOREACH_CHILD(                      \
                DT_DRV_INST(n),                    \
                AUTOCOMPLETE_SEQ_INIT              \
            )                                      \
    };                                             \
                                                    \
    static const                                   \
        struct autocomplete_config                 \
        cfg_##n = {                                \
                                                    \
            .max_delay_ms =                        \
                DT_INST_PROP(                      \
                    n,                             \
                    max_delay_ms                   \
                ),                                 \
                                                    \
            .sequences =                           \
                seqs_##n,                          \
                                                    \
            .sequence_count =                      \
                ARRAY_SIZE(seqs_##n),              \
    };                                             \
                                                    \
    BEHAVIOR_DT_INST_DEFINE(                       \
        n,                                         \
        autocomplete_init,                         \
        NULL,                                      \
        &autocomplete_data_##n,                    \
        &cfg_##n,                                  \
        APPLICATION,                               \
        CONFIG_KERNEL_INIT_PRIORITY_DEFAULT,       \
        &api                                       \
    );

DT_INST_FOREACH_STATUS_OKAY(
    AUTOCOMPLETE_INST
)