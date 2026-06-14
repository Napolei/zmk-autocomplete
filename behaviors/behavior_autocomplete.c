#define DT_DRV_COMPAT zmk_behavior_autocomplete

#include <zephyr/device.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

#include <zmk/event_manager.h>
#include <zmk/events/keycode_state_changed.h>
#include <zmk/behavior.h>
#include <zmk/behavior_queue.h>

LOG_MODULE_DECLARE(zmk, CONFIG_ZMK_LOG_LEVEL);

/* ---------------- CONFIG ---------------- */

struct autocomplete_sequence {
    const struct zmk_behavior_binding *bindings;
    size_t binding_len;
};

struct autocomplete_config {
    uint32_t max_delay_ms;
    struct autocomplete_sequence *sequences;
    size_t sequence_count;
};

/* ---------------- RUNTIME STATE ---------------- */

struct autocomplete_data {
    uint32_t history[32];
    uint8_t len;
};

static struct autocomplete_data state;

/* ---------------- HISTORY ---------------- */

static void push(uint32_t v) {
    if (state.len < 32) {
        state.history[state.len++] = v;
    } else {
        memmove(&state.history[0], &state.history[1], (31 * sizeof(uint32_t)));
        state.history[31] = v;
    }
}

/* ---------------- MATCH ---------------- */

static bool match_suffix(const uint32_t *seq, size_t seq_len) {
    if (seq_len > state.len) return false;

    for (size_t i = 0; i < seq_len; i++) {
        if (state.history[state.len - seq_len + i] != seq[i]) {
            return false;
        }
    }
    return true;
}

/* ---------------- EVENT LISTENER ---------------- */

static int listener(const zmk_event_t *eh) {
    const struct zmk_keycode_state_changed *ev =
        as_zmk_keycode_state_changed(eh);

    if (!ev) return ZMK_EV_EVENT_BUBBLE;

    /* ONLY key DOWN events */
    if (!ev->state) {
        return ZMK_EV_EVENT_BUBBLE;
    }

    /* ignore modifiers */
    if (ev->usage_page != 0x07) {
        return ZMK_EV_EVENT_BUBBLE;
    }

    push(ev->keycode);

    return ZMK_EV_EVENT_BUBBLE;
}

ZMK_LISTENER(autocomplete, listener);
ZMK_SUBSCRIPTION(autocomplete, zmk_keycode_state_changed);

/* ---------------- EXECUTE ---------------- */

static int bind_exec(const struct zmk_behavior_binding *b,
                     struct zmk_behavior_binding_event event,
                     bool pressed) {
    return zmk_behavior_invoke_binding(b, event, pressed);
}

/* ---------------- MAIN BEHAVIOR ---------------- */

static int pressed(struct zmk_behavior_binding *binding,
                   struct zmk_behavior_binding_event event) {
    return 0;
}

static int released(struct zmk_behavior_binding *binding,
                    struct zmk_behavior_binding_event event) {

    const struct device *dev = device_get_binding(binding->behavior_dev);
    const struct autocomplete_config *cfg = dev->config;

    for (size_t s = 0; s < cfg->sequence_count; s++) {
        struct autocomplete_sequence *seq = &cfg->sequences[s];

        if (!match_suffix((const uint32_t *)seq->bindings, seq->binding_len)) {
            continue;
        }

        /* emit full sequence */
        for (size_t i = 0; i < seq->binding_len; i++) {
            bind_exec(&seq->bindings[i], event, true);
            bind_exec(&seq->bindings[i], event, false);
        }

        break;
    }

    return 0;
}

static const struct behavior_driver_api api = {
    .binding_pressed = pressed,
    .binding_released = released,
};

static int init(const struct device *dev) {
    return 0;
}

/* ---------------- DEVICE DECLARATION ---------------- */

#define SEQ_CHILD(child) \
    static const struct zmk_behavior_binding bindings_##child[] = \
        DT_PROP(child, bindings);

#define SEQ_INIT(child) \
    { .bindings = bindings_##child, \
      .binding_len = DT_PROP_LEN(child, bindings) },

#define AUTOCOMPLETE_INST(n) \
    DT_FOREACH_CHILD(DT_DRV_INST(n), SEQ_CHILD) \
    static struct autocomplete_sequence sequences_##n[] = { \
        DT_FOREACH_CHILD(DT_DRV_INST(n), SEQ_INIT) \
    }; \
    static struct autocomplete_config config_##n = { \
        .max_delay_ms = DT_INST_PROP(n, max_delay_ms), \
        .sequences = sequences_##n, \
        .sequence_count = ARRAY_SIZE(sequences_##n), \
    }; \
    static struct device_data data_##n; \
    BEHAVIOR_DT_INST_DEFINE(n, init, NULL, &data_##n, &config_##n, \
        APPLICATION, CONFIG_KERNEL_INIT_PRIORITY_DEFAULT, &api);

DT_INST_FOREACH_STATUS_OKAY(AUTOCOMPLETE_INST);