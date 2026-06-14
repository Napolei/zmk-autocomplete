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
    const uint32_t *bindings;
    uint32_t semantic[64];
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
/* SEMANTIC ENCODING                                                         */
/* -------------------------------------------------------------------------- */

static inline uint32_t encode_semantic(uint8_t mods, uint16_t usage_page, uint16_t keycode) {
    return ((uint32_t)mods << 24) | ((uint32_t)usage_page << 16) | keycode;
}

static inline uint32_t encoded_from_event(const struct zmk_keycode_state_changed *ev) {
    uint8_t mods = ev->implicit_modifiers | ev->explicit_modifiers;
    return encode_semantic(mods, ev->usage_page, ev->keycode);
}

static inline uint32_t binding_to_semantic(uint32_t keycode) {
    return encode_semantic(0, HID_USAGE_KEY, keycode);
}

/* -------------------------------------------------------------------------- */
/* HISTORY                                                                    */
/* -------------------------------------------------------------------------- */

static void history_push(struct autocomplete_data *d, uint32_t v) {
    d->history[d->head] = v;
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
                           uint32_t *out) {
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
                        const uint32_t *hist,
                        uint8_t len) {
    if (len > seq->binding_len) return false;

    return memcmp(hist, seq->semantic, len * sizeof(uint32_t)) == 0;
}

/* -------------------------------------------------------------------------- */
/* INIT                                                                       */
/* -------------------------------------------------------------------------- */

static int autocomplete_init(const struct device *dev) {
    struct autocomplete_config *cfg = dev->config;

    for (int i = 0; i < cfg->sequence_count; i++) {
        struct autocomplete_sequence *seq =
            (struct autocomplete_sequence *)&cfg->sequences[i];

        for (int j = 0; j < seq->binding_len; j++) {
            seq->semantic[j] = binding_to_semantic(seq->bindings[j]);
        }
    }

    return 0;
}

/* -------------------------------------------------------------------------- */
/* PER INSTANCE DATA                                                          */
/* -------------------------------------------------------------------------- */

#define AUTOCOMPLETE_DATA(n) \
    static struct autocomplete_data autocomplete_data_##n;

/* -------------------------------------------------------------------------- */
/* LISTENER                                                                   */
/* -------------------------------------------------------------------------- */

static struct autocomplete_data *active_data;

static int listener(const zmk_event_t *eh) {
    const struct zmk_keycode_state_changed *ev =
        as_zmk_keycode_state_changed(eh);

    if (!ev || ev->state) return ZMK_EV_EVENT_BUBBLE;
    if (ev->usage_page != HID_USAGE_KEY) return ZMK_EV_EVENT_BUBBLE;

    uint32_t keycode = ev->keycode;
    uint32_t encoded = encoded_from_event(ev);

    if (!active_data || active_data->firing) {
        return ZMK_EV_EVENT_BUBBLE;
    }

    if (keycode == HID_USAGE_KEY_KEYBOARD_DELETE_BACKSPACE ||
        keycode == HID_USAGE_KEY_KEYBOARD_DELETE_FORWARD) {

        history_pop(active_data);
        return ZMK_EV_EVENT_BUBBLE;
    }

    if (keycode >= HID_USAGE_KEY_KEYBOARD_LEFTCONTROL &&
        keycode <= HID_USAGE_KEY_KEYBOARD_RIGHT_GUI) {
        return ZMK_EV_EVENT_BUBBLE;
    }

    history_push(active_data, encoded);

    return ZMK_EV_EVENT_BUBBLE;
}

ZMK_LISTENER(autocomplete, listener);
ZMK_SUBSCRIPTION(autocomplete, zmk_keycode_state_changed);

/* -------------------------------------------------------------------------- */
/* EMIT (IMPORTANT FIX)                                                       */
/* -------------------------------------------------------------------------- */

static int emit_binding(const struct zmk_behavior_binding *b,
                        struct zmk_behavior_binding_event event) {

    int ret = zmk_behavior_invoke_binding(b, event, true);
    if (ret < 0) return ret;

    return zmk_behavior_invoke_binding(b, event, false);
}

/* -------------------------------------------------------------------------- */
/* BEHAVIOR CALLBACKS                                                         */
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

    const struct autocomplete_sequence *matches[16];
    uint32_t buf[AUTOCOMPLETE_HISTORY_SIZE];

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
        uint32_t expected;
        int first = 1;

        for (int i = 0; i < match_count; i++) {
            const struct autocomplete_sequence *s = matches[i];

            if (best_len + continuation >= s->binding_len)
                goto done;

            uint32_t v = s->semantic[best_len + continuation];

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
/* DRIVER API                                                                */
/* -------------------------------------------------------------------------- */

static const struct behavior_driver_api api = {
    .binding_pressed = binding_pressed,
    .binding_released = binding_released,
};

/* -------------------------------------------------------------------------- */
/* DT WIRING                                                                  */
/* -------------------------------------------------------------------------- */

#define AUTOCOMPLETE_CHILD(child) \
    static const uint32_t child##_bind[] = DT_PROP(child, bindings);

#define AUTOCOMPLETE_SEQ(child) \
    { .bindings = child##_bind, .binding_len = ARRAY_SIZE(child##_bind) },

#define AUTOCOMPLETE_INST(n)                                         \
    DT_FOREACH_CHILD(DT_DRV_INST(n), AUTOCOMPLETE_CHILD)            \
    static struct autocomplete_sequence seqs_##n[] = {              \
        DT_FOREACH_CHILD(DT_DRV_INST(n), AUTOCOMPLETE_SEQ)          \
    };                                                              \
    static struct autocomplete_config cfg_##n = {                   \
        .max_delay_ms = DT_INST_PROP(n, max_delay_ms),              \
        .sequences = seqs_##n,                                      \
        .sequence_count = ARRAY_SIZE(seqs_##n),                     \
    };                                                              \
    BEHAVIOR_DT_INST_DEFINE(n, autocomplete_init, NULL,             \
        &autocomplete_data_##n, &cfg_##n, APPLICATION,              \
        CONFIG_KERNEL_INIT_PRIORITY_DEFAULT, &api);

DT_INST_FOREACH_STATUS_OKAY(AUTOCOMPLETE_INST);