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

#define AUTOCOMPLETE_HISTORY_SIZE CONFIG_ZMK_AUTOCOMPLETE_HISTORY_SIZE

/* -------------------------------------------------------------------------- */
/* DATA STRUCTURES                                                            */
/* -------------------------------------------------------------------------- */

struct autocomplete_sequence {
    const struct zmk_behavior_binding *bindings;
    uint8_t binding_len;
};

struct autocomplete_config {
    int32_t max_delay_ms;

    const struct autocomplete_sequence *sequences;
    uint8_t sequence_count;
};

struct autocomplete_data {
    struct zmk_behavior_binding history[AUTOCOMPLETE_HISTORY_SIZE];
    uint8_t head;
    uint8_t count;

    int64_t last_press_time;
    bool firing;
};

/* -------------------------------------------------------------------------- */
/* HISTORY                                                                    */
/* -------------------------------------------------------------------------- */

static void history_push(struct autocomplete_data *d,
                         const struct zmk_behavior_binding *b) {
    d->history[d->head] = *b;

    d->head = (d->head + 1) % AUTOCOMPLETE_HISTORY_SIZE;

    if (d->count < AUTOCOMPLETE_HISTORY_SIZE) {
        d->count++;
    }

    d->last_press_time = k_uptime_get();
}

static void history_pop(struct autocomplete_data *d) {
    if (d->count == 0) return;

    d->head = (d->head + AUTOCOMPLETE_HISTORY_SIZE - 1)
              % AUTOCOMPLETE_HISTORY_SIZE;

    d->count--;
}

static bool history_suffix(const struct autocomplete_data *d,
                           uint8_t len,
                           struct zmk_behavior_binding *out) {
    if (len == 0 || len > d->count) return false;

    int start =
        ((int)d->head - (int)len + AUTOCOMPLETE_HISTORY_SIZE)
        % AUTOCOMPLETE_HISTORY_SIZE;

    for (uint8_t i = 0; i < len; i++) {
        out[i] = d->history[(start + i) % AUTOCOMPLETE_HISTORY_SIZE];
    }

    return true;
}

/* -------------------------------------------------------------------------- */
/* MATCHING                                                                   */
/* -------------------------------------------------------------------------- */

static bool seq_matches(const struct autocomplete_sequence *seq,
                        const struct zmk_behavior_binding *hist,
                        uint8_t len) {

    for (int i = 0; i < len; i++) {
        if (seq->bindings[i].param1 != hist[i].param1) {
            return false;
        }
    }

    return true;
}

/* -------------------------------------------------------------------------- */
/* INIT                                                                       */
/* -------------------------------------------------------------------------- */

static int autocomplete_init(const struct device *dev) {
    ARG_UNUSED(dev);
    return 0;
}

/* -------------------------------------------------------------------------- */
/* PER INSTANCE DATA                                                         */
/* -------------------------------------------------------------------------- */

#define AUTOCOMPLETE_DATA(n) \
    static struct autocomplete_data autocomplete_data_##n;

DT_INST_FOREACH_STATUS_OKAY(AUTOCOMPLETE_DATA)

/* -------------------------------------------------------------------------- */
/* LISTENER                                                                   */
/* -------------------------------------------------------------------------- */

static struct autocomplete_data *active_data;

static int listener(const zmk_event_t *eh) {
    const struct zmk_keycode_state_changed *ev =
        as_zmk_keycode_state_changed(eh);

    if (!ev || ev->state) return ZMK_EV_EVENT_BUBBLE;
    if (ev->usage_page != HID_USAGE_KEY) return ZMK_EV_EVENT_BUBBLE;

    if (!active_data || active_data->firing) return ZMK_EV_EVENT_BUBBLE;

    const struct zmk_behavior_binding b = {
        .behavior_dev = NULL,
        .param1 = ev->keycode,
        .param2 = 0
    };

    /* backspace handling */
    if (ev->keycode == HID_USAGE_KEY_KEYBOARD_DELETE_BACKSPACE ||
        ev->keycode == HID_USAGE_KEY_KEYBOARD_DELETE_FORWARD) {
        history_pop(active_data);
        return ZMK_EV_EVENT_BUBBLE;
    }

    history_push(active_data, &b);

    return ZMK_EV_EVENT_BUBBLE;
}

ZMK_LISTENER(autocomplete, listener);
ZMK_SUBSCRIPTION(autocomplete, zmk_keycode_state_changed);

/* -------------------------------------------------------------------------- */
/* EMIT                                                                       */
/* -------------------------------------------------------------------------- */

static int emit_binding(const struct zmk_behavior_binding *b,
                        struct zmk_behavior_binding_event event) {

    int ret = zmk_behavior_invoke_binding(b, event, true);
    if (ret < 0) return ret;

    return zmk_behavior_invoke_binding(b, event, false);
}

/* -------------------------------------------------------------------------- */
/* BEHAVIOR                                                                   */
/* -------------------------------------------------------------------------- */

static int binding_pressed(struct zmk_behavior_binding *b,
                           struct zmk_behavior_binding_event e) {
    return 0;
}

static int binding_released(struct zmk_behavior_binding *b,
                            struct zmk_behavior_binding_event e) {

    const struct device *dev = device_get_binding(b->behavior_dev);
    const struct autocomplete_config *cfg = dev->config;
    struct autocomplete_data *d = dev->data;

    active_data = d;

    struct zmk_behavior_binding buf[AUTOCOMPLETE_HISTORY_SIZE];

    const struct autocomplete_sequence *matches[16];

    int match_count = 0;
    int best_len = 0;

    for (int len = d->count; len >= 1; len--) {

        if (!history_suffix(d, len, buf)) continue;

        match_count = 0;

        for (int i = 0; i < cfg->sequence_count; i++) {
            const struct autocomplete_sequence *seq =
                &cfg->sequences[i];

            if (seq_matches(seq, buf, len)) {
                matches[match_count++] = seq;
            }
        }

        if (match_count > 0) {
            best_len = len;
            break;
        }
    }

    if (match_count == 0) return 0;

    int continuation = 0;

    while (1) {
        int first = 1;
        uint32_t expected = 0;

        for (int i = 0; i < match_count; i++) {
            const struct autocomplete_sequence *s = matches[i];

            if (best_len + continuation >= s->binding_len)
                goto done;

            uint32_t v = s->bindings[best_len + continuation].param1;

            if (first) {
                expected = v;
                first = 0;
            } else if (v != expected) {
                goto done;
            }
        }

        continuation++;
    }

done:

    if (continuation == 0) return 0;

    d->firing = true;

    const struct autocomplete_sequence *s = matches[0];

    for (int i = 0; i < continuation; i++) {
        emit_binding(&s->bindings[best_len + i], e);
    }

    d->firing = false;

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
/* DT WIRING                                                                  */
/* -------------------------------------------------------------------------- */

#define AUTOCOMPLETE_BINDING_ENTRY(node_id, prop, idx) \
    {                                                  \
        .behavior_dev = DEVICE_DT_NAME(                \
            DT_PHANDLE_BY_IDX(node_id, prop, idx)      \
        ),                                             \
        .param1 = DT_PHA_BY_IDX_OR(                    \
            node_id, prop, idx, param1, 0              \
        ),                                             \
        .param2 = DT_PHA_BY_IDX_OR(                    \
            node_id, prop, idx, param2, 0              \
        ),                                             \
    },

#define AUTOCOMPLETE_CHILD_DECL(child)                 \
    static const struct zmk_behavior_binding           \
        autocomplete_bindings_##child[] = {           \
            DT_FOREACH_PROP_ELEM(                      \
                child,                                 \
                bindings,                              \
                AUTOCOMPLETE_BINDING_ENTRY             \
            )                                          \
    };

#define AUTOCOMPLETE_SEQ_INIT(child)                  \
    {                                                 \
        .bindings = autocomplete_bindings_##child,    \
        .binding_len = ARRAY_SIZE(                    \
            autocomplete_bindings_##child             \
        ),                                            \
    },

#define AUTOCOMPLETE_DATA(n) \
    static struct autocomplete_data autocomplete_data_##n;

DT_INST_FOREACH_STATUS_OKAY(AUTOCOMPLETE_DATA)

#define AUTOCOMPLETE_INST(n)                          \
                                                      \
    DT_FOREACH_CHILD(                                 \
        DT_DRV_INST(n),                               \
        AUTOCOMPLETE_CHILD_DECL                       \
    )                                                 \
                                                      \
    static const struct autocomplete_sequence         \
        seqs_##n[] = {                                \
            DT_FOREACH_CHILD(                         \
                DT_DRV_INST(n),                       \
                AUTOCOMPLETE_SEQ_INIT                 \
            )                                         \
    };                                                \
                                                      \
    static const struct autocomplete_config           \
        cfg_##n = {                                   \
            .max_delay_ms = DT_INST_PROP(             \
                n,                                    \
                max_delay_ms                          \
            ),                                        \
            .sequences = seqs_##n,                    \
            .sequence_count = ARRAY_SIZE(seqs_##n),   \
    };                                                \
                                                      \
    BEHAVIOR_DT_INST_DEFINE(                          \
        n,                                            \
        autocomplete_init,                            \
        NULL,                                         \
        &autocomplete_data_##n,                       \
        &cfg_##n,                                     \
        APPLICATION,                                  \
        CONFIG_KERNEL_INIT_PRIORITY_DEFAULT,          \
        &api                                          \
    );

DT_INST_FOREACH_STATUS_OKAY(AUTOCOMPLETE_INST);